# Integrating the BEST PAIRS workflow into the `wf` tool

Big-picture notes on reducing the human bookkeeping in
`docs/best-pairs-workflow-v2.md`. Based on reading v2, the review doc, and the
`wf` source (`words/workflow/{config,context,eval,complete,names,show}.py` and
the `.wf` layout under `words/final`). No changes proposed to existing
behaviour yet; this is the design discussion.

## The diagnosis

`wf` models **a batch of pairs moving through classification**. Its nouns are
phase (p1/p2/p3), bundle, done-set. That model is sound and the primitives are
good — `submit`/`eval`/`complete` as a three-beat lifecycle, `Context` carrying
root+phase+bundle, the steps list in `complete.py` being the only per-phase
difference. None of it needs rework.

But the thing actually being executed has a different noun: **a
(sentence × segment-count) target**, refined over iterations. That object
exists nowhere. Its state lives in three places, none queryable:

- **A 20-line shell prologue** (`SENTENCE`, `LETTERS`, `MIN_WORD_LENGTH`,
  `SEGMENTS`, `P1_MIN`, `DFS_RESULTS`, `TOP_COUNT`, plus `$IDX`, `$DICT`, and
  `${S6:0:N}` from `setup.sh`). Every downstream command interpolates it.
  Re-establishing it correctly is the largest per-session burden, and the
  failure mode is silent: a right filename with a wrong `-g`.
- **Filenames.** `names.py` says artifact names are "rendered, never parsed" —
  right discipline for the tool, but it makes the *human* the parser. "What
  have I done for s2?" is answered by `ls results/s2` and squinting at
  `dfs.s2.idx2.m4.x2.g4.85.15.1000000`.
- **The operator's head**, for everything filenames don't carry: which
  iteration, whether a given DFS predates the last hard-NO addition, which
  segment counts were intended but never started.

The classification workflow is managed; the *production* workflow around it
isn't. Closing that gap is mostly additive.

## Proposed shape

**Two new records, not one.** Stages 1–2 (candidates, P1, seed) are
per-sentence and effectively one-shot. Stages 3–7 repeat per segment count —
v2 says exactly this ("Repeat stages 3 through 7"). Modeling the split is what
stops the seed being re-derived for every `-g`:

- a **sentence** record: letters, `-m`, `-x`, index, dict, candidate file, P1
  chunk progress, seed path;
- **run** records under it, one per segment count: `-g`, `DFS_RESULTS`,
  `TOP_COUNT`, P1 band, iteration counter, and each stage's artifact.

Create with `wf run new s2 -g 4`, inheriting sentence-level params. **Letters
get recorded at creation, not re-sourced** — `source ./setup.sh` +
`${S6:0:N}` is a hidden dependency that should be resolved once and frozen into
the record, so a run stays reproducible after `setup.sh` drifts.

Once the record exists, every parameter in the v2 prologue leaves the command
line. The tool renders filenames (same contract as today — they just stop being
typed, which is where the errors are) and knows the paths.

## The three commands that carry the load

**`wf status`** — the queued / in-progress / done repository. Two levels: a
table across all runs (sentence, `g`, current stage, blocker, age), and a
per-run detail view showing each stage as done/pending/stale with its artifact.
This is where "where am I with s2 g5" gets a real answer.

**`wf next`** — determines the next incomplete stage for a run, prints the
exact command, runs it. For human-boundary stages it prints instructions and
stops. This addresses the "commands I have to remember" problem directly: the
answer becomes one command, and the v2 recipe becomes the tool's internal
transition table instead of a document followed by hand.

**Staleness tracking** — the quietly valuable one. Record each stage's input
fingerprints. When `.wf/classified/no/no.pairs` grows, `wf status` marks the
provisional DFS and everything downstream stale. That removes an entire class
of "did I rerun that after adding the hard-NOs?" doubt, which in a
multi-iteration loop is a constant low-grade tax.

## The one genuinely new primitive

Stages 4–5 hand over a judgment call with no support: "Inspect the distribution
before submitting it. If common poor entries crowd out the useful sample…" The
raw frequency list arrives unannotated, prior rulings must be recalled from
memory, and hard-NO entry means hand-writing a `.pairs` file.

**An annotated ranked view.** Join `top-segments --pairs` output against the P2
done-set and the hard-NO aggregate, so each line carries frequency, cumulative
share, and prior verdict (confirmed YES / soft NO / hard NO / never seen). Two
things fall out:

1. "Is a bad segment dominating?" becomes visible rather than remembered — e.g.
   three hard-NO entries visibly account for 40% of the sample.
2. Hard-NO entry happens *from* that view instead of by composing a file by
   hand, feeding straight into `wf classify no`.

This is also where the cross-run leverage lives: once s2/g4 establishes a
segment is bad, s2/g5's ranked list shows it pre-marked.

## Secondary items

- **Long-running steps.** Run DFS detached with a log so `wf status` can report
  "dfs g4 iter 2, running 14m". That gives the concurrency check in `CLAUDE.md`
  a natural home — the tool can warn or refuse when another `dfs-anagrams` or
  `query-index` is live, instead of relying on a remembered `pgrep`.
- **The evalpair gap.** `wf eval p1` prepares a bundle and stops; the operator
  then runs a variant of `juniper.sh` / `mini.sh` with hand-edited `-r` and
  `--pair-file` paths pointing into the bundle. The bundle dir is exactly where
  the JSONL must land, so the tool already knows both arguments. Recording the
  evaluator command per sentence and having `wf eval p1` offer to invoke it
  closes the last hand-edited path in stage 1.
- **Reproducibility moves from the doc to a log.** If the tool builds the
  command lines, append every invocation verbatim to a per-run log. The v2 doc
  then shrinks to explaining *why* the stages exist, and the log records what
  actually ran — strictly better than a doc that can drift from practice.

## Open questions

1. **Where should the run registry live?** Artifacts are split across two repos
   — `nutrimatic/idx`, `nutrimatic/results`, and `words/final/.wf`. Preference
   is `words/final/.wf/runs/` (already the state directory, already owned by
   `wf`) with absolute paths into nutrimatic — but that makes `wf` the owner of
   nutrimatic outputs, which may not match the intended boundary.

2. **Should `wf` execute the nutrimatic binaries, or only emit the commands?**
   Executing gives `wf next`, status, and staleness for free. Emitting (print a
   command, pipe it to `sh`) keeps the repos decoupled and keeps the operator in
   control of every long run, but leaves the copy-paste. Leaning toward
   executing with the log preserving reproducibility. This is the main
   architectural fork.

3. **Should iterations be preserved?** v2 says they need not be. With an
   iteration counter in the record, keeping them costs almost nothing and gives
   a comparison trail across refinements.

4. **Is P1 genuinely per-sentence and done-once in practice**, or do candidate
   increments (larger `-x`, different `-m`) get added mid-stream? This decides
   whether the sentence record needs its own progress state machine or can be a
   params bag with a "seed exists" flag.

5. **How many (sentence, segment-count) runs are live at once?** A handful means
   `wf status` is a simple table and `wf next` takes an explicit run name.
   Dozens means a current-run pointer (`wf use s2.g4`) and cross-run
   scheduling — a meaningfully bigger tool.
