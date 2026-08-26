# Plan: integrate the BEST PAIRS workflow into `wf`

## Status

This is an implementation plan. No source changes have been made for it yet,
beyond the `stable_mtime` prerequisite noted below.

It supersedes `findings/integrate-pairs-workflow-codex.md`,
`findings/integrate-pairs-workflow-claude.md`, and
`findings/what-wf-remembers.md`, which remain as design history.

Status values are `[ ]` pending, `[-]` in progress, and `[x]` complete.

**Milestone**, not phase. In this document *phase* always means a `wf`
classification phase (`p1`, `p2`, `p3`), and *stage* means either one of v2's
recipe stages or one of `gen`'s stage names. The units of implementation order
below are milestones, and they have nothing to do with any of those.

Primitives first. Every milestone below is one operation on one target, doing
exactly what it is told. The collective operation that decides *which* stage to
run is not part of this plan; see "Not in this plan".

| Milestone | Deliverable | Status | Proposed commit |
|---:|---|:---:|---|
| 0 | `complete` preflights archive destinations | [ ] | `Preflight complete's archive destinations` |
| 1 | `.wf/best` layout, target accessor, freshness, `wf best status` | [ ] | `Add the BEST PAIRS state layout` |
| 2 | `gen dfs.seed`, `gen top.segments`, status after every operation | [ ] | `Run provisional DFS and top-segments from wf` |
| 3 | `exclude`, `review`, `complete`, `gen best.pairs` | [ ] | `Materialize BEST PAIRS from the confirmed-YES set` |
| 4 | `gen dfs.best` | [ ] | `Add the final DFS stage` |

Milestone 1 is one unit because `status` is what shapes the accessor: with no
registry, enumerating targets means walking the tree, so the accessor needs
"every target under this prefix" rather than "the path of this one target".
Building it against any other consumer would get that interface wrong.

There is no `wf best show`. A target directory holds the same four filenames
always, so listing it answers only which of the four exist -- and existence is
the first clause of the freshness check, so `status` computes it anyway and
prints strictly more. `wf show` earns its place on the phase directories
because their contents genuinely vary; a target's do not.

## Outcome

`docs/best-pairs-workflow-v2.md` describes a recipe whose scheduler, parameter
store, and status database is the operator. This plan makes stages 3 through 7
of that recipe a managed object: a **run**, keyed by sentence, minimum word
length, and exact segment count.

Stages 1 and 2 stay manual. Candidate generation, the `comm` against
`p1_done.pairs`, chunking, and the `evalpair` cycle remain as v2 documents
them; this plan begins with `seed.pairs` already extracted.

The scope boundary and the ownership boundary are the same line. Everything
stages 1 and 2 produce is placed by hand; everything stages 3 through 7 produce
is the tool's. So there is no registration command: the sentence directory, its
`letters` file, and `m4/seed.pairs` are created by the operator, exactly as the
index and dictionary links are.

What that buys is the disappearance of the v2 shell prologue from the command
line:

```text
wf best gen s2 -g 4 dfs.seed
```

`-x 2` is assumed throughout and is not a key. It stays in rendered filenames,
where it distinguishes historical artifacts, but the tool does not vary it.

## State layout

The workflow's own state lives under `.wf/best/`:

```text
.wf/best/
  idx/
    wiki-merged.2.index          # symlink, placed by hand; opaque to the tool
  dict/
    words.big                    # copy or symlink of tmp/words.big
  s2/
    letters                      # the letter bag, written once by hand
    m4/
      seed.pairs                 # the 85/15 P1 extract
      g4/
        dfs.seed -> RESULTS/dfs.s2.idx2.m4.x2.g4.85.15.1000000
        top.segments
        best.pairs
        dfs.best -> RESULTS/dfs.s2.idx2.m4.x2.g4.top1000.1000000
      g5/
        ...
```

Three levels, each holding exactly what is scoped to it:

- **Sentence** (`s2/`): the letter bag, and nothing else. `letters` is written
  once by hand and thereafter read from the file, never re-sourced from
  `setup.sh`, so a run stays reproducible after `setup.sh` drifts. The tool
  does not compare it against the environment: it is frozen precisely because
  the environment is not authoritative.
- **Candidate universe** (`m4/`): `seed.pairs` and anything else determined by
  `-m`. The seed's own v2 name (`pairs.s2.idx2.m4.x2.85.15.p1.yes`) says it is
  `m`-dependent, so it cannot live above this level. This level stays even
  while only `m4` is exercised: collapsing it into the sentence directory would
  put an `m`-dependent artifact at a level that does not key on `m`.
- **Segment count** (`g4/`): the four generated artifacts of stages 3 through 7.

The index and dictionary are shared across every sentence. The index is 3.3 GB
and never changes; it is referenced, not copied.

Inside the tree the directory *is* the key, so files carry role names
(`seed.pairs`, `top.segments`) rather than the canonical rendered names v2 uses
in the flat `results/` tree. Rendered names survive only where an artifact
lives outside the tree.

## No history

A superseded refinement round is not retained. Regenerating any artifact
overwrites it in place. Nothing here records which round produced what, how
many rounds have run, or what a previous round's ranking looked like.

This is affordable because every artifact is reproducible from its inputs, and
because the question history would answer — "am I in a refinement loop?" — is
derivable without it: the loop is running exactly when
`.wf/classified/no/no.pairs` is newer than `dfs.seed`. The loop is a freshness
reading, not a recorded state.

There is likewise no state file. See "What needs regenerating" below: every
state in the pipeline is derivable from artifact mtimes plus the location of
the p2 bundle, and a stored copy of a derived fact is one more thing that can
disagree with reality.

## What needs regenerating

This is the core of the design. Freshness is derived, never stored.

| Artifact | Stale when | Cost |
|---|---|---|
| `dfs.seed` | `seed.pairs` newer, or `classified/no/no.pairs` newer | minutes–hours |
| `top.segments` | `dfs.seed` newer | seconds |
| `best.pairs` | `top.segments` newer, or `classified/yes/yes.pairs` newer | milliseconds |
| `dfs.best` | `best.pairs` newer, or `classified/no/no.pairs` newer | minutes–hours |

Comparisons are against the symlink *target*'s mtime for `dfs.seed` and
`dfs.best`, which is what `Path.stat()` returns.

Staleness is advisory. It is reported and offered, never enforced, and never
triggers a regeneration the operator did not ask for.

`classified/no/no.pairs` is a global aggregate: a hard NO recorded while
working on one sentence marks every other sentence's DFS outputs stale, even
though a different letter bag makes those exclusions largely irrelevant. This
is left as is — it errs toward false "stale" rather than false "fresh", and
correcting it would mean recording which exclusions were in effect, which is
the bookkeeping this design exists to avoid. It is reported as **"hard-NO set
changed since this was generated"**, which is what is actually known.

### The review gate

Timestamps cannot see the p2 review, which is a human gate that stays open for
days with no file changing. That state is already durable, in the location of
the bundle named by the run:

| Where the bundle is | Review state |
|---|---|
| nowhere | not submitted |
| `p2/queued/` | submitted, not prepared |
| `p2/eval/` | notes out, awaiting review |
| `p2/done/` | complete |

It is derived from directory membership, not copied into the run.

## Four rules

Every artifact in the tree obeys these. They are what keeps the edge cases from
multiplying.

1. **One atomic commit per artifact.** Everything before the commit is scratch
   and freely deletable; everything after it is garbage collection and freely
   skippable. A crash leaves an orphan, never an ambiguity.
2. **Never store what the artifact yields.** The top-N cutoff is `wc -l`. The
   `-n` cap is in the symlink target's name. Freshness is mtime. Review state
   is the bundle's directory.
3. **An mtime means the content changed.** Enforced by writing every artifact
   whose mtime is read through `setops` with `stable_mtime=True`, so a
   byte-identical regeneration disturbs nothing downstream.
4. **A derived fact may be cached for a human to read, never consumed by the
   tool.**

### What the rules decide

`top.segments` and `best.pairs` carry **no parameter in their names**. A
parameter in a filename makes publishing a two-step operation — rename the new
file, unlink the old one — with an observable intermediate state and an
invariant a crash can violate. Both parameters are recoverable without it:

- `top.segments` is `head -n N` output, so its N is `wc -l`.
- `best.pairs` is derived from `top.segments`, which is in the same directory,
  so its universe size is `wc -l top.segments`.

Each is therefore a fixed name published by a single `rename(2)`, which
`setops._place` already does.

The DFS outputs are the exception: `-n` is a *cap*, and a run that exhausts the
search comes up short, so the cap is not recoverable from the output. Those are
published as **symlinks** — write the data under its rendered name, then
`rename(2)` a symlink over the fixed name. That rename is the single commit
point; unlinking the previous target is garbage collection, and skipping it
leaves an orphan identifiable as "a `dfs.*` no symlink names".

A requested top-N larger than the number of available segments yields fewer
lines than requested, and `wc -l` reports what is actually there. Re-requesting
the larger N regenerates byte-identical output, which rule 3 turns into a no-op.

## BEST PAIRS is an intersection

```text
best.pairs = common(top.segments, .wf/classified/yes/yes.pairs)
```

**not** the current review batch's `*.p2.yes`. v2 cannot have both of the
things it claims: normal p2 filtering avoids repeat review across segment
counts, and one bundle's `*.p2.yes` is the complete BEST PAIRS artifact. If
`wooden,toy` was confirmed while reviewing `g4` and appears in `g5`'s top
segments, p2's done-set filters it out of `g5`'s review, so `g5`'s `*.p2.yes`
does not contain it — yet it belongs in `g5`'s BEST PAIRS.

Building it as an intersection over the current candidate universe fixes that,
and makes `--no-filter` unnecessary: the run reviews only genuinely unknown
candidates, and known YES entries are restored by the intersection rather than
by re-reviewing them.

It also makes cutoff increments work without special handling. Raising the
cutoff regenerates `top.segments` over a longer prefix; p2's done-set means only
newly encountered pairs are queued for review; and `best.pairs` is rebuilt from
the whole new prefix rather than from the latest batch. The intersection is a
pure function of two files and costs milliseconds, so it is regenerated rather
than accumulated.

Prior soft NOs stay skipped by default, and `--no-filter` remains available as
the deliberate way to reconsider them.

## Targets

A **target** is one sentence at one segment count: the directory `s2/m4/g4`.
The vocabulary is deliberately make's, because the design is make's — files
with declared prerequisites, out-of-date computed from mtimes, work ordered by
the dependency graph. *Target*, *prerequisite*, *out of date*, and *up to date*
all mean here what they mean there.

`wf best status` addresses targets by a `/`-delimited prefix of the tree, which
means "every target under it":

```text
wf best status              # every target
wf best status s2           # all of s2
wf best status s2/m4        # every segment count under s2/m4
wf best status s2/m4/g4     # one target
```

The prefix semantics are what justify the form, and only `status` has them:
every other command acts on exactly one fully-qualified target and names it
with `-g`/`-m`. Whether the address form should reach them anyway is recorded
in `words/docs/todo`.

## Command surface

Only the primitives are in scope. Each does one thing, to one target, with no
inference about what should happen next.

```text
wf best status   [ADDRESS]
wf best gen      SENTENCE -g N [-m N] STAGE [-r|--results-dir DIR] [-n N] [--top N]
wf best exclude  SENTENCE -g N [-m N] FILE
wf best review   SENTENCE -g N [-m N]
wf best complete SENTENCE -g N [-m N]
```

`-g` is required; `-m` defaults to 4. They are named the same as the
`dfs-anagrams` options they carry, so the flag you type is the flag that runs.

There is no `wf best next`. Deciding which stage follows and running it is the
collective operation, which this plan does not cover; what `next` was actually
for survives as a report line, described under "Status after every operation".

### Stages

`gen` produces one artifact and is named for the artifact it produces, so the
stage you type and the file that appears are the same string.

| Stage | Runs | Produces |
|---|---|---|
| `gen dfs.seed` | `dfs-anagrams --pairs seed.pairs` | `g4/dfs.seed` -> results/ |
| `gen top.segments` | `top-segments --pairs \| head -n N` | `g4/top.segments` |
| `gen best.pairs` | `common(top.segments, classified/yes)` | `g4/best.pairs` |
| `gen dfs.best` | `dfs-anagrams --pairs best.pairs` | `g4/dfs.best` -> results/ |

The other three commands are verbs rather than stages because they are not
artifact production: `exclude` composes `wf classify no`, and `review` and
`complete` compose the existing p2 lifecycle (`submit p2` / `eval p2`, then
`complete p2`).

`complete` writes `best.pairs` as its tail, since the intersection is free.
`gen best.pairs` exists separately because `classified/yes` can grow from
*another* target's review, which puts this target's `best.pairs` out of date
with no review of its own to complete.

### `gen` is imperative

`gen` always regenerates. It does not consult freshness, does not skip an
artifact that is already up to date, and has no notion of "already done".

This follows from the primitives being deliberately dumb: a primitive that
second-guesses the operator is not one. Freshness belongs to `status`, and
skipping what is fresh belongs to the collective operation that is not in this
plan. It also settles the parameter case without a special rule -- a fresh
`top.segments` of 1000 rows with a wanted cutoff of 2000 is not *stale*, so a
freshness-gated `gen` would refuse work that was never in question.

The consequence is that **`-f` carries no overwrite meaning on `gen`**, on any
stage, because there is no skip for it to override.

### Creating a target directory

`gen dfs.seed -f` creates a missing `gN/`. Every other stage fails when `gN/`
is absent, and so does `gen dfs.seed` without `-f`.

`dfs.seed` is the pipeline's first stage, so it is the only one for which a
missing target directory is an ordinary situation rather than a mistake. For
the rest, absence means a prerequisite was never built -- they would fail on
their missing inputs a moment later anyway, so refusing on the directory is
the same error reported earlier and more clearly.

Requiring `-f` rather than creating it silently is what catches a mistyped
segment count: `gen s2 -g 45 dfs.seed` should not quietly open a new target and
start an hour of DFS.

`-f` creates `gN/` and nothing else. A missing `s2/` or `m4/` is always an
error, because those hold hand-placed files -- `letters` and `seed.pairs` --
that `gen dfs.seed` requires and `-f` cannot conjure.

Two notes on the flag itself. `-f` is global (`usage.py:13`) and its help text
reads "force overwrite existing files"; here the guard is absence rather than
existence, so the description generalizes to **overriding the refusal** -- the
tool says no, `-f` says do it anyway -- which also covers what `submit` and
`complete` already use it for. And because the global parser hands `-f` to every
subcommand, rejecting it where it means nothing (`gen top.segments -f`) is a
per-subcommand check rather than something the parser gives for free. It should
be rejected: an accepted argument that does nothing is a lie.

### Output

One line per operation, in the shape `log.success` already uses elsewhere --
what happened, the count, then the destination. Counts come from
`fs.line_count`.

```text
$ wf best gen s2 -g 4 top.segments
Generated 1000 top segments -> s2/m4/g4/top.segments
```

A generation that shells out prints the command first, so the exact invocation
is on screen whether or not `wf` is the one running it:

```text
$ wf best gen s2 -g 4 dfs.seed
Running dfs-anagrams:
  dfs-anagrams "$IDX" "$S2" -m 4 -S 20 -p 10000000 \
    -n 1000000 --word-bonus 1 --dict .wf/best/dict/words.big \
    --pairs .wf/best/s2/m4/seed.pairs --exclude-pairs WFROOT \
    -x 2 -g 4
Generated 1000000 results in 14m22s -> results/s2/dfs.s2.idx2.m4.x2.g4.85.15.1000000
```

This keeps the reproducibility value of an emitted command while `wf` retains
the state, the freshness tracking, and the progress reporting.

### Status after every operation

Every command ends by reporting its target's derived state and, when the target
is out of date, the one command that would advance it:

```text
$ wf best exclude s2 -g 4 hard-no.pairs
Classified NO: 3 new, 47 total -> no.pairs
s2/m4/g4: dfs.seed out of date (hard-NO set changed)
  next: wf best gen s2 -g 4 dfs.seed

$ wf best gen s2 -g 4 dfs.best
Generated 1000000 results in 16m03s -> results/s2/dfs...top1000.1000000
s2/m4/g4: up to date
```

This is where `next` belongs. Its value was never the execution -- it was
knowing what follows. As a report line it costs nothing and cannot surprise
anyone; as a command it would sometimes print a sentence and sometimes start an
hour of DFS under the same name.

Two constraints follow. It reports **only the target the command acted on**: an
`exclude` puts every sentence's `dfs.seed` out of date, and printing all of that
after every command is noise, so cross-target fallout stays in `wf best status`
where it was asked for. And the freshness derivation has to be a callable rather
than a command body, because four commands end by invoking it.

`wf` does not check for competing `dfs-anagrams` or `query-index` processes.
Keeping timing measurements clean stays the operator's job, as `CLAUDE.md`
describes; a check run from inside a sandbox PID namespace would see an empty
process table and report a false all-clear, which is worse than no check.

`dfs-anagrams` and `top-segments` are invoked by name. `setup.sh` puts
`~/code/nutrimatic/build` on `PATH`, alongside tools like `note` and `split.sh`,
so the workflow state holds no path to them. A binary missing from `PATH` is a
diagnostic naming it, raised before any artifact is touched.

### `-r` / `--results-dir`

`-r`/`--results-dir` names the parent results directory and defaults to
`results/`. It must already exist; a missing directory is a diagnostic rather
than an implicit `mkdir`, because the common failure is a typo, not a genuinely
new tree. The per-sentence subdirectory beneath it is the tool's and is created
as needed.

It is resolved against the invoking process's working directory before the
`wf` wrapper changes directory.

## Layout integration

`.wf/best` has a static crown and a dynamic body, and the seam is at the
sentence level. Only the crown is expressible in `config.CONFIG_LAYOUT`:

```text
best/          static   -> registered in CONFIG_LAYOUT
  idx/         static
  dict/        static
  s2/          dynamic  -> not registered
```

Registering the crown gives `wf init` the three directories and makes
`config.path(root, ["best"])` a valid anchor, which is all the dynamic layer
needs. `config.path` stays a closed-world validator and must not learn
wildcards: every layout slot access goes through it, and its per-level
`fs.raise_if_not_dir` is what turns a mistyped or uninitialized slot into a
diagnostic instead of a silently created directory.

The dynamic body gets a typed accessor built on that anchor, validating by
shape rather than by name membership. `config.classified` and the new
`config.fold_classified` are the existing precedent for a typed helper that
appends components the layout does not enumerate.

`show.py` needs no change at all. `best` is a top-level command, not a `show`
target, so it registers in `wf.py`'s `COMMANDS` as a
`command.Dispatcher("best — ...", {"status": ..., "gen": ..., ...})`, exactly
the way `submit`, `eval`, `complete`, `extract`, and `classify` already nest
their own targets. `dispatch.run` recurses through a registry and never
consults the layout, so nothing about the dynamic tree reaches `layout_args`,
`show_target`, or `_build_aggregates`.

That is the payoff of dropping `wf best show`: the one place the dynamic tree
would have collided with the static layout machinery was `show`'s
`argv`-as-path-components model, and without it there is no collision to
resolve.

## Prerequisites

Landed:

- `wf submit p2 FILE` queues a neutral `*.pairs` idempotently, and `wf eval p2`
  accepts both `*.pairs` and `*.p1.yes` (`names.QUEUE_SUFFIXES`).
- `wf classify no FILE` unions hard NOs into `.wf/classified/no/no.pairs`.
- `dfs-anagrams --exclude-pairs FILE|WORKFLOW-DIR` (commit `f7870a0`).
- `setops` `stable_mtime`, and `config.fold_classified` as the one way to write
  the classified aggregates. Rule 3 depends on this.

Milestone 0, still open:

- `complete` preflights every archive destination and requires `-f` on that
  invocation when an archived artifact would be replaced. Today the archive
  steps call `fs.move_into` / `fs.rename_once` one at a time with `ctx.force`,
  so a collision fails partway through the recipe instead of before it starts.

## Not in this plan

The collective operation -- one command that decides which stage a target needs
and runs everything mechanical until it reaches a human gate. The primitives
above are deliberately dumb so that it can be designed against them once they
exist, rather than guessed at now.

What the design work so far established, so it is not lost:

- **The stage graph is a `steps` recipe.** `steps/__init__.py` already runs a
  list of steps, skips whatever `is_done` reports, and takes `-f` to mean
  "ignore `is_done` and overwrite". That is the collective operation's runner,
  already written.
- **Freshness is generic, not per-stage.** Every step declares `inputs(ctx)`
  and `outputs(ctx)`, so the whole "what needs regenerating" table is one
  `is_done` variant -- "outputs exist and are not older than their inputs" --
  beside the default "outputs exist".
- **The runner has no blocked state.** A step that is neither done nor runnable
  is exactly what the review gate is: `top.segments` is up to date, and
  `best.pairs` cannot be built until a human checks notes. Stopping cleanly on
  that is a change to a module `complete p1`, `complete p2`, and every future
  recipe share, which is why it is not smuggled in beside a primitive.
- **It gates on the target directory and never creates one**, with no `-f`
  override. An early exit validates that `gN/` exists and fails otherwise:
  there can be no next step where there was no previous one. Opening a new
  target stays the exclusive job of `gen dfs.seed -f`, so directory creation
  has exactly one home in the whole tool, reachable one way.

  The gate being unconditional is what keeps `-f` coherent across the two.
  Inside a `steps` recipe `-f` means what `steps/__init__.py` already says --
  ignore `is_done` and overwrite -- and on `gen dfs.seed` it overrides a refusal
  to create. Those readings never meet: the collective operation has no creation
  for `-f` to reach, and `gen` has no `is_done` for it to ignore.

Also outside this plan: v2 stages 1 and 2 (candidate generation, the `comm`
against `p1_done.pairs`, chunking, and the `evalpair` cycle), which stay manual.

