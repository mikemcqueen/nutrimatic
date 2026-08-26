# Integrating BEST PAIRS into the workflow tool

## Main recommendation

The right abstraction is a first-class, durable **BEST PAIRS run** keyed by
`(sentence, segment count)`. It should orchestrate the existing P2 workflow,
rather than becoming another classification phase or merely adding more
unrelated shell-command wrappers.

`docs/best-pairs-workflow-v2.md` describes the computational recipe well, but
the human is still serving as its scheduler, parameter store, status database,
and provenance tracker.

## What `wf` should remember

The workflow should retain state conceptually like this:

```text
sentence s2
  shared:
    letters
    P1 seed
    m=4, x=2
    index and dictionary
  segment count g4
    refinement round 2
    DFS result count 1000000
    candidate cutoff 1000
    current exclusions
    current ranked candidates
    P2 batch status
    current BEST PAIRS
    final DFS status
  segment count g5
    ...
```

The important scoping is:

- Sentence-level: letters, P1 seed, `m`, `x`, index, and dictionary.
- Segment-count-level: `g`, cutoff, DFS result count, current candidate
  universe, and final BEST PAIRS.
- Refinement-round-level: exclusions added, provisional DFS output, and ranked
  candidates.
- Global workflow knowledge: confirmed YES, explicit hard NO, and previously
  reviewed-but-unconfirmed pairs.

This removes almost all environment-variable setup and filename construction
after initial registration.

The durable state and smaller artifacts could live under something like
`.wf/best-pairs/`. The million-line DFS files could remain under Nutrimatic's
`results/` tree, with the workflow recording their canonical locations.
Centralizing the state matters more than copying every large output into the
workflow repository.

## The human-facing model

The ideal interaction is approximately:

1. Register a sentence and its segment counts once.
2. Ask for status.
3. Run the next mechanical stage for one segment count.
4. Inspect the ranked candidates.
5. Explicitly choose either to exclude some entries and begin another
   refinement round, or send the viable candidate set to manual review.
6. Review notes.
7. Complete the review.
8. Produce the complete BEST PAIRS artifact.
9. Run final DFS.

Illustrative commands might be:

```text
wf best-pairs status
wf best-pairs run s2 4
wf best-pairs exclude s2 4 FILE
wf best-pairs review s2 4
wf best-pairs complete s2 4
wf best-pairs final s2 4
```

The exact vocabulary can come later. The high-value command is probably:

```text
wf best-pairs next [s2 [4]]
```

It should report the current state and the one next meaningful action:

```text
s2/g4: refinement round 2
provisional DFS: complete
top 1000: complete
P2 review: not submitted

next: inspect .../top.s2.m4.g4.1000.pairs
then either:
  wf best-pairs exclude s2 4 FILE
  wf best-pairs review s2 4
```

The summary view should be a sentence-by-segment-count matrix rather than the
raw directory listing that current `wf show` provides.

The existing `submit`, `eval`, `complete`, and `extract` commands should remain
useful lower-level escape hatches. The new layer should compose them.

## A significant reuse gap in version 2

There is a subtle conflict between avoiding repeat review and producing a
complete per-segment-count BEST PAIRS file.

Version 2 says that normal P2 filtering can avoid repeated work across segment
counts and increments. It then treats that one bundle's `*.p2.yes` as the
complete BEST PAIRS artifact. Those cannot both be true:

- Suppose `wooden,toy` was confirmed YES while reviewing `g4`.
- It appears in the `g5` top 1000.
- Normal P2 filtering removes it because it was already reviewed.
- Therefore the `g5` bundle's `*.p2.yes` does not contain it.
- Yet it should be present in the complete `g5` BEST PAIRS list.

Using `--no-filter` avoids the missing entry only by making the human review
known entries again, including both known YES and prior soft NO.

The run-level solution is:

```text
BEST PAIRS =
    current run candidate universe
    intersect
    global confirmed-YES set
```

After reviewing the unknown candidates, `wf` should materialize that
intersection automatically. The existing global confirmed-YES aggregate
already provides the reusable knowledge.

The same mechanism naturally supports candidate increments:

- The top 1000 contains known YES, soft NO, hard NO, and unknown entries.
- Known YES is automatically included.
- Soft NO is skipped by default, with an explicit reconsider option.
- Hard NO is excluded.
- Only unknown entries are queued for review.
- Increasing the cutoff to 2000 queues only newly encountered unknown entries.
- BEST PAIRS is regenerated from the complete current prefix, rather than
  merely the latest review batch.

That is a much better reduction in manual work than broadly using
`--no-filter`.

## Refinement rounds

The workflow should preserve a small record of every refinement round even if
the huge provisional DFS output is replaced:

- Parameters used.
- Hard-NO additions.
- Ranked candidate file.
- Counts and timestamps.
- Whether the round was accepted or superseded.

That provides useful answers to "why am I on round 3?" without retaining
several million-line DFS outputs.

There should also be a distinction between:

- **Global hard NO:** this pair is semantically invalid everywhere.
- **Run-local suppression:** this pair is undesirable or overly dominant for
  this particular exploration.

The version 2 hard-NO mechanism is global. That is correct for genuinely
invalid pairs, but dominance alone may not always justify a permanent global
classification.

Once refinement ends, the candidate universe should be the chosen prefix of
the latest accepted ranking. Earlier reviews remain reusable global knowledge,
but an entry from a superseded ranking should not automatically remain in this
segment count's final BEST PAIRS unless it appears in the accepted candidate
universe.

## Implementation order

1. Implement the explicit version 2 prerequisites: neutral P2 input,
   `wf classify no`, exact DFS pair exclusion, and archive preflighting. These
   are still prerequisites in the current checkouts; current P2 evaluation
   accepts only `*.p1.yes`, and Nutrimatic does not yet have
   `--exclude-pairs`.
2. Add the run registry and useful status view before adding broad automation.
   Merely knowing what is queued, refining, under review, ready for final DFS,
   or done would remove substantial cognitive load.
3. Add candidate partitioning and BEST PAIRS reconstruction. This is the key
   cross-segment-count and incremental-review primitive.
4. Add refinement-round operations and automatic command construction.
5. Optionally let `wf` execute `dfs-anagrams` and `top-segments`, using
   temporary outputs promoted only after successful completion.

## Open question

Should `wf best-pairs run s2 4` actually invoke and wait for the Nutrimatic
programs, or should `wf` own the state while printing and registering the exact
long-running command?

The initial recommendation is that it should execute the deterministic
commands itself, while retaining explicit human gates for exclusions, review
submission, and finalization.
