# Review: plans/integrate-best-pairs.md

Read against the plan itself, `docs/best-pairs-workflow-v2.md`, the three
findings it supersedes, and the `wf` source it builds on
(`/home/mike/code/words/workflow/*`, `/home/mike/code/words/wf`).

The design is coherent and the reasoning about derived-versus-stored state is
sound; the intersection argument for BEST PAIRS is the strongest part of the
document. Nothing below argues against the core. Items 1-5 change what gets
built.

## Blocking -- the design does not work as written

### 1. `best.pairs = common(top.segments, yes.pairs)` is invalid: `top.segments` is not a sorted set

`top-segments` sorts by descending count, ties broken by text
(`source/top-segments.cpp:89-93`). That ordering is load-bearing -- `head -n N`
is what makes the file "top N". But `setops.common` is `comm -12`
(`setops.py:96`), which requires both inputs collated. `comm` emits a wrong
intersection silently; it does not error.

The plan's claim that the intersection is "a pure function of two files" costing
milliseconds still holds, but the `comm` input has to be a sorted copy. Decide
whether `top.segments` is stored ranked (and sorted on the fly for the
intersection) or stored sorted (with the ranking discarded after `head`).
Everything under "BEST PAIRS is an intersection" depends on this.

### 2. `-r` cannot be "resolved against the invoking process's working directory"

`words/wf` changes directory before `exec`ing Python:

```sh
here="$(cd -- "$(dirname -- "$0")" && pwd)"; cd "$here"
exec "$here/../.torch/bin/python" -m workflow.wf "$@"
```

The caller's cwd is gone by the time argparse runs -- which is why v2 says
"absolute paths are intentional". A default of `results/` therefore resolves to
`/home/mike/code/words/results`, not nutrimatic's tree, and the stated
resolution rule needs a wrapper change (`export WF_PWD="$PWD"` before the `cd`)
that appears in no milestone or prerequisite. The same issue already affects
`-d`, which defaults to `Path.cwd()` (`wf.py:50`).

### 3. The tool cannot render the DFS filenames the plan shows

`results/s2/dfs.s2.idx2.m4.x2.g4.85.15.1000000` needs two components that exist
nowhere in `.wf/best`:

- `idx2`, the index generation. The layout calls `idx/wiki-merged.2.index`
  "opaque to the tool"; rendering `idx2` means parsing that filename, which
  contradicts both the opacity claim and `names.py`'s "rendered, never parsed".
- `85.15`, the P1 band -- a property of the hand-placed `seed.pairs`. Stages 1
  and 2 are out of scope, so nothing records it.

Either the tree stores these (a small params file, which "No history" forbids),
or the rendered names drop them and diverge from v2's canonical keys.
Undecided, and it determines the symlink target names in milestones 2 and 4.

### 4. The p2 bundle name is never defined

"The review gate" derives review state from "the bundle named by the run", but
`submit p2` names the bundle from the submitted file's basename
(`submit.py:36`, `names.queue_name`). Submitting `top.segments` yields the
bundle `top.segments` for every sentence and every `-g` -- an immediate
collision in `p2/queued`, `p2/eval`, and the `done/{in,out}` archives.

So a rendered bundle name is required (v2's `top.s2.m4.g4.1000`), and with it a
decision the plan does not make: **does the bundle name include the cutoff?**

- With the cutoff: raising it makes a new bundle, and the run's derived review
  state flips from "complete" back to "not submitted".
- Without it: re-review collides with archived artifacts, which is exactly why
  milestone 0 exists.

### 5. A DFS regeneration is not atomic in the common case

Rule 1 says the symlink rename is the single commit point and that a crash
leaves an orphan, never an ambiguity. But the rendered name is a pure function
of (sentence, `m`, `x`, `g`, band, `-n`), all unchanged across a refinement
round. The second `gen dfs.seed` therefore writes *the file the current symlink
already points at*. An interrupted run leaves the live symlink resolving to a
truncated DFS output whose mtime looks fresh.

v2 has the same overwrite (`> "$PROVISIONAL_DFS"`), but v2 never claimed
atomicity. Fix: write to a scratch name in the results directory and
`rename(2)` into the rendered name before the symlink swap.

## Prerequisites the plan does not list

### 6. Rule 3 is unenforceable for three of the four artifacts

"Enforced by writing every artifact whose mtime is read through `setops` with
`stable_mtime=True`":

- `setops.common(a, b, dst)` has **no `stable_mtime` parameter**
  (`setops.py:96`); only `merge` and `fold` do. `best.pairs` needs it. That is a
  signature change, unlisted.
- `top.segments` is a pipeline (`top-segments --pairs | head -n N`).
  `setops._place` takes one argv and no shell, so no existing primitive can
  produce it at all, let alone with a content compare. Note also that `head`
  closing the pipe gives `top-segments` SIGPIPE, so `check=True` needs care.
- `dfs.seed` and `dfs.best` are shell redirects published by symlink, and
  comparison is against the target's mtime, which is fresh by construction. A
  byte-identical DFS rerun therefore always marks `top.segments` stale. Cheap in
  practice, but "Every artifact in the tree obeys these" is false as stated.

### 7. `--exclude-pairs` must be conditional

The example command passes `--exclude-pairs WFROOT` unconditionally, but
`dfs-anagrams` treats a missing aggregate as an error by design ("a missing
`.wf` directory or missing aggregate must be an error, not an empty exclusion
set" -- v2), which is why v2 stage 7 guards with `test -f`. On a target with no
hard NOs yet, every `gen dfs.*` fails.

### 8. `wf init` must be re-run on existing roots

`config.path` calls `fs.raise_if_not_dir` at every level, so
`config.path(root, ["best"])` throws on `words/final` until `init` runs again.
`init.ensure_layout` is idempotent, so this is a note in milestone 1 rather than
work -- but a first-run failure otherwise.

## Omissions

9. **`-n` and `--top` have no stated defaults** (v2 uses 1000000 and 1000), and
   no stated per-stage validity. The plan argues at length that accepting `-f`
   where it means nothing "is a lie"; the same applies to `-n` on
   `gen top.segments` and `--top` on `gen dfs.seed`, and it is not mentioned.

10. **No selection rule for `idx/` and `dict/`.** Both are directories; the plan
    shows one entry each. "Exactly one, else a diagnostic" is presumably
    intended but unstated -- and `setup.sh` currently points `$IDX` at
    `wiki-merged.2.index` while `CLAUDE.md` says `.5`, so two indexes plausibly
    land there.

11. **The address grammar is undefined.** "Validating by shape rather than by
    name membership" never says what the shape is, and `wf best status` with no
    address walks `.wf/best/`, where `idx/` and `dict/` sit at the same level as
    `s2/`. Reserved names, and whether the `m` and `g` levels must match
    `m\d+` / `g\d+`, need one line.

12. **Nothing reports missing hand-placed inputs.** The freshness table has no
    row for `letters` or `seed.pairs`, yet `status` on a fresh target is exactly
    the case of "seed not placed yet". By the table's own logic `letters` is
    also a prerequisite of `dfs.seed` -- edit the bag and the DFS is stale.

13. **No tests at all.** Every other plan in `plans/` names its smoke tests, and
    `words/tests/` already has `wf_fixture.py` plus five `test_workflow_*.py`
    files. Even under the minimal-tests rule, the freshness derivation and the
    address walker are pure functions worth one test each.

14. **`wf best review` has no `--no-filter` pass-through.** The plan keeps
    `--no-filter` as "the deliberate way to reconsider" soft NOs, but the
    command surface has no flag for it and `review` is the only path to
    `eval p2`.

15. **The empty-review case is not handled.** Once the done-set filter drops
    everything -- likely on a modest cutoff bump -- `eval p2` opens a bundle,
    splits zero pairs, and creates zero notes (`eval.py:_split_pairs`,
    `_make_notes`), leaving an in-flight bundle that no review can complete.
    `best.pairs` needs no review in that case; `review` should say so and do
    nothing.

16. **Hard NOs can be re-queued for review.** They enter `classified/no` via
    `exclude` without ever passing through p2, so they are absent from
    `p2_done.pairs` and survive `bundle.filter_done`. In the intended flow
    `dfs.seed` is regenerated with `--exclude-pairs` first and they vanish;
    submitting a stale `top.segments` re-reviews them. Either subtract
    `classified/no` at submit, or state the reliance.

17. **Long-run mechanics are unspecified.** The output example implies `wf`
    blocks for `14m22s`. Foreground or detached? Where does `-p 10000000`
    progress go once stdout is redirected into the results file? What does
    Ctrl-C leave behind (see item 5)? And two `gen`s on one target write the
    same paths -- `CLAUDE.md` says concurrent sessions are normal.

18. **The review-state table's "done" row does not match the code.** There is no
    `p2/done/<bundle>/`: `p2_archive` scatters the bundle into
    `done/in/<bundle>.pairs`, `done/out/<bundle>.p2.yes`, and
    `done/out/enex/<bundle>/`, and `p2_advance` -> `bundle.finish` removes the
    eval directory. The derivation is fine; it just has to name the real probe,
    and note that the eval directory emptying is what separates "notes out" from
    "complete".

19. **Two proposals from the superseded findings vanish without a decision.**
    The annotated ranked view (frequency, cumulative share, prior verdict --
    called "the one genuinely new primitive" in
    `findings/integrate-pairs-workflow-claude.md`), and codex's distinction
    between a global hard NO and a run-local suppression. `exclude` composes
    `wf classify no`, so "this pair dominates *this* exploration" becomes a
    permanent global verdict. Both may be deliberate exclusions; neither appears
    in "Not in this plan".

## Smaller ambiguities

20. "`show.py` needs no change at all" -- true that nothing breaks, but
    registering `best` in `CONFIG_LAYOUT` makes `wf show best` a working command
    that lists the crown and is blind to the body, and `wf show best s2` a
    layout error. That is a half-truth the plan asserts away rather than
    accepts.

21. The printed command mixes unresolved shell variables (`"$IDX"`, `"$S2"`), a
    placeholder (`WFROOT`), and a relative path (`.wf/best/dict/words.big`)
    whose base is the words repo after the wrapper `cd`. If the point is
    reproducibility, decide: fully resolved absolute paths, or symbolic with a
    legend.

22. "The loop is running exactly when `classified/no/no.pairs` is newer than
    `dfs.seed`" inherits the global-aggregate falseness the plan concedes two
    paragraphs earlier -- any sentence's hard NO puts every sentence "in a
    loop".

23. `--top` breaks the "the flag you type is the flag that runs" rule that
    justifies `-g` and `-m`; it is `head -n`.

24. Dangling `dfs.*` symlink (target garbage-collected or hand-deleted):
    `Path.stat()` raises, `Path.exists()` reports False. Freshness should read
    it as "missing", and status should probably distinguish it.

25. Orphan `dfs.*` files in `results/` are declared "freely skippable" garbage
    with no owner and no command to find them.
