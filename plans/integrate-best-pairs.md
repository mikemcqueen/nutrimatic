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
| 0 | Prerequisites: `complete` preflight, `top-segments -n`, `setops.{diff,common}(stable_mtime=)`, `init` classified aggregates, `-d` from `$WFROOT` | [ ] | one commit each |
| 1 | `.wf/best` layout, target accessor, freshness, `wf best status` | [ ] | `Add the BEST PAIRS state layout` |
| 2 | `gen dfs.seed`, `gen top.segments`, status after every operation | [ ] | `Run provisional DFS and top-segments from wf` |
| 3 | `exclude`, `review`, `complete`, `gen best.pairs` | [ ] | `Accumulate BEST PAIRS from the confirmed-YES set` |
| 4 | `gen dfs.best` | [ ] | `Add the final DFS stage` |

Each milestone brings whatever smoke test its own deliverable needs, named
when the milestone is worked rather than enumerated here. Python tests go in
`words/tests/`, building their tree with `wf_fixture` so a layout change stays
a one-line edit in `config.CONFIG_LAYOUT`; the `top-segments -n` prerequisite
is tested in nutrimatic. Smoke tests only, per `CLAUDE.md`.

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
them; this plan begins with the seed already extracted.

The scope boundary and the ownership boundary are the same line. Everything
stages 1 and 2 produce is placed by hand; everything stages 3 through 7 produce
is the tool's. So there is no registration command: the sentence directory, its
`letters` file, and `m4/seed*.pairs` are created by the operator, exactly as the
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
    wiki-merged.2.index          # symlink, placed by hand; named, never parsed
  dict/
    words.big                    # copy or symlink of tmp/words.big
  s2/
    letters                      # the letter bag, written once by hand
    m4/
      seed.idx2.85.15.pairs      # the 85/15 P1 extract
      g4/
        dfs.seed -> RESULTS/dfs.s2.idx2.85.15.m4.x2.g4.1000000
        top.segments
        best.pairs
        dfs.best -> RESULTS/dfs.s2.idx2.85.15.m4.x2.g4.best.1000000
      g5/
        ...
```

Three levels, each holding exactly what is scoped to it:

- **Sentence** (`s2/`): the letter bag, and nothing else. `letters` is written
  once by hand and thereafter read from the file, never re-sourced from
  `setup.sh`, so a run stays reproducible after `setup.sh` drifts. The tool
  does not compare it against the environment: it is frozen precisely because
  the environment is not authoritative.
- **Candidate universe** (`m4/`): the seed and anything else determined by
  `-m`. The seed's own v2 name (`pairs.s2.idx2.m4.x2.85.15.p1.yes`) says it is
  `m`-dependent, so it cannot live above this level. This level stays even
  while only `m4` is exercised: collapsing it into the sentence directory would
  put an `m`-dependent artifact at a level that does not key on `m`.
- **Segment count** (`g4/`): the four generated artifacts of stages 3 through 7.

The index and dictionary are shared across every sentence. The index is 3.3 GB
and never changes; it is referenced, not copied.

Both are found by **hardcoded filename**: `best/idx/wiki-merged.2.index` and
`best/dict/words.big`. An absent one is a diagnostic naming the path, raised
before any artifact is touched. `idx/` is a directory rather than a bare
symlink so that a later `--index` can select among several without moving
anything, but nothing selects today and a second index sitting there is simply
ignored.

Inside the tree the directory *is* the key, so files carry role names
(`top.segments`, `best.pairs`) rather than the canonical rendered names v2 uses
in the flat `results/` tree. Rendered names survive only where an artifact
lives outside the tree.

### The seed annotation

Two dimensions of v2's DFS names are unavailable to the tool. `idx2` is the
index generation: the tool knows the index by a hardcoded filename, but reading
`idx2` back out of `wiki-merged.2.index` would mean parsing that name for a
dimension, which is what `names.py` forbids. `85.15` is the P1 band, a
property of the hand-placed seed that stages 1 and 2 -- out of scope -- would
have to record.

Both are properties of the seed: the band is the extract window, and the
candidates behind the seed came out of that index. So the seed carries them, in
its own filename, and the tool finds it by glob:

```text
s2/m4/seed*.pairs
```

Exactly one match, else a diagnostic. Whatever sits between `seed` and `.pairs`
is an **opaque annotation**: it is copied verbatim into the rendered DFS names
and never taken apart, so `names.py`'s rendered-never-parsed rule holds. Present
it and it appears; omit it and the names are shorter.

```text
s2/m4/seed.idx2.85.15.pairs -> dfs.s2.idx2.85.15.m4.x2.g4.1000000
s2/m4/seed.pairs            -> dfs.s2.m4.x2.g4.1000000
```

The tool cannot validate the string, so a typo reaches the results directory
unchallenged -- cosmetic, since nothing reads it back. Annotating a seed that
already has DFS output changes the rendered target name, which orphans the
previous output rather than overwriting it.

One thing the annotation cannot promise: the index link is shared and sits
above the sentence, so re-pointing it without re-extracting leaves `idx2` a
true statement about the seed's provenance and a false one about the DFS run
that used it. Nothing else records the generation either, so this is where it
rests.

## No history

A superseded refinement round is not retained. Regenerating any artifact
overwrites it in place. Nothing here records which round produced what, how
many rounds have run, or what a previous round's ranking looked like.

`best.pairs` is the exception, and the only one: it unions the prior artifact
into each regeneration, so confirmations accumulate across rounds rather than
tracking the current sample. That is history of a kind -- the set, never the
sequence -- and it is there because `best.pairs` is the workflow's product
rather than an intermediate. See "What accumulation costs".

Everything else is affordable for two reasons. Every other artifact is
reproducible from its inputs, so a superseded round is not information, it is
a cache. And the
question history would be kept for -- "how many rounds have I run on this
target?" -- is already answered elsewhere: the p2 archive holds one bundle per
completed round, `p2/done/in/top.s2.m4.g4.*.pairs`, with the ordinals `review`
mints. That is history `wf` keeps for its own reasons, and the tree does not
need a second copy.

The tool never recognizes a refinement loop as such. There is no loop state
anywhere in the design, and no command reports one. `status` answers what is
out of date and which command advances it, which is all the loop ever needed
from it.

There is likewise no state file. See "What needs regenerating" below: every
state in the pipeline is derivable from artifact mtimes and the p2 bundle's
whereabouts, and a stored copy of a derived fact is one more thing that can
disagree with reality.

## What needs regenerating

This is the core of the design. Freshness is derived, never stored.

| Artifact | Stale when | Cost |
|---|---|---|
| `dfs.seed` | the seed newer, or `classified/no/no.pairs` newer | minutes–hours |
| `top.segments` | `dfs.seed` newer | seconds |
| `best.pairs` | `top.segments` newer, or either `classified` aggregate newer | milliseconds |
| `dfs.best` | `best.pairs` newer, or `classified/no/no.pairs` newer | minutes–hours |

Comparisons are against the symlink *target*'s mtime for `dfs.seed` and
`dfs.best`, which is what `Path.stat()` returns.

A **dangling** symlink -- target garbage-collected, or `results/` cleaned by
hand -- has no mtime to compare. `Path.stat()` follows the link and raises;
`Path.exists()` reports False. Freshness reads it as **missing**, so `next:`
names the `gen` that rebuilds it, and `status` says which kind of missing it is
along with the path that is gone. A deleted result and a stage never run want
the same command but suggest very different things about what happened.

Staleness is advisory. It is reported and offered, never enforced, and never
triggers a regeneration the operator did not ask for.

`classified/no/no.pairs` is a global aggregate: a hard NO recorded while
working on one sentence marks every other sentence's DFS outputs stale, even
though a different letter bag makes those exclusions largely irrelevant. This
is left as is — it errs toward false "stale" rather than false "fresh", and
correcting it would mean recording which exclusions were in effect, which is
the bookkeeping this design exists to avoid. It is reported as **"hard-NO set
changed since this was generated"**, which is what is actually known.

### Hand-placed inputs

`letters` and the seed are not in the table, because staleness is not what
`status` can say about them. They have no producing command, so "out of date"
has no `next:` line to offer. What `status` reports instead is **missing**, and
where to put the file:

```text
$ wf best status s2/m4/g4
s2/m4/g4: letters missing
  place: $WFROOT/.wf/best/s2/letters

$ wf best status s2/m4/g4
s2/m4/g4: seed missing
  place: $WFROOT/.wf/best/s2/m4/seed*.pairs

$ wf best status s2/m4/g4
s2/m4/g4: dfs.seed missing
  next: wf best gen s2 -g 4 dfs.seed
```

`letters` is reported when absent and its mtime is **not** read. It is written
once and frozen -- that is the whole reason it is a file rather than a lookup
into `setup.sh` -- so treating an edit to it as a staleness signal would buy
nothing and put a third clause on every DFS freshness check.

### The review gate

Timestamps cannot see the p2 review, which is a human gate that stays open for
days with no file changing. That state is already durable, in the location of
the target's bundle:

| Probe | Review state |
|---|---|
| `p2/queued/<prefix>*.pairs` | submitted, not prepared |
| `p2/eval/<prefix>*/` | notes out, awaiting review |
| newest `p2/done/in/<prefix>*.pairs` newer than `top.segments` | reviewed |
| otherwise | not reviewed |

It is derived from what is on disk, not copied into the run.

There is no `p2/done/<bundle>/` to look in. `complete p2` scatters a bundle:
`p2_archive` moves the input to `done/in/<bundle>.pairs`, the verdicts to
`done/out/<bundle>.p2.yes`, and the notes to `done/out/enex/<bundle>/`, then
`p2_advance` publishes the NO set into `p3/queued/` and `bundle.finish` removes
the eval directory. Emptying the eval directory is what separates "notes out"
from "reviewed".

The last row needs the mtime, not just the probe. The prefix matches every
archived round of this target, so its presence alone says only that *some*
review finished -- not that one covers the `top.segments` sitting there now.
The archived input's mtime is when `submit` wrote it, preserved through the
rename, so a round newer than `top.segments` was submitted after the current
content was generated and therefore covers it. `stable_mtime` on `top.segments`
means a byte-identical regeneration does not bump it, and a prior review keeps
counting.

Mtimes alone cannot answer this. `best.pairs` intersects against the *global*
`classified/yes`, so a target nobody has reviewed still gets a real, non-empty
`best.pairs` from other targets' verdicts -- `wooden,toy` confirmed under `g4`
lands in `g5` without `g5` being reviewed. That is intended. But it leaves
`best.pairs` newer than `top.segments` with this target's own candidates never
judged, and only an archived round can distinguish that.

#### The bundle name

`submit p2` names the bundle from the submitted file's basename
(`submit.py:36`, `names.queue_name`), so submitting a file called
`top.segments` would name the bundle `top.segments` for every sentence and
every `-g` at once. `review` therefore submits under a rendered name:

```text
top.<sentence>.m<N>.g<N>.<cutoff>.r<K>.pairs
```

`K` is the **round**: the number of archived bundles for this target, plus one.
It is derived at submit time, not stored -- there is no round counter anywhere
in the tree. The cutoff sits in the name because the p2 archive is flat and has
no directory to carry it; it is not what makes the name unique.

The round exists because the cutoff cannot separate one review from the next.
The common case is `exclude` -> `gen dfs.seed` -> `gen top.segments` -> review
again, where the cutoff is unchanged and the content is not. Without an ordinal
every round of every target would collide with its own archived copy.

#### The in-flight probe

The target's bundle is found by its **prefix**, cutoff excluded:

```text
p2/queued/top.s2.m4.g4.*.pairs     # files
p2/eval/top.s2.m4.g4.*/            # directories
```

The trailing separator is load-bearing -- without it `g4` also matches `g45`.
The cutoff is out of the prefix so that raising it cannot hide a bundle
submitted under the old one. This is the same shape of lookup the existing
commands use: `select` by `stem:` prefix, over `names.queue_globs`.

#### `review` submits the target's own file, or fails

| Target's bundle | `review` |
|---|---|
| nowhere | submits under the rendered name, then `eval p2` |
| `p2/queued/` or `p2/eval/` | fails, naming the bundle and where it sits |
| archived | submits a new round |

#### What `review` submits

Not `top.segments` itself, but `top.segments` minus the hard NOs:

```text
submitted = diff(sorted(top.segments), .wf/classified/no/no.pairs)
```

Hard NOs enter `classified/no` through `exclude`, which composes
`wf classify no`. They never pass through p2, so they are absent from
`p2_done.pairs` and no filter in the p2 machinery would drop them -- and since
`review` does not filter at all, a `top.segments` generated before the
exclusion puts every one of them back in front of the reviewer.

The intended flow removes them earlier: `exclude` reports `dfs.seed` out of
date, regenerating it with `--exclude-pairs` drops the entries, and
`top.segments` never contains them. Subtracting at submit makes that ordering
an optimization rather than a requirement.

The sort is the same scratch copy `gen best.pairs` takes: `top.segments` is
stored ranked and `comm -23` needs collation. Nothing sorted is kept. The
ranking is not lost to the subtraction either way -- `submit` places its input
with `setops.merge`, which is `sort -u`, so the queued artifact is collated
however it arrives.

`review` also refuses on an **empty `top.segments`**, before submitting
anything. `eval p2` would otherwise open a bundle, split zero pairs, and make
zero notes (`eval.py:_split_pairs`, `_make_notes`), leaving a bundle in
`p2/eval/` that no `complete` can drain and that the in-flight probe above
blocks every future `review` on. Checking the count first means nothing is
created and there is no unwind path to get wrong. An empty `top.segments` means
the DFS found no multi-word segments, so the diagnostic points back at
`dfs.seed`.

That is the whole of the empty case, because `review` does not filter -- see
below. With `bundle.filter_done` in play, a cutoff bump whose new prefix was
entirely already-judged would drain a non-empty `top.segments` to nothing and
land in the same wedged state.

`review` never adopts an artifact it did not place. A file put into `p2/queued/`
by hand is finished by hand with `wf eval p2`; picking it up automatically would
mean `review` producing notes over a candidate list it never chose.

That refusal is also what makes the round sound. `K` is counted only from the
state where nothing is in flight, so a bundle sitting in `queued/` or `eval/`
can never be skipped by the count and re-minted under the same name. The one
way to break it is deleting an archived bundle by hand, which reuses an
ordinal; milestone 0's preflight is what catches that.

## Four rules

Every artifact in the tree obeys these. They are what keeps the edge cases from
multiplying.

1. **One atomic commit per artifact.** Everything before the commit is scratch
   and freely deletable; everything after it is garbage collection and freely
   skippable. A crash leaves an orphan, never an ambiguity.
2. **Never store what the artifact yields.** The top-N cutoff is `wc -l`. The
   `-n` cap is in the symlink target's name. Freshness is mtime. Review state
   is where the bundle sits.
3. **An mtime means the content changed**, for every artifact `setops` places.
   `top.segments` and `best.pairs` are written with `stable_mtime=True`, so a
   byte-identical regeneration disturbs nothing downstream. The two DFS stages
   are outside the rule; see below.
4. **A derived fact may be cached for a human to read, never consumed by the
   tool.**

### What the rules decide

`top.segments` and `best.pairs` carry **no parameter in their names**. A
parameter in a filename makes publishing a two-step operation — rename the new
file, unlink the old one — with an observable intermediate state and an
invariant a crash can violate. Both parameters are recoverable without it:

- `top.segments` is `top-segments -n N` output, so its N is `wc -l`.
- `best.pairs` has no parameter to carry. It accumulates across rounds, so it
  is not scoped to any one cutoff and there is no N that would be true of it.

Each is therefore a fixed name published by a single `rename(2)`, which
`setops._place` already does.

The DFS outputs are the exception: `-n` is a *cap*, and a run that exhausts the
search comes up short, so the cap is not recoverable from the output. Those are
published as **symlinks**.

`dfs-anagrams` writes to a scratch name beside the rendered one, and the
rendered name appears only by `rename(2)` on clean exit. Without that, a
refinement round is not atomic at all: the rendered name is a pure function of
keys that do not change between rounds, so the second `gen dfs.seed` would write
over the very file the live symlink resolves to, and an interrupted run would
leave the symlink pointing at a truncated output with a fresh mtime -- which
`status` reads as up to date. A crash now leaves only the scratch file.

What commits depends on whether the rendered name changed, because a symlink
stores a *path*, not a reference to the file it names:

- **Name unchanged** (the refinement round). The symlink already spells that
  path, so renaming the scratch into it publishes the new data with no second
  step. That rename is the commit point, and the previous inode is freed --
  there is no orphan.
- **Name changed** (a different `-n`, or a seed annotation added). Nothing
  points at the new name yet, so its rename is unobservable; the commit is
  `os.symlink(target, tmp)` followed by `os.replace(tmp, dfs.seed)`, which
  renames the link itself and is atomic. The previous target is left orphaned,
  identifiable as "a `dfs.*` no symlink names", and unlinking it is garbage
  collection.

Either way exactly one rename is observable.

They are also where rule 3 stops. The mtime read for a DFS stage is the
freshly-renamed target's, so a byte-identical rerun always marks
`top.segments` stale. Content-comparing two 1,000,000-line files after every
hours-long run to avoid a few seconds of `top-segments` is not a trade worth
making, and the error is in the safe direction -- the same false-stale the
global `no.pairs` aggregate already produces.

Orphans and abandoned scratch files are the **operator's** to clean. Nothing
reports them and no command removes them. There is one per rendered-name change
and one per interrupted run, at roughly 50 MB each for a 1,000,000-line output,
and identifying one means knowing every live symlink target across the tree.
`results/` is a directory the operator already owns and prunes by hand; this
plan does not take that over, and it gains no destructive command.

A requested top-N larger than the number of available segments yields fewer
lines than requested, and `wc -l` reports what is actually there. Re-requesting
the larger N regenerates byte-identical output, which rule 3 turns into a no-op.

## BEST PAIRS accumulates

```text
best.pairs = diff(common(merge(sorted(top.segments), best.pairs),
                         .wf/classified/yes/yes.pairs),
                  .wf/classified/no/no.pairs)
```

The prior `best.pairs` is an operand of its own regeneration. This is the one
artifact in the tree that carries history, and the exception is deliberate:
`best.pairs` is what the whole workflow exists to produce, and a confirmed pair
that has fallen out of the current sample is still a confirmed pair worth
bonusing. The union is with the prior artifact only -- not a log of rounds --
so what accumulates is the set itself, nothing about how it got that way.

On the first generation there is no prior file, and `merge` takes the collated
`top.segments` copy alone. That is the same existence guard `setops.fold`
already writes once, applied to a scratch destination rather than to an
accumulator in place.

`top.segments` is stored **ranked** -- descending count, ties broken by text,
exactly as `top-segments --pairs` emits it. That ordering is what makes
`-n N` mean "top N", and it is the only thing that makes the file
interpretable to a human. It is therefore not a `setops` set: `comm -12`
requires `LC_ALL=C` collation and returns a silently wrong intersection given
anything else.

So `gen best.pairs` collates a scratch copy under `LC_ALL=C` and unions the
prior artifact into it. The sort is internal to the stage and never appears in
the tree -- storing a collated sibling would be storing a derived fact. The
prior `best.pairs` needs no sorting: it was placed by `comm` output and is
already a set. On files of a few thousand lines the whole expression is
unmeasurable, still milliseconds.

What it is **not**, at any point in the expression, is the current review
batch's `*.p2.yes`. v2 treats one bundle's
`*.p2.yes` as the complete BEST PAIRS artifact, which makes the artifact a
record of one review rather than a statement about the current candidate set.
Those come apart as soon as a verdict is reached anywhere else: `wooden,toy`
confirmed under `g4` belongs in `g5`'s BEST PAIRS, and no `g5` bundle has to
exist for that to be true.

The expression states the thing directly -- *the segments this target has
ranked, that are confirmed YES and not hard NO* -- and it holds whatever the
review history looks like. Under
p2's default filtering a known YES would be dropped from the bundle and missing
from its `*.p2.yes`; under this plan's unfiltered review it would be
re-confirmed and present. The intersection is the same file either way.

That is also what makes it rebuildable with no review to run. `classified/yes`
grows from other targets, and `gen best.pairs` picks that up for milliseconds
-- so it is regenerated on demand, and a raised cutoff simply rebuilds it over
the longer prefix.

### Hard NOs are subtracted

The subtraction is the only thing that ever removes an entry, which under
accumulation makes it load-bearing rather than tidy: without it a hard NO that
once reached `best.pairs` would stay there for every future round. `classify` records
a verdict into one aggregate and never withdraws it from the other: a pair
confirmed YES in p2 and later hard-NO'd by `exclude` sits in both, and
`classify.py`'s `_warn_if_contradicted` warns and proceeds precisely because
"there is no un-classify to undo the earlier call with". The contradiction is a
supported, durable state, and resolving it is the consumer's job.

`gen best.pairs` is that consumer, and the precedence is already settled
elsewhere in words: a hard NO says the pair must not appear in a result at all,
which is stronger than any YES. So the intersection is followed by a difference
against `classified/no`, and `classified/no` joins the artifact's freshness
row.

The DFS would paper over the omission -- `--exclude-pairs` drops the entry from
every result, so a hard-NO pair carrying a bonus cannot reach the output -- but
three things still go wrong without the subtraction:

- `status` reports `best.pairs` up to date immediately after an `exclude`,
  because `no.pairs` is not one of its prerequisites. The one artifact whose
  content the exclusion just invalidated is the one nothing offers to rebuild.
- `wc -l best.pairs` would overstate the bonus set that actually took effect,
  and under accumulation the overstatement is permanent.
- `review` already subtracts the same aggregate. Leaving the next stage without
  it makes the pipeline defend against one contamination at submit and forget
  about it a stage later.

Doing it here rather than only at `review` is also what survives the human
gate. `review`'s subtraction is a snapshot taken at submit; the bundle then
sits in `p2/eval/` for days, and an `exclude` in that window cannot reach notes
already written. The pair stays checkable, `complete p2` folds it into
`classified/yes` -- through `fold_classified`, which carries no contradiction
warning -- and `complete`'s own `best.pairs` tail would publish it. Only a
subtraction evaluated at generation time sees every verdict that exists by
then.

Both operands are `LC_ALL=C` sets already: `fold_classified` is the one writer
of either aggregate and places through `merge`, which is `sort -u`. Both carry
`stable_mtime: True` in `config._CLASSIFIED`, so adding `classified/no` to the
freshness row cannot manufacture a false stale from a no-op fold. The cost is a
second `comm` over a few thousand lines.

### What accumulation costs

Three properties are given up, knowingly.

**`best.pairs` is not reproducible from its current inputs.** Delete it and
regenerate, and what comes back is the current prefix's confirmed pairs alone
-- correct, but smaller. It is the one file in the tree an operator cannot
prune and rebuild, and the only one whose backup is worth having. Nothing
guards it; it is a few thousand lines of text and the operator owns the tree.

**The cutoff only opens.** Raising `-n` widens the universe and adds entries;
lowering it removes nothing, because the entries from the wider prefix are
already folded in. Narrowing a target's BEST PAIRS means deleting the file and
regenerating at the smaller cutoff, which is the deliberate act the previous
paragraph describes.

**`wc -l best.pairs` no longer measures the current prefix.** Rule 2's
recoverability argument does not reach this file: its universe is every prefix
that has ever been generated here, and nothing records that.

That is also why `dfs.best` renders as `dfs.<sentence>.<seed>.m<N>.x2.g<N>.best.<n>`
rather than carrying `top<cutoff>`. The cutoff was only ever true of a
single-round intersection, and a name that asserts a universe the file does not
have is worse than one that asserts less. `best` still separates the stage from
`dfs.seed`, which is all that component was structurally doing.

What is *not* given up is any verdict. `classified/yes` and `classified/no`
remain the standing record, `best.pairs` is downstream of both, and a hard NO
still reaches it on the next generation.

Because the file only grows, what came in is worth seeing:

```text
$ wf best gen s2 -g 4 best.pairs
Generated 152 best pairs (9 added, 2 dropped) -> s2/m4/g4/best.pairs
```

The counts are against the file being replaced, which `gen` has in hand before
`_place` renames over it. Added is the new confirmations; dropped is hard NOs,
and is normally zero.

### `review` does not filter

`wf best review` passes `--no-filter` to `eval p2` **always**, and exposes no
flag for it. The top N segments are reviewed whole, every round.

The alternative is `bundle.filter_done`'s default, which drops every pair
already carrying a p2 verdict. That is less human work -- a cutoff bump from
1000 to 2000 would present only the new 1000 -- but it means judging a
candidate against a batch that prior verdicts have already thinned, and it
leaves a soft NO recorded in some earlier context permanently invisible. For a
ranked top-N set read as a whole, seeing it whole is worth the repetition.

Nothing is lost by it. `complete p2` still folds every verdict into the p2
done-sets, so the record accumulates exactly as before; it is simply not
consulted here. And `best.pairs` does not depend on the filter at all -- the
intersection restores confirmed YES entries whether or not they were in this
round's bundle.

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

### The address grammar

```text
<sentence>[/m<N>[/g<N>]]      s[1-9] / m\d+ / g\d+
```

Each level has a shape, and a component that does not match its level's shape
is a diagnostic rather than an empty result. This is what "validating by shape
rather than by name membership" means: nothing enumerates the known sentences,
so a new one needs no registration, and `s2/4` is still rejected.

The shapes are also what keep `idx/` and `dict/` out of the walk. They sit
directly under `best/`, at the same level as `s2/`, but neither matches
`s[1-9]`, so a bare `wf best status` passes over them without a reserved-name
list to maintain.

The prefix semantics are what justify the form, and only `status` has them:
every other command acts on exactly one fully-qualified target and names it
with `-g`/`-m`. Whether the address form should reach them anyway is recorded
in `words/docs/todo`.

## Command surface

Only the primitives are in scope. Each does one thing, to one target, with no
inference about what should happen next.

```text
wf best status   [ADDRESS]
wf best gen      SENTENCE -g N [-m N] STAGE [-r|--results-dir DIR] [-n N]
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
| `gen dfs.seed` | `dfs-anagrams --pairs <seed>` | `g4/dfs.seed` -> results/ |
| `gen top.segments` | `top-segments --pairs -n N` | `g4/top.segments` |
| `gen best.pairs` | prior `best.pairs` + `top.segments`, ∩ `classified/yes`, less `classified/no` | `g4/best.pairs` |
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

### `-n`

`-n` is the count option of whichever tool the stage runs -- `dfs-anagrams -n`
for the DFS stages, `top-segments -n` for `top.segments`. One name, because in
every case it is the flag that runs.

| Stage | `-n` | Omitted |
|---|---|---|
| `gen dfs.seed` | `dfs-anagrams -n`, the results cap | `-n 1000000` |
| `gen top.segments` | `top-segments -n`, the cutoff | not passed |
| `gen best.pairs` | rejected | -- |
| `gen dfs.best` | `dfs-anagrams -n`, the results cap | `-n 1000000` |

`top.segments` is pure pass-through: `top-segments` carries the cutoff default
itself (1000, v2's `TOP_COUNT`), so omitting `-n` means omitting it from the
command. There is no second copy of the number to drift.

The DFS stages cannot do that, for two reasons. `dfs-anagrams -n` defaults to
10000 (`dfs-anagrams.cpp:21`), two orders of magnitude below what this workflow
wants -- v2 passes 1000000 explicitly. And rule 2 puts the cap in the symlink
target's name, because a run that exhausts the search comes up short and the
cap is not recoverable from the output; `wf` cannot render a name around a
value the tool chose without telling it. So `gen dfs.*` always passes an
explicit `-n`, and 1000000 is what it passes when the operator does not.

`gen best.pairs` shells out to nothing and has no count, so `-n` is rejected
there, for the same reason `-f` is rejected where it means nothing: an accepted
argument that does nothing is a lie.

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
error, because those hold hand-placed files -- `letters` and the seed --
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
  dfs-anagrams /home/mike/code/words/final/.wf/best/idx/wiki-merged.2.index \
    "$(cat /home/mike/code/words/final/.wf/best/s2/letters)" \
    -m 4 -S 20 -p 10000000 -n 1000000 --word-bonus 1 \
    --dict /home/mike/code/words/final/.wf/best/dict/words.big \
    --pairs /home/mike/code/words/final/.wf/best/s2/m4/seed.idx2.85.15.pairs \
    --exclude-pairs /home/mike/code/words/final \
    -x 2 -g 4
Generated 1000000 results in 14m22s -> /home/mike/code/nutrimatic/results/s2/dfs.s2.idx2.85.15.m4.x2.g4.1000000
```

Every path is absolute and resolved -- the line pastes into any shell with
nothing sourced and runs what `wf` ran, which is the only reason to print it.
The one abbreviation is the letter bag, a hundred characters that would bury
the rest.

It is spelled `$(cat .../letters)` rather than `"$S2"` deliberately. `wf` reads
the bag from the frozen `letters` file and never compares it against the
environment, so printing `"$S2"` would name a different string than the one that
ran in precisely the case the freezing exists for.

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
Generated 1000000 results in 16m03s -> /home/mike/code/nutrimatic/results/s2/dfs...g4.best.1000000
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

### Long runs

`gen dfs.*` runs in the **foreground** and blocks for the duration -- minutes
for a `g4` seed, longer for a wider search. `wf` is doing what the command it
printed would do if typed by hand, which is the whole point of printing it.

The two streams separate cleanly. `dfs-anagrams` writes results to stdout,
which is redirected into the scratch file, and progress through
`dfs_diagnostic` to stderr, which passes straight through to the terminal. So
`-p 10000000` still reports live while the output is being captured.

Ctrl-C leaves the scratch file and nothing else: the rendered name and the
symlink are untouched, so the previous output stays live and `status` keeps
telling the truth. Cleaning up the scratch is garbage collection.

Two `gen`s on one target race. The scratch name is per-target, so they collide
on it, and whichever finishes second wins the rename. Nothing guards this. It
is the same position the plan takes on competing processes below -- a lock is
durable state the design does not keep, and a killed run would leave one
standing. Two `gen`s on *different* targets share nothing and are safe.

`wf` does not check for competing `dfs-anagrams` or `query-index` processes.
Keeping timing measurements clean stays the operator's job, as `CLAUDE.md`
describes; a check run from inside a sandbox PID namespace would see an empty
process table and report a false all-clear, which is worse than no check.

`dfs-anagrams` and `top-segments` are invoked by name. `setup.sh` puts
`~/code/nutrimatic/build` on `PATH`, alongside tools like `note` and `split.sh`,
so the workflow state holds no path to them. A binary missing from `PATH` is a
diagnostic naming it, raised before any artifact is touched.

### The workflow root

`wf` runs against a workflow root -- the directory holding `.wf`. This plan's
is `$WFROOT`, `~/code/words/final`. The `.wf/best` tree lives there, while the
DFS outputs it points at live under nutrimatic's `results/`, which is why the
two are named separately. (`~/code/words/.wf` is a second, older tree; nothing
here touches it.)

`-d` defaults to `$WFROOT` when it is set, and to the working directory
otherwise. Without that default every `wf best` invocation carries
`-d ~/code/words/final`, because the working directory is nutrimatic and the
`.wf` tree is not there.

`$WFROOT` selects *which* workflow; it is never recorded into an artifact. That
is the same line `letters` draws -- the environment may say where to look, and
may not become part of what is stored.

### `-r` / `--results-dir`

`-r`/`--results-dir` names the parent results directory and defaults to
`results/`. It must already exist; a missing directory is a diagnostic rather
than an implicit `mkdir`, because the common failure is a typo, not a genuinely
new tree. The per-sentence subdirectory beneath it is the tool's and is created
as needed.

It resolves against the invoking process's working directory. The `wf` wrapper
used to `cd` into the words repo before `exec`ing Python, which made every
relative path argument -- including `-d`'s `Path.cwd()` default -- resolve there
instead of where the operator stood. It now puts the repo on `PYTHONPATH` and
runs `python -P`, so the working directory survives and `-r results/` typed in
nutrimatic means nutrimatic's `results/`.

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
needs.

`wf init` has to be re-run on `$WFROOT` before any `wf best` command works.
`config.path` calls `fs.raise_if_not_dir` at every level, so
`config.path(root, ["best"])` raises on a root initialized before `best` was
registered. `ensure_layout` is idempotent (`init.py:15-19`), so the re-run is a
step rather than work, and it is the same run that creates the empty
`classified/no/no.pairs` the prerequisites list:

```sh
source ./setup.sh    # for $WFROOT
wf init
```

Re-sourcing first is what makes `-d` land on `~/code/words/final` instead of
the working directory.

`config.path` stays a closed-world validator and must not learn
wildcards: every layout slot access goes through it, and its per-level
`fs.raise_if_not_dir` is what turns a mistyped or uninitialized slot into a
diagnostic instead of a silently created directory.

The dynamic body gets a typed accessor built on that anchor, validating by
shape rather than by name membership. `config.classified` and the new
`config.fold_classified` are the existing precedent for a typed helper that
appends components the layout does not enumerate.

`show.py` takes one change: `best` is skipped as a `show` target, and naming it
reports that with a pointer to `wf best status`.

Without that, registering the crown makes `wf show best` a half-working
command. `show_target` lists a directory with `path.iterdir()`, so it would
print `idx`, `dict`, and every sentence -- but `wf show best s2` goes through
`config.path(root, ["best", "s2"])`, which the layout does not contain, and
raises. One command that half-descends is worse than one that declines: the
address grammar lives on `status`, and there is exactly one way to walk the
tree.

That is the only change. `best` is a top-level command, not a `show` target, so
it registers in `wf.py`'s `COMMANDS` as a
`command.Dispatcher("best — ...", {"status": ..., "gen": ..., ...})`, exactly
the way `submit`, `eval`, `complete`, `extract`, and `classify` already nest
their own targets. `dispatch.run` recurses through a registry and never
consults the layout, so nothing about the dynamic tree reaches `layout_args`,
`show_target`, or `_build_aggregates`.

That is the payoff of dropping `wf best show`: the one place the dynamic tree
would have collided with the static layout machinery was `show`'s
`argv`-as-path-components model, and the skip is all it takes to keep them
apart.

## Prerequisites

Landed:

- `wf submit p2 FILE` queues a neutral `*.pairs` idempotently, and `wf eval p2`
  accepts both `*.pairs` and `*.p1.yes` (`names.QUEUE_SUFFIXES`).
- `wf classify no FILE` unions hard NOs into `.wf/classified/no/no.pairs`.
- `dfs-anagrams --exclude-pairs FILE|WORKFLOW-DIR` (commit `f7870a0`).
- `setup.sh` exports `WFROOT=~/code/words/final` (`setup.sh:14`), and the `wf`
  wrapper no longer `cd`s into the words repo -- it puts the repo on
  `PYTHONPATH` and runs `python -P`. Together those give the workflow root a
  name and restore the caller's working directory, which is what `-r` and every
  other relative path argument resolve against.
- `setops` `stable_mtime`, and `config.fold_classified` as the one way to write
  the classified aggregates. Rule 3 depends on this.

Milestone 0, still open:

- `complete` preflights every archive destination and requires `-f` on that
  invocation when an archived artifact would be replaced. Today the archive
  steps call `fs.move_into` / `fs.rename_once` one at a time with `ctx.force`,
  so a collision fails partway through the recipe instead of before it starts.
- `top-segments -n N` prints only the top N rows, defaulting to 1000, and
  replaces `| head -n N`.
  `print_counts` already builds the ordered vector, so this is a
  `std::partial_sort` and a bounded print loop. The pipeline is what makes
  `top.segments` unplaceable: `setops._place` takes one argv and no shell, so
  without `-n` it cannot produce the file at all, let alone with the content
  compare rule 3 needs. It also makes the exit status `top-segments`' rather
  than `head`'s -- through the pipe, a crashed `top-segments` yields a
  truncated file and exit 0 -- and removes the SIGPIPE the pipe delivers on
  every run.
- `wf init` creates an empty `.wf/classified/yes/yes.pairs` and
  `.wf/classified/no/no.pairs` when they are absent, so both standing
  aggregates always exist. Neither reader tolerates a missing one.
  `dfs-anagrams` treats a missing hard-NO aggregate as an error rather than an
  empty exclusion set, deliberately, so without this the first `gen dfs.seed`
  on any root fails. `common` and `diff` shell out to `comm`, which fails on a
  missing operand, so `gen best.pairs` fails the same way until some other
  target's `complete p2` happens to have created `yes.pairs` first -- and
  nothing creates it before then, since `fold_classified` writes it only when
  there is a verdict to fold. Both live roots are in exactly that state today:
  `~/code/words/.wf/classified/{yes,no}` are empty directories, and
  `$WFROOT` (`~/code/words/final`) has no `.wf` at all.

  Empty is the right initial value for both, not merely a convenient one.
  `load_pair_file` reads an empty file as `0 pairs, 0 keys` and succeeds, so
  `gen dfs.*` passes `--exclude-pairs <root>` unconditionally and has no
  branch; `comm` over an empty operand yields the empty intersection and the
  unchanged difference, which is what "nothing has been classified yet"
  means. The alternative -- every reader branching on existence -- puts the same
  three-way check in four places and makes an absent file mean "empty" in one
  and "error" in another.

  `gen best.pairs` warns when the result comes out empty -- the result, not
  this round's intersection, which under accumulation can be empty while the
  file is not. That is the signal that this target has never been reviewed and
  no other target has confirmed anything in its top segments, and it is not
  otherwise visible:
  `dfs-anagrams` zeroes `pair_bonus` only when `--pairs` is absent
  (`dfs-anagrams.cpp:295`), not when the file it names is empty, so
  `gen dfs.best` would run bonus-free while looking fully configured.
- `setops.diff` and `setops.common` gain the `stable_mtime` parameter `merge`
  and `fold` already have. `best.pairs` is placed by the `diff` -- the
  intersection goes to a scratch file beside the collated `top.segments` copy
  -- so `diff` is the one that must have it, and `common` gets it in the same
  commit because the two are one signature. Without it every regeneration of an
  unchanged `best.pairs` marks `dfs.best` stale.
- `-d` defaults to `$WFROOT` when it is set, falling back to `Path.cwd()`
  (`wf.py:50`). The variable is exported; nothing reads it yet.

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
  beside the default "outputs exist". `best.pairs` is the one step whose output
  is also an operand, and it must not appear in its own `inputs(ctx)`: a file
  is never older than itself, so declaring it would make the step permanently
  up to date.
- **The runner has no blocked state.** A step that is neither done nor runnable
  is exactly what the review gate is: `top.segments` is up to date, and
  `best.pairs` should not be built until a human checks notes -- it *would*
  build, from other targets' verdicts, and be silently incomplete. Stopping cleanly on
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

Two proposals from the superseded findings are deliberately out:

- **The annotated ranked view** -- `top.segments` shown with frequency,
  cumulative share, and prior verdict, which
  `findings/integrate-pairs-workflow-claude.md` called the one genuinely new
  primitive. It is a report over the pipeline rather than a stage in it, and it
  needs data this plan discards: `top-segments --pairs` prints bare pairs with
  no counts, so it would take a second output mode plus a join against
  `classified/{yes,no}`. Deferred, not rejected -- it is what would make a
  cutoff an informed choice rather than a guess.
- **Run-local suppression** -- codex's distinction between a global hard NO and
  "this pair dominates *this* exploration". `exclude` composes
  `wf classify no`, so every exclusion is a permanent global verdict. That is
  deliberate: a hard NO means the pair is never an answer, and a per-target NO
  set is exactly the record of which exclusions were in effect that this design
  refuses to keep -- the same refusal that makes the global aggregate's
  false-stale cost acceptable.

Also outside this plan: v2 stages 1 and 2 (candidate generation, the `comm`
against `p1_done.pairs`, chunking, and the `evalpair` cycle), which stay manual.

