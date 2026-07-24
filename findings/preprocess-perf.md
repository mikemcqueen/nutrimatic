# `dfs-anagrams` phase-2 preprocessing performance

## What happens after phase 1

The pause between

```text
# phase 1 complete: ...
```

and the first

```text
# phase 2: ...
```

message is mostly phase-2 preprocessing. `DfsAnagramSearch::run()`:

1. Encodes the remaining letter bag as a mixed-radix integer key.
2. Builds the packed `HotClass` representation.
3. Allocates the score-bound and fitting-candidate caches.
4. Recursively computes a memoized completion-score bound from the full bag.
5. Resets the phase-2 node counters and starts the reported DFS.

The score bound is

```text
H(empty) = 0

H(B) = max over classes c fitting B:
           best_member_log_score[c]
         + restart_log_rate
         + H(B - c)
```

`H` intentionally ignores the canonical DFS `entry_point`. It is therefore a
relaxed upper bound: it may admit completions the real search will reject, but
must never underestimate a real completion.

The prepass also populates the fitting-candidate cache. The subsequent DFS can
therefore reuse both the bound and candidate list for a remaining bag. This is
why the search can become extremely fast after a noticeable silent pause.

The preprocessing is absent from phase-2 progress statistics. The node and
solution counters are reset after `compute_score_bound()` returns. The first
progress message means that the real DFS has already visited
`100,000 * progress_factor` nodes.

The theoretical mixed-radix state count is

```text
product over symbols s of (input_multiplicity[s] + 1)
```

Only states reachable through fitting classes are computed, but their count
and transition fanout can grow rapidly with the bag.

## Eager versus lazy construction

A temporary `--lazy-score-bound` prototype delayed computing `H(B)` until:

- the node is below the root;
- the top-N spelling heap is full and exposes a score floor; and
- the DFS actually needs the bound.

Each lazy request still computes and memoizes the full transitive dependency
closure below that bag. The reported count is therefore the number of bound
states actually computed, not merely the number of direct DFS lookups.

The prototype reported

```text
N bound states computed lazily
```

The prototype and CLI option were removed after measurement. The normal eager
search retains the `N bound states computed` completion statistic.

Measurements used `idx/wiki-merged.5.index`, `-m 4`, and top 10,000:

| Bag | Eager states | Lazy states | Avoided |
|---|---:|---:|---:|
| `firestationteamused` (19 letters) | 18,380 | 17,979 | 401 |
| `firestationteamusedb` (20 letters) | 34,845 | 34,353 | 492 |
| `beginitwwherewarmwatersha` (25 letters) | 143,693 | 143,689 | 4 |

The eager and lazy stdout files were byte-identical in every comparison.

For the 25-letter case:

| Mode | Phase-2 nodes | Solutions visited | Single-run wall |
|---|---:|---:|---:|
| Eager | 6,513,120 | 25,240 | 5.04 s |
| Lazy | 6,513,225 | 25,240 | 5.46 s |

Lazy construction skipped only four states, or 0.0028%, and was slightly
slower. State count is not a complete CPU-cost proxy: high-level bags have
larger candidate buckets, and normal DFS work before a lazy request can also
populate candidate-cache entries that the bound computation later reuses.

## Sparse-table exhaustion at 30 letters

The first 30 characters of

```text
beginitwwherewarmwatershaltandtakeitinthecanyondown
```

produce this normalized bag:

```text
aaaabdeeeeghhiilmnnrrrstttwwww
```

With `-m 4`, phase 1 produced:

```text
459162 entries, 166852 classes, 3054340 trie nodes
```

The default 64 MiB cache budget gives score bounds at most one quarter of the
budget. When dense storage does not fit, the sparse score table has 1,048,576
slots and a 50% load limit: 524,288 stored states.

This 30-letter workload appears to reach that limit. On exhaustion, the eager
implementation discards the entire score table, gives the memory back to the
candidate cache, and runs phase 2 without score pruning. The aborted
measurement was stopped after:

```text
2,110,000,000 phase-2 nodes
351,578,227 solutions
```

No final output had yet been written. In this failure mode, all time spent
constructing the score bound is wasted for the eventual search. Predicting
exhaustion, growing the bound budget, retaining a useful partial table, or
constructing bounds lazily could avoid that all-or-nothing loss.

## Logarithms are not per bound state

`dfs-anagrams` uses `log()`, not `logf()`, for this scoring path.

Before bound construction, the `DfsAnagramSearch` constructor computes

```text
log(best_member_count)
```

once per extracted anagram class. It also computes two logarithms for the
restart rate. Additional logarithms occur later while expanding non-leading
class members into output spellings.

An instrumented 25-letter run with `-n 1` measured:

```text
35,711 classes
143,693 bound states
35,742 total log() calls
107,003,050 total nextafter() calls
```

Almost every logarithm was the one-time per-class calculation. The bound
recurrence itself calls no logarithm. Optimizing logarithms will not materially
reduce the preprocessing pause.

## The expensive work is per transition, not per state

For every fitting class transition considered by `H`, the current code:

1. Subtracts the class's packed letter requirements.
2. Recursively loads or computes the child bound.
3. Restores the bag.
4. Adds class score, restart score, and child bound in `long double`.
5. Rounds the candidate upward into `double`.
6. Calls `nextafter()` two unconditionally and sometimes three times.

The compiled x86-64 code uses x87 instructions for the `long double`
arithmetic and calls `nextafter()` through the PLT. The 107 million
`nextafter()` calls imply roughly 36--54 million successful bound transitions
for only 143,693 stored states. The exact transition count depends on how often
the initial `long double` to `double` conversion rounds downward and triggers
the optional third call.

Candidate fit scans, recursive memo lookups, bag subtract/restore, x87
arithmetic, and conservative rounding therefore scale with bound-graph edges,
not just memoized states.

## Why `long double` is used

On the measured Linux x86-64 build:

```text
sizeof(long double) = 16
LDBL_MANT_DIG       = 64
DBL_MANT_DIG        = 53
```

This is the x86 80-bit extended format stored in a padded 16-byte object. The
memo table still stores one 8-byte `double` per state; `long double` is used
only for transient arithmetic and the final prune comparison.

The extra precision is not needed to reproduce historical output scores.
`H` never contributes to an emitted score. It only determines whether a
subtree can safely be pruned. A looser bound, including positive infinity,
preserves output but prunes less.

The correctness requirement is:

```text
stored H(B) >= every realizable completion score below B
```

Underestimating `H` could incorrectly prune a winning spelling. The current
`long double` evaluation, upward conversion, extra ulps, and depth-scaled final
comparison padding are defensive measures that make the bound admissible.
`long double` specifically is not an absolute requirement if another
conservative floating-point error bound replaces it.

## Most promising optimization

The current implementation rounds every candidate edge before comparing it
with the best candidate for the state:

```text
for every fitting edge:
    candidate = round_up(class + restart + child)
    best = max(best, candidate)
store(best)
```

A much cheaper form is to retain the maximum in higher precision and round
only the winning value when the state is stored:

```text
long double best = -infinity
for every fitting edge:
    candidate = class + restart + child
    best = max(best, candidate)
store(round_up(best))
```

This would reduce conservative rounding from two or three `nextafter()` calls
per transition to two or three per stored state. On the measured 25-letter
case, the rough reduction is from 107 million calls to about 0.3--0.4 million.

This transformation needs a short rounding argument covering selection of the
maximum: an unselected candidate whose transient `long double` value rounded
slightly downward must still be bounded by the inflated stored maximum. The
existing extra ulps likely provide ample margin, but this must be proved and
validated rather than assumed.

After that change, replacing per-edge `long double` arithmetic with `double`
may be worthwhile. A safe implementation could:

- compute candidates in `double`;
- retain the largest computed candidate;
- inflate the stored state bound using an error allowance derived from operand
  magnitudes and remaining depth; and
- retain the depth-scaled padding in the final heap-floor comparison.

Emitted score calculations would remain unchanged. Safe changes may alter
phase-2 node, solution, and prune counts because a looser bound prunes less,
but retained stdout must remain byte-identical.

## Recommended measurement sequence

1. Add a low-overhead counter for successful bound transitions.
2. Time phase-2 setup separately from the real DFS.
3. Move upward rounding from each transition to each stored state.
4. Verify byte-identical stdout and retained spelling sets across dense and
   sparse modes and the documented reference workloads.
5. Recount `nextafter()` calls and profile the remaining preprocessing cost.
6. Prototype all-`double` candidate evaluation with an explicit conservative
   error envelope.
7. Investigate sparse-table exhaustion separately; an optimization that makes
   the doomed prepass faster still leaves all of its work unused at 30 letters.
