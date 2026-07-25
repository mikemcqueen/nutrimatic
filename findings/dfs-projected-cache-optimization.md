# Projected score-bound construction optimization

## Result

The largest measured win in this whole area is not in the projected builder at
all. It is a length-group certificate in the concrete DFS, which exactly
removes 49--91% of the class fit tests phase 2 performs — 84--96% with its
optional score-descending refinement — using about 15 KiB of tables. It is
implemented behind `NUTRIMATIC_LENGTH_CERTIFICATE=1` and
`NUTRIMATIC_LENGTH_CERTIFICATE_SUFFIX=1` and is measured in
"Concrete-search length-group certificates" below. On the four measured
workloads it improves total phase 2 by 1.04x, 2.4x, 3.7x, and 32x relative to
the current production shape selector, and it moves the best projection depth
down by one to six dimensions. Every run reproduced its established output
SHA-256.

That result subsumes most of what follows: the setup-versus-search tradeoff
that the depth sweeps below measure was measured with an uncertified search,
and the certificate halves to quarters the search side of it.

The projected recurrence should operate on projected actions, not concrete
anagram classes.

Several concrete classes can consume the same exact-letter multiplicities and
the same number of wildcard letters. They therefore have the same projected
key delta and the same child state. Only the largest class score can affect
the recurrence's maximum; every lower-scoring class with that delta is
dominated.

The implementation now builds one action per distinct projected delta and
forced-exact-letter bucket. It retains the highest class score for that action.
This changes preprocessing work without changing the projected bound.

The measurements below establish that this optimization is real, but they are
not the main subject of the remaining work. The research question is now how
to avoid constructing so many action/state edges and abstract states in the
first place, rather than which adjacent projection depth wins a timing sweep.

Table shape is nevertheless an immediate policy issue. The current
largest-fitting selector overbuilds the measured larger workloads. A fixed
one-dimension backoff looked adequate on the first `S6` sweeps, but an
unrelated-bag validation rejects it as a general policy. Moving one letter
kind into the wildcard bucket can leave the abstraction exactly isomorphic,
and the best measured unrelated shape was four dimensions below the largest
fitting shape. Selection must be based on predicted construction and search
work, not cache occupancy or dimension count.

Construction work now has a practical pre-build predictor. Summing, for every
deduplicated action, the number of logical mixed-radix states in which it fits
counts the complete projected box's potential fitting edges without building
the box. Calibrating that count with one coarse completed projection predicts
the measured richer transition counts within 0.3--17.7% across the current
sweeps. The remaining selector uncertainty is chiefly pruning value: a
bounded concrete-search or richer-lookup pilot is still needed before changing
the production depth policy.

A length-only hierarchical certificate can also remove a material fraction of
the rich recurrence's action edges exactly. The shadow experiment certifies
30--62% of fitting edges in the current traversal order on the measured bags.
Actually skipping them reduces successful transitions by 32--66% while
retaining byte-identical output and final DFS node counts. It does not,
however, combine well with the current parallel root-closure scheduler:
skipped children leave table slots unbuilt, and constructing those exact
bounds synchronously on first concrete demand moves work from parallel setup
onto the single DFS thread. The serial experiment improves 12.9%; the
20-thread experiments regress despite doing much less mathematical work.
Certificates should therefore be carried forward into a parallel layered or
nonblocking demand scheduler, not enabled as a local recursive-loop shortcut.

A nonblocking scheduler cannot rely on a high aggregate fallback-prune rate.
When certificate-created holes use the complete 9-bit x 2 modular bound
immediately, that fallback prunes 97.8--99.9% of hole lookups but exposes
3.4--9.5x as many final DFS nodes as complete rich coverage. The rare misses
are concentrated above large subtrees. Widening the quotient to ten bits does
not materially change the result. A background builder must close important
holes promptly, and its priority signal must include subtree leverage or a
bounded-search observation; raw query count and prune coverage are
insufficient. A complete small exact-letter projection remains the safer
always-available fallback.

Dead projected work is sharply localized by remaining-letter layer. Across
two `S6` projections and the unrelated-bag validation, 99.97--100% of edges
to dead children land in layers with at most seven letters remaining. Those
low layers contain only 5.5--10.1% of the forward-constructed states. This
does not justify a full reverse builder, because reverse generation can still
create root-irrelevant states. It does justify a bounded reverse-completable
perimeter through layer seven: measure its generated size and use its
finite/dead mask to avoid the 6.7--10.3% of fitting edges that currently
discover dead children recursively. The preflight enumeration now confirms
that this perimeter is compact: it contains only 1.31--1.48x as many finite
states as the forward traversal reaches in those layers, and a direct
full-key-space bitset costs 16--105 KiB on the measured cases.

## Measurements

Unless otherwise noted, measurements use `idx/wiki-merged.5.index`, prefixes
of `S6`, `-m 4 -n 1000`, and a warm index. The 20-thread results use `-T 20`.

### 28 letters, `d=14`, `-C 8`

| builder | threads | setup | successful transitions |
|---|---:|---:|---:|
| concrete-class traversal | 1 | 21.929s | 292,735,991 |
| projected-action traversal | 1 | 11.706s | 213,302,595 |
| concrete-class traversal | 20 | 2.314s | 292,735,991 |
| projected-action traversal | 20 | 1.138s | 213,302,595 |

The optimized projected output is byte-identical to the complete exact dense
table's top-1000 output.

### 40 letters, `d=15`, `-C 32`, 20 threads

| builder | setup | search | phase 2 | successful transitions |
|---|---:|---:|---:|---:|
| concrete-class traversal | 226.549s | 20.207s | 246.757s | 27,133,454,068 |
| projected-action traversal | 29.736s | 20.776s | 50.512s | 5,919,322,956 |

The projection contains 151,440 distinct actions derived from 490,329 anagram
classes. Setup is 7.6x faster and total phase 2 is 4.9x faster. Final DFS node
and solution counts are unchanged: 342,949,072 nodes and 24,804 solutions.

At `d=16`, action deduplication reduces 490,329 classes to 222,643 actions.
The measured setup before the final instruction-level changes was 115.664s,
down from 510.705s, and transitions fell from 51.882 billion to 18.671
billion.

The output SHA-256 was identical for the measured `d=13`, `d=14`, `d=15`, and
`d=16` runs.

### Timing reproducibility

The transition counts are deterministic, but the wall-clock measurements are
not a permanent baseline. A newer local `d=15`, 40-letter run recorded
47.757s setup and 36.558s search with the same 5,919,322,956 successful
transitions, 342,949,072 final DFS nodes, and 24,804 solutions as the 29.736s
setup run above. The algorithmic comparisons should therefore use transition
and node counts alongside controlled paired timing runs.

A later local timing set recorded:

| letters | `-C` | `d` | actions | states built | setup | search | phase 2 | transitions | final DFS nodes |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 38 | 32 MiB | 15 | 124,513 | 2,984,149 | 32.796s | 9.979s | 42.775s | 4,159,322,534 | 105,067,790 |
| 40 | 32 MiB | 15 | 151,440 | 3,411,183 | 47.757s | 36.558s | 84.315s | 5,919,322,956 | 342,949,072 |
| 40 | 128 MiB | 16 | 222,643 | 8,787,405 | 169.550s | 19.446s | 188.996s | 18,670,931,559 | 183,381,681 |

All three runs report 20 preprocessing workers. The two 40-letter runs form
the most useful comparison within this set: `d=16` halves neither setup nor
search. It reduces final DFS nodes by 46.5% and search time by 46.8%, but
increases construction transitions by 3.15x and setup time by 3.55x. Total
phase 2 is consequently 2.24x slower than `d=15`, despite using the larger
cache and producing the tighter bound.

Later investigation established that other `dfs-anagrams` instances sometimes
run concurrently on this host. The timings above and the initial support-group
timings below were not preceded by a host-process check, so they are
observations rather than controlled speedup evidence. Future timing runs must
check the host process table immediately before the run and during long runs.
Deterministic state, transition, node, and solution counts are unaffected.

## Data layout and hot-loop changes

The action metadata is split into two 16-byte, 64-byte-aligned arrays:

- fit metadata: exact support, exact-requirement offset and counts, and
  wildcard length;
- value metadata: projected key delta and class score.

An unsuccessful fit test touches only fit metadata. Exact requirements are
copied into a projection-specific packed array, so the recurrence no longer
branches over wildcard requirements while mutating and restoring a worker
bag. Actions are stored contiguously by their rarest required exact letter,
with a separate wildcard-only bucket.

The temporary action storage is released after bound construction. It is not
part of the final DFS working set.

Callgrind attributed 70.3% of post-dedup instructions on the 24-letter serial
workload to the successful projected-transition kernel. Two smaller changes
target that path:

- compute the conservative floating-point error envelope once per state from
  separately conservative maximum magnitudes, instead of several arithmetic
  operations per edge;
- prevent GCC from partially inlining the recursive cache-miss path into the
  overwhelmingly common cache-hit transition path.

The host's WSL kernel has no usable `perf` PMU support, so hardware cache and
branch-miss counters were not available.

## What remains in the current recurrence

The action reduction implemented here recognizes only single-action
equivalence: actions with the same forced bucket and projected delta reach the
same child, so only their maximum score survives.

After that reduction, every fitting action is still evaluated. A successful
evaluation mutates the projected bag, loads or recursively constructs the
child, restores the bag, and contributes to the maximum. On the 40-letter
`d=15` workload this is still 5.919 billion successful transitions over
3.411 million constructed states, or roughly 1,735 successful transitions per
constructed state.

Projected construction also remains eager. It starts from every fitting root
action and constructs the resulting abstract dependency closure before the
concrete DFS has produced a score floor. Consequently, the current
implementation has not exhausted either of the two main ways to reduce setup:

1. prove that fewer action-to-child transitions need evaluation; or
2. construct bounds for fewer abstract states.

## Support-subset traversal experiment

The first diagnostics and fit-index prototype are opt-in:

```sh
NUTRIMATIC_PROJECTED_DIAGNOSTICS=1
NUTRIMATIC_PROJECTED_SUPPORT_GROUPS=1
```

Diagnostics partition action scans into wildcard-length, exact-support, and
multiplicity rejections, and partition fitting edges into successful and dead
children. They also count ready child loads, first-owner state claims,
ownership conflicts, dependency-spin iterations, and finite/dead constructed
states. Counters are worker-local during construction and merged afterward.
The ordinary projected loop remains unchanged when diagnostics are disabled.

The support-group prototype compresses the selected exact-letter support into
a `d`-bit key and groups action IDs by their exact support. At a state with
support `M`, it enumerates only submasks of `M` containing the forced rarest
bit. Those are exactly the action-support groups that can pass the existing
support test and obey forced-letter canonicalization. When no exact letter
remains it visits only the wildcard-only group.

The group index is temporary preprocessing metadata. On the 28-letter,
`d=14` case it contains 16,385 offsets and 73,522 action IDs: 425,168 bytes
(415.2 KiB) with the current 8-byte offsets. It is released with the other
projected action metadata before the concrete DFS.

### 28-letter measurement

These are paired runs on the same `S6` prefix with `-m 4 -n 1000 -C 8 -F`.
Every variant constructed 430,123 states, performed 213,302,595 successful
transitions and 994,904 `nextafter` calls, visited 3,086,650 final DFS nodes,
and produced byte-identical top-1000 output.

| traversal | threads | diagnostics | setup | search | action scans |
|---|---:|---|---:|---:|---:|
| bucket scan | 20 | off | 1.994s | 0.237s | not counted |
| support subsets | 20 | off | 1.397s | 0.208s | not counted |
| bucket scan | 20 | on | 2.365s | 0.211s | 4,787,820,579 |
| support subsets | 20 | on | 1.481s | 0.213s | 473,063,458 |
| bucket scan | 1 | off | 20.367s | 0.211s | not counted |
| support subsets | 1 | off | 10.538s | 0.209s | not counted |

Support grouping removes 90.1% of actual per-action fit checks. Setup improves
29.9% in the clean 20-thread pair and 48.3% in the clean serial pair. The
larger serial improvement is consistent with parallel construction hiding
some scan cost.

The ungrouped diagnostic partition was:

| outcome | count | share of scans |
|---|---:|---:|
| wildcard length rejected | 1,086,369,610 | 22.7% |
| exact support rejected | 3,332,842,204 | 69.6% |
| exact multiplicity rejected | 132,125,145 | 2.8% |
| fitting edge | 236,483,620 | 4.9% |

Of the fitting edges, 23,181,025 (9.8%) reached a dead child. Only 11,615 of
430,123 constructed states (2.7%) were dead. Reverse-only construction is
therefore unlikely to remove much state work on this case by itself, although
dead-child certificates could still avoid a meaningful fraction of fitting
edges.

The ungrouped run recorded 25,527 ownership conflicts among 236.5 million
child lookups, so conflicts are rare. The same conflicts accumulated
124,055,861 pause-loop iterations, which means a blocked lookup can wait a
long time; elapsed wait cycles are still needed before justifying a
continuation scheduler. Support grouping changed traversal order and recorded
98,301 conflicts but fewer pause iterations (101,767,621), while still
finishing sooner. Conflict frequency alone is not a useful optimization
objective.

The 38-letter, `d=15`, 20-thread validation also produced byte-identical
top-1000 output and identical state, transition, final-node, and solution
counts. Support grouping reduced setup from 30.367s to 23.852s (21.5%). The
two runs' search times were 7.895s and 19.063s despite identical node counts,
another example of the host's timing variability. The `nextafter` count
differed by two out of about 7.26 million because the group traversal changes
action order.

The 40-letter, `d=16`, 128 MiB validation also retained exactly 8,787,405
states, 18,670,931,559 successful transitions, 183,381,681 final DFS nodes,
and byte-identical top-1000 output. Ungated support-group runs recorded
131.075s and 112.012s setup versus an earlier ungated 169.550s reference; these
are not a controlled comparison. A later run checked before launch and during
construction found no competing `dfs-anagrams` and recorded 93.128s, but lacks
a similarly gated ungrouped pair. The support index contained 65,536 groups
and 222,643 action IDs and took 8.5--9.7ms to build.

The evidence promotes support-subset traversal from a speculative fit index to
a strong candidate for the projected builder. Index-build time is now reported
separately from recurrence construction.

### Wildcard-length ordering experiment

Sorting every support group by wildcard consumption allows its scan to stop at
the first action longer than the state's wildcard remainder. On the 28-letter
diagnostic case this behaved exactly as intended at the scan level: it removed
104,454,693 of 473,063,458 checks (22.1%), leaving 368,608,765. State,
transition, final-node, solution, and output counts were unchanged. Building
and sorting the 16,384-group index took 2.5--3.1ms.

The optimization was nevertheless a regression in the clean serial paired
runs:

| traversal within each support group | setup run 1 | setup run 2 |
|---|---:|---:|
| original projected-action order | 10.324s | 10.289s |
| wildcard length, then action ID | 10.797s | 10.854s |

The mean setup penalty was 5.0%. The successful transitions dominate this
workload, and their traversal order affects recursive construction and cache
locality. Removing cheap rejected checks did not compensate for the worse
successful-edge order. The wildcard ordering is therefore not retained.
Future fit indexes should preserve the existing successful-action order or
demonstrate a more valuable ordering objective; scan-count reduction alone is
not sufficient evidence. These paired runs also predated the host-process
gate, so the timing magnitude is tentative even though both pairs consistently
favored the original order.

## Final-query demand experiment

Final-query diagnostics are independently enabled with:

```sh
NUTRIMATIC_PROJECTED_QUERY_DIAGNOSTICS=1
```

They use a temporary one-bit-per-table-slot set and count concrete-DFS bound
lookups, distinct projected keys, lookups that prune, and DFS nodes reached
before the top-N sink has a score floor. Full projected-builder diagnostics
also enable these query counters; the separate switch avoids instrumenting
billions of construction edges when only final demand is needed.

| workload | states built | unique queried | built queried | bound lookups | rich prunes | length-only prunes | modular preserves | nodes before floor |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 28 letters, `d=14` | 430,123 | 324,005 | 75.3% | 3,086,592 | 3,080,422 (99.80%) | 1,592,898 (51.7%) | 2,834,857 (92.0%) | 57 |
| 40 letters, `d=16` | 8,787,405 | 5,202,667 | 59.2% | 183,381,299 | 183,299,923 (99.96%) | 74,271,639 (40.5%) | 144,571,967 (78.9%) | 381 |

Eager construction built 106,118 states never queried in the smaller case and
3,584,738 never queried in the larger case. This establishes meaningful
state-selection headroom, especially at `d=16`, but it is not a direct estimate
of lazy construction work: a queried state's exact rich value may require
unqueried descendants. The score floor also arrives almost immediately and
the DFS then issues bound queries at nearly every node, so a background
refiner would face demand quickly rather than enjoying a long warm-up window.

The very high prune rate makes admissible fallback quality critical. A
length-only recurrence was evaluated in shadow mode on every rich lookup. It
missed 1,487,524 rich prunes in the smaller case and 109,028,284 in the larger
case. It produced zero coarse-only prunes, as required by its relaxation, but
preserved only 51.7% and 40.5% of rich pruning respectively.

This rejects length-only fallback as the sole basis for online refinement.
Saving projected states would expose tens of millions of concrete nodes until
refinement caught up. Before implementing a lazy/background builder, test a
stronger still-cheap fallback—such as a second small projection or several
weighted scalar projections—against this same disagreement counter. Bound-gap
histograms would help distinguish near misses from cases needing much more
abstraction detail.

The first stronger shadow fallback uses four independent tables keyed by
remaining length and a deterministic 6-bit modular multiset signature. Each
table contains only 64 states per length layer. It deduplicates concrete
classes by `(length, signature delta)`, relaxes all count and fit constraints,
and computes a complete max-plus recurrence. Each table is independently
admissible, so their minimum is admissible and can also complement the rich
projection.

This fallback preserved 92.0% of rich prunes at `d=14` and 78.9% at `d=16`,
far better than length-only but still missing 245,565 and 38,727,956 rich
prunes respectively. It also proved 552 and 4,844 prunes that the rich
projection missed, confirming complementary value. The `d=16` modular run was
host-process-gated at launch and during construction and produced
byte-identical output.

Four 6-bit signatures are therefore promising as an additional complementary
bound, but not yet strong enough as the only fallback for absent rich states.
The next cheap sweep should vary signature width, hash selection, and table
count while reporting rich-prune coverage per table byte and per final-query
operation. If coverage saturates well below 100%, a small exact-letter
projection is a better fallback candidate.

### Modular-signature parameter sweep

The modular diagnostic now accepts:

```sh
NUTRIMATIC_PROJECTED_MODULAR_BITS=3..10
NUTRIMATIC_PROJECTED_MODULAR_COUNT=1..8
NUTRIMATIC_PROJECTED_MODULAR_SEED=unsigned-integer
```

The defaults remain four 6-bit tables with seed zero. The diagnostic reports
cumulative pruning after each table, table and per-class-delta bytes,
preparation time, distinct relaxed actions, and deterministic candidate scans.
Per-class deltas use one byte per class and table through eight bits and two
bytes for nine- and ten-bit signatures.

The first sweep used the 28-letter `d=14` final-query stream above and eight
tables of each width. Every run retained 430,123 rich states, 213,302,595 rich
construction transitions, 3,086,650 final DFS nodes, 3,652 solutions, and the
same shadow-only final-search behavior. The runs were checked for another
`dfs-anagrams` process before launch. Timing is omitted because both the host
and the amount of shadow diagnostic work vary; the prune counts are
deterministic.

| bits | table bytes | table + delta bytes | rich prunes preserved | rich prunes missed |
|---:|---:|---:|---:|---:|
| 4 | 29,696 | 771,248 | 86.06% | 429,563 |
| 5 | 59,392 | 800,944 | 90.26% | 299,987 |
| 6 | 118,784 | 860,336 | 93.57% | 198,011 |
| 7 | 237,568 | 979,120 | 95.84% | 128,257 |
| 8 | 475,136 | 1,216,688 | 97.31% | 82,895 |
| 9 | 950,272 | 2,433,376 | 98.29% | 52,588 |

Width and table count should not be selected independently. At approximately
equal table storage, fewer wider tables consistently did better:

| table storage | configuration | rich prunes missed | lookup loads |
|---:|---|---:|---:|
| 118,784 B | 6-bit x 8 | 198,011 | 8 |
| 118,784 B | 7-bit x 4 | 163,022 | 4 |
| 118,784 B | 8-bit x 2 | 151,149 | 2 |
| 118,784 B | 9-bit x 1 | 172,129 | 1 |
| 237,568 B | 7-bit x 8 | 128,257 | 8 |
| 237,568 B | 8-bit x 4 | 106,174 | 4 |
| 237,568 B | 9-bit x 2 | 97,643 | 2 |
| 237,568 B | 10-bit x 1 | 116,675 | 1 |
| 475,136 B | 8-bit x 8 | 82,895 | 8 |
| 475,136 B | 9-bit x 4 | 67,076 | 4 |
| 475,136 B | 10-bit x 2 | 63,214 | 2 |
| 950,272 B | 9-bit x 8 | 52,588 | 8 |
| 950,272 B | 10-bit x 4 | 43,524 | 4 |

The comparison is even more favorable to fewer tables when delta storage is
included: each table adds 92,694 bytes of class deltas through eight bits and
185,388 bytes at nine or ten bits on this workload.
The best point depends on how many final-query loads and how much construction
work are acceptable, but the original 6-bit x 4 configuration is not on the
measured storage/operation frontier.

Nine-bit x 2 is the best cheap measured point. It preserves 96.83% of rich
prunes using 608,344 total bytes and two final-query loads. It deduplicates to
6,261 and 6,284 relaxed actions, performs 179,845,120 deterministic candidate
scans, and prepared in 0.216s in its gated run. Ten-bit x 2 improves
preservation to 97.95% and uses 845,912 total bytes, but expands to 669,261,824
candidate scans and took 0.819s. That is 3.72x the construction scans for
34,429 additional matched rich prunes. Ten-bit x 4 preserves 98.59% but already
performs 1.334 billion candidate scans and took 1.631s, comparable to the rich
builder itself on this small case. Wider is therefore not free even though
the final tables remain small.

Changing the deterministic hash seed did little. For 9-bit x 4, seeds zero
through three missed 67,076, 68,071, 68,027, and 68,162 rich prunes. The full
range is only 1.6% of the best miss count. Random seed search is therefore a
low-priority way to improve this quotient. A deliberately selected weight
vector should be judged by whether it separates the actual remaining
rich-only states, not by generic hash quality.

The wider quotient improves the fallback conclusion but does not reverse it.
Even 9-bit x 4 misses 67,076 rich prunes on this small query stream, and 9-bit
x 8 misses 52,588; 10-bit x 4 still misses 43,524. Those misses can expose
more than one concrete node each when the fallback is used for real.

The launch- and construction-gated 40-letter `d=16` validation makes that
limitation clearer:

| fallback | table bytes | delta bytes | prepare | candidate scans | rich prunes preserved | rich prunes missed |
|---|---:|---:|---:|---:|---:|---:|
| 6-bit x 4 | 83,968 | 1,961,316 | not separately timed | not recorded | 78.87% | 38,727,956 |
| 9-bit x 1 | 167,936 | 980,658 | included below | 173,690,880 | 84.74% | 27,965,638 |
| 9-bit x 2 | 335,872 | 1,961,316 | 0.480s | 347,402,240 | 89.37% | 19,492,901 |

The rich run retained 8,787,405 states, performed 18,670,931,559 rich
construction transitions, visited 183,381,681 final DFS nodes, and issued
183,381,299 bound lookups. The two modular tables additionally proved 11,955
prunes that the rich projection missed. Phase 2 reported 110.671s setup
(including 0.480s modular preparation) and the instrumented final search
20.437s, but the deterministic work and prune counts are more portable than
those host timings.

Cutting rich-only misses in half relative to 6-bit x 4 is useful, but leaving
19.5 million misses rules out even the improved modular quotient as the only
large-workload fallback. Use 9-bit x 2 as a cheap complement and as one stage
in a fallback cascade. This made a small exact-letter projection the next
fallback comparison; the live-pruning sweep below measures concrete nodes
exposed rather than only per-lookup disagreement.

### Small exact-letter fallback under live pruning

Forced smaller projections were run as the actual phase-2 bound, rather than
as a per-lookup shadow diagnostic. Their final DFS node counts therefore
measure the concrete work exposed by weakening the projection. This avoids
trying to infer node expansion from prune-disagreement counts.

The first sweep used the established 28-letter workload with support-subset
traversal and 20 preprocessing threads:

| `d` | table bytes | actions | states built | setup | search | phase 2 | construction transitions | final DFS nodes |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 6 | 6,080 | 1,390 | 1,040 | 0.020s | 15.330s | 15.349s | 55,954 | 188,177,315 |
| 8 | 31,104 | 4,823 | 4,750 | 0.032s | 4.874s | 4.906s | 611,754 | 65,099,399 |
| 10 | 110,592 | 12,103 | 15,861 | 0.102s | 1.958s | 2.060s | 3,316,664 | 28,809,173 |
| 11 | 290,304 | 20,815 | 39,439 | 0.237s | 1.181s | 1.418s | 10,905,285 | 17,854,432 |
| 12 | 539,136 | 30,774 | 71,899 | 0.213s | 0.777s | 0.990s | 23,327,436 | 11,331,110 |
| 13 | 1,368,576 | 50,687 | 178,416 | 0.635s | 0.401s | 1.036s | 75,624,933 | 6,059,101 |
| 14 | 3,359,232 | 73,522 | 430,123 | 1.243s | 0.214s | 1.457s | 213,302,595 | 3,086,650 |

All seven outputs had SHA-256
`de32cca977192c1ab65b56329c9a7f8f97e25441fbea1e39c049b7ebf8c33ee6`.
The deterministic solution counts increase as the bound weakens because more
below-floor complete solutions are reached, but the retained top 1000 are
unchanged.

On this case `d=12` is the best measured point and is better than the
previously selected `d=14` even without online refinement. It uses 6.2x less
table storage and 9.1x fewer construction transitions, exposes 3.7x as many
final nodes, and still reduces total phase 2 by 32.0%. Adjacent `d=13` is
within 4.7%, which is smaller than the host's known timing variability, but
both clearly beat `d=14`. Even `d=10`, at only 108 KiB, is within 1.42x of
the `d=14` end-to-end time while doing 64x fewer construction transitions.
The weakest points show that exact-letter information degrades gracefully for
several depth steps, but not indefinitely: `d=6` exposes 188 million nodes.

The 28-letter timings were process-gated at launch but were not all monitored
throughout; a sibling worktree began unrelated `dfs-anagrams` runs later in
the sweep. Use the deterministic work counts as the portable evidence and the
timings as an initial shape.

The larger validation used 38 letters, `-C 32`, support-subset traversal, and
20 preprocessing threads. Each run was checked before launch and while it was
active; only the measured instance was present.

| `d` | table bytes | actions | states built | setup | search | phase 2 | construction transitions | final DFS nodes |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 13 | 2,322,432 | 44,843 | 286,429 | 1.011s | 50.826s | 51.837s | 189,115,902 | 540,441,307 |
| 14 | 9,400,320 | 82,327 | 1,151,524 | 5.832s | 16.415s | 22.248s | 1,277,019,105 | 203,485,601 |
| 15 | 24,883,200 | 124,513 | 2,984,149 | 19.814s | 7.807s | 27.621s | 4,159,322,534 | 105,067,790 |

All three outputs had SHA-256
`398abaeaeb5245dfe071f1f11d933742591230b685a6be2ac724abc35a4ffec4`.

Here `d=14` is the best measured point. Relative to `d=15`, it cuts table
storage by 2.65x, constructed states by 2.59x, transitions by 3.26x, and setup
by 70.6%. Its 1.94x larger final traversal raises search from 7.81s to 16.42s,
but total phase 2 is still 19.5% faster. `d=13` is a plausible fast emergency
fallback—it is ready in about one second—but is too weak as the sole bound on
this workload.

### Intermediate depth-selection validation

A 34-letter sweep fills the gap between the 28- and 38-letter cases. It used
`-C 32`, support-subset traversal, and 20 preprocessing threads. The `d=12`
run and a repeated `d=13`/`d=14` pair were checked against the host process
table before launch and during each run; only the measured instance was
present.

| `d` | table bytes | actions | states built | setup | search | phase 2 | construction transitions | final DFS nodes |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 12 | 1,658,880 | 46,671 | 278,309 | 1.028s | 17.884s | 18.912s | 181,103,560 | 249,546,057 |
| 13 | 4,354,560 | 74,365 | 723,914 | 3.157s | 10.495s | 13.652s | 619,235,789 | 154,009,521 |
| 14 | 11,197,440 | 111,306 | 1,830,525 | 10.071s | 5.537s | 15.607s | 1,904,727,125 | 76,881,663 |

All three outputs had SHA-256
`9a71f496f06e1851c4ab8ac268a66dea5727f90a898a9d35cefd97ca08af0cb4`.
An initial ungated pair had the same deterministic counts and also favored
`d=13`: 13.320s versus 14.898s total phase 2.

Here the current largest-fitting selector chooses `d=14`. A forced `d=15`
would require 33,592,320 bytes, only 37,888 bytes more than the 32 MiB budget,
but that near fit is not desirable: `d=13` is still 12.5% faster than `d=14`.
It reduces construction transitions by 67.5% while exposing 2.00x as many
final nodes. The 6.91s setup saving exceeds the 4.96s search penalty.

The deterministic work also explains the timing knee. Across the three
controlled runs, setup costs 5.1--5.7ns per successful construction
transition and search costs 68--72ns per final DFS node. A simple linear proxy
using those two counts selects `d=13`, so the wall-clock result is not caused
by an anomalous phase timing.

### Depth-selection policy

Across the now-measured `S6` prefixes, choosing one exact-letter dimension
below the largest table that fits initially looked like a robust simple
policy:

| letters | largest fitting `d` | best measured `d` | one-step-backoff result |
|---:|---:|---:|---|
| 26 | 12 | 12 | 0.9% slower than best |
| 28 | 14 | 12 | 4.6% slower than best, 28.9% faster than largest |
| 34 | 14 | 13 | best, 12.5% faster than largest |
| 38 | 15 | 14 | best, 19.5% faster than largest |

The earlier 40-letter comparison points the same way: `d=15` was much faster
end to end than `d=16`, although that timing set was more variable. A one-step
backoff is not always the exact optimum, but in the tabulated sweep it is
within 5% of the best point and avoids every material overbuild. Blindly
spending the last cache bytes does not. This was evidence for testing the
heuristic, not enough evidence to install it.

A first composition check used the 28-letter window `S6[24:52]` rather than a
prefix:

| `d` | table bytes | states built | setup | search | phase 2 | construction transitions | final DFS nodes |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 13 | 1,622,016 | 177,490 | 0.340s | 0.387s | 0.727s | 63,204,880 | 4,754,917 |
| 14 | 2,949,120 | 298,850 | 0.587s | 0.240s | 0.827s | 124,763,129 | 2,889,429 |

Both outputs had SHA-256
`5f1bba997e11c2612f0f6cdc918d1f54339fc0c6e3017c098a99d49db790dde9`.
The one-step backoff again won, by 12.1% observed. A sibling's next
`dfs-anagrams` run appeared immediately after each sub-second measurement, so
these wall times are not treated as a controlled pair. The deterministic
counts point the same way: applying the controlled 34-letter cost ranges
predicts `d=13` at 0.65--0.70s and `d=14` at 0.83--0.92s.

The unrelated validation used the 29-letter bag
`firestationteamusedquickbrown`, which has 17 distinct letters. It used
`-C 32`, support-subset traversal, and 20 preprocessing threads. Every run was
checked before launch and once per one or two seconds while active; only the
measured `dfs-anagrams` instance was present.

| `d` | wildcard letters | table bytes | actions | states built | setup | search | phase 2 | construction transitions | final DFS nodes |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 12 | 12 | 718,848 | 71,382 | 98,101 | 0.524s | 6.544s | 7.068s | 64,076,272 | 78,432,288 |
| 13 | 10 | 1,824,768 | 124,723 | 239,529 | 1.200s | 3.880s | 5.080s | 215,610,693 | 47,935,597 |
| 14 | 7 | 5,308,416 | 201,521 | 673,525 | 4.450s | 2.043s | 6.493s | 764,416,628 | 25,143,714 |
| 15 | 5 | 11,943,936 | 270,847 | 1,487,302 | 12.677s | 1.230s | 13.908s | 1,817,808,981 | 14,792,187 |
| 16 | 3 | 23,887,872 | 363,069 | 2,968,011 | 34.234s | 0.742s | 34.976s | 4,012,601,755 | 8,078,253 |
| 17 | 0 | 23,887,872 | 363,069 | 2,968,011 | 40.216s | 0.707s | 40.923s | 4,012,601,755 | 8,078,253 |

All six outputs had SHA-256
`43ce76eb6381861d29af81ecfffda40136bcd525d9c6830888d7361a240187b7`.
The automatic largest-fitting selector chose `d=17`, the complete exact
table. A repeated gated `d=14` then `d=13` pair reported 6.612s and 5.666s
total phase 2 respectively, again favoring `d=13`; their deterministic counts
were identical to the table.

This sweep rejects one-dimension backoff as a general selector. At `d=16` the
only wildcard letter kind has multiplicity three. Its exact radix of four is
replaced by a wildcard span of four, so `d=16` and `d=17` have the same flat
state count. Because that wildcard count still uniquely identifies the
removed letter's count, the two recurrences are isomorphic: they also have the
same actions, constructed states, transitions, final nodes, and output. Their
setup-time difference is host variability, not an algorithmic improvement.

The first genuinely lossy backoff is `d=15`, but it is still 2.74x slower than
the measured optimum. `d=13` uses only 7.6% of the automatic table storage,
does 18.6x fewer construction transitions, and exposes 5.9x more final nodes;
the construction saving wins by 6.9--8.1x end to end relative to the
exact-equivalent `d=16` and `d=17` runs. Moving to `d=12` confirms the other
side of the knee: setup falls by 0.68s, but search rises by 2.66s.

The phase costs remain regular once the shapes become meaningfully projected.
For `d=13` through `d=15`, construction costs 5.6--7.0ns per successful
transition and final search costs 81--83ns per node. Those counts correctly
select `d=13` after the runs, as the earlier linear proxy did for the
34-letter case. They still do not solve pre-build selection because the final
node count is not yet known.

One cheap setup predictor does emerge from the existing sweeps. Constructed
states are a smooth fraction of logical table capacity within a bag:

| workload | measured `d` | constructed states / capacity |
|---|---|---|
| 28-letter `S6` | 12, 13, 14 | 53.3%, 52.1%, 51.2% |
| 34-letter `S6` | 12, 13, 14 | 67.1%, 66.5%, 65.4% |
| 38-letter `S6` | 13, 14, 15 | 49.3%, 49.0%, 48.0% |
| unrelated 29-letter | 12, 13, 14, 15, 16 | 54.6%, 52.5%, 50.8%, 49.8%, 49.7% |

The absolute density varies too much across bags for one global constant, but
a small projection can calibrate the richer candidates for the same bag.
Using the unrelated `d=12` density unchanged would predict the `d=13`,
`d=14`, and `d=15` constructed-state counts within 4%, 8%, and 10%. This makes
logical capacity plus a coarse-build density a credible state-count estimate.
It is not by itself a transition estimate: successful transitions per
constructed state also rise with depth.

### Analytic construction-edge predictor

The stronger setup predictor does not require state sampling. For each
deduplicated projected action, the number of logical mixed-radix states in
which it fits can be counted directly:

```text
wildcard choices
    * product(exact-root-count[i] - requirement[i] + 1)
```

Digits rarer than the action's forced bucket contribute one choice, because
they must be zero. The rarest exact digit stops one below its root count
because the compact table omits the root slab. Summing this product over
actions and adding the fitting root actions gives the logical fitting-edge
count for the complete projected box. The calculation is
`O(projected actions * d)`, saturates safely on overflow, and runs during
action preparation without visiting or allocating projected states.

`NUTRIMATIC_PROJECTED_PREFLIGHT_ONLY=1` now stops after action preparation so
candidate shapes can report table capacity, deduplicated actions, and logical
fitting edges without constructing a bound or running concrete DFS. The full
preflight, including ordinary class/action preparation, took 0.050s for the
28-letter `d=14` case, 0.130s for 34-letter `d=14`, 0.169s for 38-letter
`d=15`, and 0.246s for the unrelated exact `d=17` case.

The existing completed sweeps let a coarse depth calibrate what fraction of
the logical edges the top-down builder actually evaluates. The table applies
the coarsest measured depth's fraction unchanged to every richer depth:

| workload | calibration `d` | richer `d` values | actual transition / logical-edge range | richer transition prediction error |
|---|---:|---|---:|---:|
| 28-letter `S6` | 12 | 13, 14 | 39.6--44.5% | +5.7%, +12.5% |
| 34-letter `S6` | 12 | 13, 14 | 54.5--54.9% | +0.8%, +0.3% |
| 38-letter `S6` | 13 | 14, 15 | 44.5--46.4% | -3.1%, -4.1% |
| unrelated 29-letter | 12 | 13, 14, 15, 16 | 42.7--50.2% | +6.9%, +15.9%, +17.7%, +17.3% |

Positive error means overprediction. The estimate is not exact, but within a
bag it is much more stable than raw action count and directly captures the
rapid edge growth that dominates setup. Even the worst calibration error is
small relative to the 3--18x transition differences among candidate depths.
The instrumented 28-letter `d=12` validation counted 52,399,806 logical edges,
26,006,886 fitting edges in actually constructed states, and 23,327,436
successful transitions; its output retained the established SHA-256.

A deliberately crude retrospective total-cost proxy further checks that the
predictor contains useful selection information. Calibrate transitions from
the coarse build as above, estimate richer final nodes by scaling the coarse
node count with `(coarse actions / candidate actions)^1.5`, and charge 6ns per
transition plus 80ns per final node. It selects the measured optimum for the
34-letter, 38-letter, and unrelated workloads. It selects `d=13` instead of
`d=12` for the 28-letter workload, where the measured difference was only
4.6%. This is not a production selector: the exponent is empirical and the
coarse final-node count is known only after a complete coarse search. It does
show that logical fitting edges plus a pruning-value estimate are sufficient
to locate the observed knees; cache occupancy is not.

### Hierarchical coarse-certificate experiment

The first exact edge certificate uses the already implemented length-only
projection. For a rich action `a`, it forms the conservative envelope

```text
action_score(a) + restart + length_bound(child_letters)
```

and compares it with a feasible lower witness from an earlier fully evaluated
action at the same rich state. The experiment stores one temporary
downward-rounded float witness per rich table slot. A child publishes that
witness before publishing its existing upward-rounded bound, so the upper and
lower sides remain conservatively ordered across preprocessing threads.

`NUTRIMATIC_PROJECTED_CERTIFICATE_DIAGNOSTICS=1` leaves the recurrence
unchanged and counts envelopes already below the incumbent in the existing
action order. It does not sort actions to make the result look better.

The shadow counts are:

| workload | fitting edges | successful transitions | checked after an incumbent | certified | certified / fitting |
|---|---:|---:|---:|---:|---:|
| 28-letter `d=12` | 26,006,886 | 23,327,436 | 24,881,138 | 10,993,274 | 42.3% |
| 28-letter `d=14` | 236,483,620 | 213,302,595 | 231,027,835 | 71,209,010 | 30.1% |
| unrelated 29-letter `d=13` | 231,084,895 | 215,610,693 | 223,265,709 | 143,930,534 | 62.3% |

This is enough headroom to promote hierarchical certification from a
speculation to a scheduler input. It also shows that score ordering is not a
prerequisite for a useful rate. Ordering by coarse envelope may increase the
rate, but should be tested only after the state-coverage problem below is
addressed.

`NUTRIMATIC_PROJECTED_CERTIFICATE_PRUNE=1` then skips a certified edge.
Skipping preserves the value of the state being computed, but it can leave the
child slot unseen. That matters because the concrete DFS may later query the
child directly even though the child could not improve this particular
parent. A first version returned no bound for those misses and passed 308
million final nodes on the 28-letter `d=12` case before being stopped. Root
recurrence exactness is not the same as complete query coverage.

The corrected prototype retains action metadata and computes an unseen rich
state exactly on its first concrete-DFS query. Its results are:

| workload | threads | mode | eager states | total states | setup | search | phase 2 | transitions |
|---|---:|---|---:|---:|---:|---:|---:|---:|
| 28-letter `d=12` | 20 | shadow | 71,899 | 71,899 | 0.329s | 1.187s | 1.516s | 23,327,436 |
| 28-letter `d=12` | 20 | skip + demand | 61,362 | 71,231 | 0.180s | 1.596s | 1.776s | 12,598,401 |
| 28-letter `d=14` | 20 | shadow | 430,123 | 430,123 | 2.181s | 0.345s | 2.525s | 213,302,595 |
| 28-letter `d=14` | 20 | skip + demand | 404,885 | 428,748 | 1.398s | 3.635s | 5.033s | 144,436,679 |
| 28-letter `d=14` | 1 | shadow | 430,123 | 430,123 | 17.880s | 0.343s | 18.224s | 213,302,595 |
| 28-letter `d=14` | 1 | skip + demand | 275,865 | 428,748 | 5.549s | 10.318s | 15.867s | 144,428,674 |
| unrelated `d=13` | 20 | shadow | 239,529 | 239,529 | 1.925s | 5.534s | 7.459s | 215,610,693 |
| unrelated `d=13` | 20 | skip + demand | 175,313 | 234,005 | 0.963s | 10.741s | 11.704s | 73,253,366 |

Every completed pair retained its established output SHA-256 and identical
final DFS node and solution counts. The 28-letter `d=12` and `d=14` skip runs
reduced transitions by 46.0% and 32.3%; the unrelated run reduced them by
66.0%. Total constructed states fell by only 0.3--2.3%, because exact
on-demand recurrence closures eventually recovered nearly all of the states
omitted during eager construction.

The serial `d=14` pair improves total phase 2 by 12.9%, demonstrating that the
certificate saves real work rather than merely moving counters. The
20-thread pairs regress by 17--99% because 9,869--58,692 state constructions
move into the single-threaded final search. The 20-thread `d=14` eager state
count is unusually high relative to its serial counterpart because each root
worker has its own incumbent and concurrent traversal changes which shared
children are already ready; both runs converge to the same 428,748 total
states and essentially the same transition count after demand completion.

These timings were gated against another `dfs-anagrams` process before each
run but not continuously monitored. The deterministic state, edge, node,
solution, and output comparisons carry the main conclusion:

1. keep the conservative upper envelope and downward witness design;
2. do not synchronously construct a rich miss on the concrete DFS thread;
3. either enqueue misses for nonblocking background refinement while using a
   complete smaller projection, or apply certificates in a parallel
   layer/state scheduler that preserves the desired table coverage; and
4. measure candidate ordering only inside that scheduler. Reducing eager root
   closure is not itself the objective when concrete queries recover almost
   the entire closure.

### Nonblocking-fallback simulation

The next experiment isolates the other endpoint. With

```sh
NUTRIMATIC_PROJECTED_CERTIFICATE_PRUNE=1
NUTRIMATIC_PROJECTED_CERTIFICATE_FALLBACK=1
```

a certificate-created rich miss does no synchronous construction. It
immediately uses the minimum of the complete length and modular bounds.
This models a requested entry that remains unfinished for the whole lookup;
it does not yet run a background refiner. The new counters report total and
distinct rich-hole lookups and how often the fallback itself prunes.

The first runs used the 9-bit x 2 modular configuration:

| workload | eager rich states | setup | search | rich transitions | fallback lookups | unique holes | fallback prunes | final DFS nodes | nodes / complete rich |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 28-letter `d=12` | 60,285 | 0.368s | 2.973s | 7,620,098 | 1,808,317 | 11,508 | 1,801,053 (99.60%) | 38,587,322 | 3.41x |
| 28-letter `d=14` | 408,818 | 1.547s | 2.187s | 115,929,036 | 312,826 | 20,763 | 306,097 (97.85%) | 29,310,946 | 9.50x |
| unrelated `d=13` | 172,157 | 1.094s | 13.876s | 33,228,256 | 11,717,768 | 66,924 | 11,707,971 (99.92%) | 162,691,681 | 3.39x |

Every output retained its established SHA-256. The complete-rich comparison
uses the earlier 11,331,110, 3,086,650, and 47,935,597 final-node counts.
Parallel traversal changes which edges receive an incumbent soon enough to be
certified, so eager state and transition counts vary somewhat between runs;
the final traversal result is the important discriminator here. Each run was
checked for another `dfs-anagrams` process before launch, and the longer
unrelated run was also checked while active.

Widening the 28-letter `d=14` fallback to 10-bit x 2 did not fix the
amplification. It pruned 337,115 of 343,939 fallback lookups (98.02%) and
visited 29,084,128 nodes, only 0.8% fewer than 9-bit x 2. Modular preparation
grew from 0.225s to 0.823s and total phase 2 grew from 3.735s to 4.255s.

Aggregate prune coverage is therefore the wrong scheduler objective. The
0.08--2.15% of fallback lookups that fail are not representative: some occur
above very large concrete subtrees and recursively generate more missing-rich
lookups. A complete small exact projection is much safer. On the same
28-letter bag, complete `d=12` visits 11.3 million nodes, versus 29.3 million
for partial `d=14` plus modular fallback. On the unrelated bag, complete
`d=12` visits 78.4 million nodes, versus 162.7 million for partial `d=13`
plus modular fallback.

This rejects a fallback-only endpoint, not an active background scheduler.
The synchronous-demand runs show that the missing exact closures add only
tens of millions of rich transitions, work that a persistent parallel builder
may close quickly. The next scheduler prototype must measure latency from a
concrete miss to a ready rich entry and nodes exposed before readiness. Queue
priority should include DFS depth, bound gap, or subtree growth observed by a
bounded pilot; lookup frequency alone discovers a costly state only after its
subtree has begun expanding.

### Dead work by remaining-letter layer

Projected diagnostics now split outgoing fitting edges, incoming dead-child
edges, and finite/dead constructed states by `letters_left`. Counters remain
worker-local and are merged after parallel construction. Incoming dead-child
edges are attributed to the child layer, which makes the measurement directly
useful for sizing a reverse-completion perimeter.

The validation used completed rich builders without certificate skips:

| workload | dead states / all states | dead-child / fitting edges | incoming dead-child edges landing at `letters_left <= 7` | forward states at `letters_left <= 7` | dead states at `letters_left <= 7` |
|---|---:|---:|---:|---:|---:|
| 28-letter `d=12` | 3,358 / 71,899 (4.7%) | 2,679,450 / 26,006,886 (10.3%) | 99.969% | 10.1% | 99.1% |
| 28-letter `d=14` | 11,615 / 430,123 (2.7%) | 23,181,025 / 236,483,620 (9.8%) | 99.988% | 5.5% | 99.6% |
| unrelated 29-letter `d=13` | 4,689 / 239,529 (2.0%) | 15,474,202 / 231,084,895 (6.7%) | 100.000% | 6.0% | 100.0% |

Every dead state in the two `S6` runs had at most 11 letters remaining; all
dead states in the unrelated run had at most seven. The few `S6` dead states
in layers 9--11 had almost no incoming edges. A layer-seven boundary therefore
captures essentially all repeatedly encountered dead children without
requiring the reverse side to extend through one third of the forward state
set.

The deterministic aggregate counts and output hashes remained unchanged. The
two 28-letter runs retained SHA-256
`de32cca977192c1ab65b56329c9a7f8f97e25441fbea1e39c049b7ebf8c33ee6`;
the unrelated run retained
`43ce76eb6381861d29af81ecfffda40136bcd525d9c6830888d7361a240187b7`.
All launches were checked for another `dfs-anagrams` process, and the longer
unrelated run was checked while active.

This changes the reverse-construction recommendation. A full reverse builder
still has weak evidence: only 2.0--4.7% of forward states are dead, and
reverse traversal can generate finite states unreachable from the root. The
bounded preflight now exploits `-m 4`: below eight remaining letters, a finite
state is either empty or exactly one projected action delta. Sorting and
deduplicating the 4--7-letter action deltas therefore enumerates the complete
reverse-completable perimeter without running a reverse recurrence.

| workload | reverse finite keys through layer 7 | forward finite states through layer 7 | root-irrelevant reverse keys | reverse keys / all forward states | full-key-space bitset |
|---|---:|---:|---:|---:|---:|
| 28-letter `d=12` | 5,811 | 3,931 | 1,880 (32.4%) | 8.1% | 16,848 B |
| 28-letter `d=14` | 16,951 | 12,273 | 4,678 (27.6%) | 3.9% | 104,976 B |
| unrelated 29-letter `d=13` | 12,698 | 9,692 | 3,006 (23.7%) | 5.3% | 57,024 B |

The 1.31--1.48x reverse/forward expansion is modest, and even the direct
one-bit-per-rich-key representation is small. The next implementation should
retain that temporary mask and let the top-down recurrence reject an unmarked
child at seven or fewer letters without recursively discovering
`-infinity`. It should report dead-child lookups avoided, mask preparation
time, and whether immediately final low-layer children improve certificate
scheduling. General reverse recursion is unnecessary unless a later
experiment moves the perimeter to eight or more letters.

### Concrete-search length-group certificates

`ideas/index-of-index.md` measured that the concrete DFS spends its time in
candidate generation rather than in bound evaluation: at 40 letters it performs
9.73 billion class fit tests to produce 343 million children, of which 181
thousand survive their bound test. Its Proposal D — one coarse test per
consumed-length group instead of a test per class — is now implemented and
timed.

At an expanding node with `letters_left` remaining and representative score
`rep`, and for the group of classes in the forced-symbol bucket that consume
`len` letters, the test is

```text
rep + restart + max_score[rank][len] + U[letters_left - len] + padding
    <= floor
```

`max_score[rank][len]` is the largest class score in that bucket at that
length and `U` is the existing length-only tail bound
(`prepare_projected_length_bounds`). Both are upper estimates over everything
the group can do, so a group that fails the test cannot contain a class whose
subtree reaches the sink's floor. The argument does not involve the rich
projected table, so the certificate stays exact whatever projection depth is
selected. `padding` reuses the `should_prune` error envelope.

The bucket for a rank is already contiguous and ordered by decreasing key
length, so a length group is an index range and a rejected group is a range
skip. Preparation is one pass over the class list: 15,304 bytes and 8ms at 40
letters, including the 41-entry `U`.

Two flags exist. `NUTRIMATIC_LENGTH_CERTIFICATE_SHADOW=1` counts what the test
would skip while still scanning everything, so its node, transition, and
solution counts remain identical to an uncertified run.
`NUTRIMATIC_LENGTH_CERTIFICATE=1` performs the skip.

| workload | group tests | rejected | class scans skipped | of |
|---|---:|---:|---:|---:|
| 40 letters, `d=15` | 1,805,219 | 60.79% | 6,723,180,516 | 9,732,285,204 (69.08%) |
| 38 letters, `d=13` | 5,343,154 | 82.77% | 18,182,854,317 | 20,445,709,628 (88.93%) |
| 38 letters, `d=12` | 9,354,516 | 85.82% | 32,988,335,020 | 36,105,059,800 (91.37%) |
| unrelated 29, `d=13` | 256,722 | 57.25% | 516,204,006 | 1,049,278,427 (49.20%) |

The shadow run at 40 letters `d=15` retained 342,949,072 final nodes, 24,804
solutions, 5,919,322,956 construction transitions, and the established output
hash, confirming the counters are non-intrusive. Its scan total agrees with the
independent scratch-patch count in `ideas/index-of-index.md` to 0.003%.

Skipping is worth about half the search on the workload the production selector
picks. Gated 40-letter `d=15`, `-C 32`, 20 threads, two paired runs:

| variant | setup | search | final DFS nodes |
|---|---:|---:|---:|
| baseline | 24.724s / 28.780s | 22.380s / 22.920s | 342,949,072 |
| group certificate | 28.335s / 30.118s | 10.679s / 11.026s | 203,098,015 |

Search falls 52.3% and 51.9%; the 6.8ms preparation is inside the setup noise.
Final nodes fall 40.8% because a certified child never becomes a node at all,
which makes node count no longer comparable across certificate settings.
Retained top-1000 output is byte-identical; the raw `solutions` count falls
from 24,804 to 24,767 because some skipped subtrees contained complete
solutions below the floor.

#### Score-descending suffix rejection

Proposal D's third item resolves the group into individual classes. Solving the
same envelope for the class score gives a per-group threshold, so a group
visited in score-descending order can stop at its first failure. Rather than
reorder the shared class list, `NUTRIMATIC_LENGTH_CERTIFICATE_SUFFIX=1` builds
a permutation confined to each length group: index order is preserved across
groups, so the entry-point tie-break survives as a per-class filter in the one
group that contains the scan start. The permutation costs four bytes per class
(2.0 MiB at 40 letters).

| workload | classes examined, group only | with suffix rejection | total candidates |
|---|---:|---:|---:|
| 40 letters, `d=15` | 3,009,104,688 | 1,565,110,632 | 9,728,325,702 |
| 40 letters, `d=13` | 11,215,058,092 | 3,854,218,790 | 60,913,150,354 |
| 38 letters, `d=13` | 2,262,855,311 | 1,197,791,376 | 20,444,384,799 |
| unrelated 29, `d=12` | 858,405,421 | 139,117,757 | 1,850,249,009 |

Two checks make these counters self-validating, and both are worth re-running
after any change to `walk_certified`. The three buckets — group skips, suffix
skips, scanned — are disjoint and must sum exactly to the reported total. And
the suffix-mode total must land *below* the shadow-mode total for the same
workload, because suffix mode visits strictly fewer nodes; at 40 letters
`d=15` that is 9,728,325,702 against 9,732,285,204. An earlier revision
counted below-entry-point ids among the suffix skips and produced a total
*above* shadow, which is how the miscount was found.

One caveat applies to `_SUFFIX` and not to the group test. `DfsTopN::offer` rejects
on `log_score <= heap[0].log_score`, so among distinct spellings whose scores
are exactly equal at the N-th position, whichever arrives first is retained.
Visiting a group in score-descending order changes arrival order, so a workload
with an exact tie at the cutoff can return a different — equally scoring —
spelling. No measured workload did, but this means an unchanged output hash is
evidence for `_SUFFIX`, not a proof of equivalence the way it is for the group
test and for `_SHADOW`.

#### Depth sweeps under certification

Because the certificate makes search much cheaper, the setup-versus-search knee
moves. All runs use `-m 4 -n 1000 -C 32`, support-subset traversal, 20
preprocessing threads, and were gated against another `dfs-anagrams` process
before launch and every 2--3 seconds while active. Every run in every table
reproduced its workload's established output SHA-256. "none" repeats the
uncertified figures measured earlier in this document.

38-letter `S6` prefix, total phase 2 seconds (final DFS nodes):

| `d` | none | group | group + suffix |
|---:|---:|---:|---:|
| 10 | | | 8.900 (111,897,322) |
| 11 | | | 8.118 (111,188,522) |
| 12 | | 7.967 (147,363,472) | 8.229 (109,245,187) |
| 13 | 51.837 (540,441,307) | **7.701** (134,652,495) | **7.396** (103,587,733) |
| 14 | **22.248** (203,485,601) | 8.872 (98,491,550) | 9.402 (81,931,370) |
| 15 | 27.621 (105,067,790) | 22.886 (63,518,076) | |

40-letter `S6` prefix:

| `d` | none | group | group + suffix |
|---:|---:|---:|---:|
| 11 | | | 23.856 (262,107,858) |
| 12 | | 30.681 (441,712,322) | 20.923 (258,014,200) |
| 13 | | 24.347 (372,554,960) | 20.043 (248,719,761) |
| 14 | | **21.984** (315,453,554) | **19.818** (227,097,702) |
| 15 | 47.104 (342,949,072) | 41.144 (203,098,015) | 39.664 (165,817,891) |

Unrelated 29-letter bag `firestationteamusedquickbrown`:

| `d` | none | group | group + suffix |
|---:|---:|---:|---:|
| 8 | | | 1.655 (11,074,843) |
| 9 | | | 1.403 (10,934,732) |
| 10 | | | 1.381 (10,584,204) |
| 11 | | 4.868 (89,934,447) | **1.268** (9,270,228) |
| 12 | 7.068 (78,432,288) | 3.433 (59,045,518) | 1.475 (8,018,918) |
| 13 | **5.080** (47,935,597) | **2.991** (38,344,142) | 1.725 (6,810,029) |
| 14 | 6.493 (25,143,714) | 4.954 (21,393,265) | |
| 17 | 40.923 (8,078,253) | | |

The 28-letter `d=14` case is setup-dominated and gains little: search falls
from 0.168s to 0.122s (group) and 0.104s (suffix), and final nodes from
3,086,650 to 2,636,902 and 1,493,982, on a 1.0s setup.

Four things follow.

1. **Enable the group certificate unconditionally.** It is exact, costs
   kilobytes and milliseconds, needs no scheduler change, and never loses on
   any measured workload.
2. **Suffix rejection is not free.** It halves to quarters the surviving scans
   everywhere, but at 40 letters `d=15` search rose from 11.03s to 12.08s
   despite examining half as many classes. That reproduces the wildcard-length
   ordering result above: within-group reordering costs successful-edge
   locality. It wins at the depths a certified search should actually select,
   and loses at over-rich ones.
3. **The certificate substitutes for projection richness.** On the unrelated
   bag the certified node count only falls from 11.1M at `d=8` to 6.8M at
   `d=13`, against 78.4M at uncertified `d=12`. The depth curve flattens: the
   best certified point uses a 276 KiB table where the production selector
   chose 23.9 MiB, and the whole `d=9`--`d=13` range is within 24% of optimal.
   Bound quality has stopped being the binding constraint on that workload.
4. **The best depth moves down** by one dimension at 38 and 40 letters and by
   six on the unrelated bag. Every conclusion in the depth-selection sections
   above was measured against an uncertified search and should be re-derived,
   including the retrospective cost proxy's 6ns/transition and 80ns/node
   constants.

End to end, against the shape the current production selector picks:

| workload | production selector | best certified | ratio |
|---|---:|---:|---:|
| 28-letter `S6` | 1.164s (`d=14`) | 1.118s (`d=14`) | 1.04x |
| 38-letter `S6` | 27.621s (`d=15`) | 7.396s (`d=13`) | 3.73x |
| 40-letter `S6` | 47.104s (`d=15`) | 19.818s (`d=14`) | 2.38x |
| unrelated 29 | 40.923s (`d=17`) | 1.268s (`d=11`) | 32.3x |

## Revised near-term recommendations

0. Promote the length-group certificate to a default, not an experiment, and
   re-run the depth-selection sweeps underneath it. Decide suffix ordering by
   workload: enable it with small tables, not with over-rich ones.

1. Do not select the largest `d` that fits the cache, and do not replace that
   rule with a fixed decrement in `d`.
2. Treat depths with only one wildcard letter kind as exact-equivalent and
   skip them as projection candidates. More generally, rank candidate shapes
   by actual state/action reduction, not by dimension count.
3. Keep the dimension backoff only as a narrow baseline for the measured `S6`
   prefixes. There is no justified production default yet: the unrelated bag
   needs a much more aggressive backoff.
4. Enumerate candidate capacities, action counts, and analytic logical fitting
   edges in preflight. Build one small projection to calibrate its reachable
   edge fraction, then use that fraction to predict richer setup work. This
   replaces table bytes with a measured construction-work proxy.
5. Estimate pruning value with a bounded concrete-DFS pilot or sampled richer
   lookups before paying for a rich table. Action-count scaling is an adequate
   retrospective baseline, not yet a justified live predictor.
6. Carry the measured coarse certificates into a parallel layered or
   nonblocking demand builder. Do not synchronously fill rich misses on the
   concrete DFS thread, but also do not leave them on modular fallback for the
   full search: even 97.8--99.9% fallback pruning exposed 3.4--9.5x more
   nodes.
7. Use a complete small exact-letter projection as the primary
   always-available fallback. The modular quotient can complement it by
   minimum, but should not replace it. Prioritize rich requests by observed
   subtree leverage or a bounded pilot, not lookup frequency alone.
8. Prototype a reverse-completable bitset only through seven remaining
   letters using the now-measured one-action enumeration. Do not replace the
   forward builder; use the 16--105 KiB mask to reject low-layer dead children
   and measure whether avoiding 6.7--10.3% of fitting edges repays its cost.
9. Measure whether a rich builder running concurrently with small-projection
   search closes the high-leverage holes early enough to repay its CPU and
   memory-bandwidth cost. The eager comparisons so far do not: on the
   unrelated bag, `d=14` spends 3.25s more setup than `d=13` to save only
   1.84s of standalone search; on the 38-letter case, `d=15` spends 13.98s
   more setup to save 8.61s of search.

## Reproducing the length-group certificate

```sh
export IDX=idx/wiki-merged.5.index
source ./s.sh

# non-intrusive shadow counters: identical nodes, transitions, and output
NUTRIMATIC_PROJECTED_SCORE=1 NUTRIMATIC_PROJECTED_SUPPORT_GROUPS=1 \
NUTRIMATIC_LENGTH_CERTIFICATE_SHADOW=1 \
  build/dfs-anagrams "$IDX" "${S6:0:40}" -m 4 -n 1000 -C 32 -F -T 20

# live skipping, and the score-descending refinement, at a chosen depth
NUTRIMATIC_PROJECTED_SCORE_D=14 NUTRIMATIC_PROJECTED_SUPPORT_GROUPS=1 \
NUTRIMATIC_LENGTH_CERTIFICATE_SUFFIX=1 \
  build/dfs-anagrams "$IDX" "${S6:0:40}" -m 4 -n 1000 -C 32 -F -T 20
```

`NUTRIMATIC_LENGTH_CERTIFICATE_SUFFIX` implies `NUTRIMATIC_LENGTH_CERTIFICATE`;
`NUTRIMATIC_LENGTH_CERTIFICATE_SHADOW` suppresses both skipping and the
permutation so that its counters describe the uncertified traversal. All three
require a projected bound, because they reuse `projected_length_bounds`.

They also require `bound_mode != SCORE_BOUND_OFF`. The certificate prunes on
the same floating-point contract as `should_prune`, so it honours the same
guards: a sink that cannot prune, a cache below one alignment unit, and — the
load-bearing one — `score_bound_arithmetic_supported()`, which disables
FP-based pruning under `__FAST_MATH__` or a non-`FE_TONEAREST` rounding mode.
Without that gate a `-ffast-math` build could reassociate the
`upper + padding <= floor` comparison and silently drop valid solutions.

## Correctness argument

The length-group certificate is exact independently of the projected table.
Every class in a `(rarest rank, consumed length)` group consumes exactly `len`
letters and scores at most `max_score[rank][len]`; every completion of the
residual scores at most `U[letters_left - len]`, because `U` is the complete
length-only relaxation of the same action set. So

```text
rep + restart + max_score[rank][len] + U[letters_left - len]
```

is an upper bound on every solution reachable through every member of the
group, and rejecting the group when that envelope cannot reach the sink's floor
cannot discard a retained solution. The score-descending form solves the same
inequality for the class score; its threshold uses the group's largest score
for the error padding, which is at least every member's magnitude, and is
rounded down twice. Both changes shrink the threshold and therefore make
rejection strictly harder.

A projected flat delta uniquely identifies the exact-letter consumption vector
and wildcard consumption. Classes with the same bucket and delta reach the
same child from every state in which they fit. For child value `H`, their
candidate values differ only in class score:

```text
class_score + restart + H(child)
```

Retaining the maximum class score therefore preserves the recurrence exactly.

For rounding, the new per-state envelope uses the maximum class-score
magnitude and maximum child magnitude independently. Their sum is at least
the magnitude sum for every individual edge, so the resulting stored bound
remains upward-conservative.

## Research inventory

The following are unmeasured hypotheses unless explicitly described as
implemented. "Exact" below means exact with respect to the selected projected
recurrence, not exact with respect to the concrete bag.

| lever | main idea | guarantee | first useful discriminator |
|---|---|---|---|
| certify concrete candidates | reject whole consumed-length groups in phase 2 with `max_score` + length tail | exact; implemented and measured | class fit tests removed, and the depth at which total phase 2 is minimized |
| remove edges | certify losing actions with a cheaper upper bound | exact, if every skipped edge has a certificate | fraction of fitting edges rejected in shadow mode |
| remove actions | single- and multi-action dominance | exact | actions removed, weighted by how often their bucket is scanned |
| remove states | refine only states requested by the concrete DFS | admissible fallback; exact entries optional | projected-key query frequency and bound gap |
| avoid dead work | build backward from the empty bag | exact over generated finite states | fitting edges to `-infinity` children and forward/reverse state overlap |
| regularize the DP | layered max-plus shift-and-reduce | exact | reachable density by total-letter layer |
| improve the abstraction | complementary, cost-partitioned, or adaptively split projections | admissible | pruning gain per constructed transition |
| share state sets | decision diagrams or repeated-subtable compression | exact or conservatively quantized | repeated-subtable and value-delta entropy |
| cheapen an edge | packed state arithmetic, fixed-point scores, and fit indexes | exact or conservatively rounded | cycles per successful and rejected transition |
| amortize setup | background construction and persistent reuse | exact ready entries plus admissible fallback | repeated-query rate and overlap with final DFS |

The categories matter because they require different evidence. A faster fit
test will not help much if almost all time is in successful edges, while a
reverse builder is attractive only if dead descendants are common enough to
offset the finite states it generates that the root traversal would never
visit.

## Reduce action-to-child edges

### Hierarchical candidate certificates

Construct a very cheap coarser projection before constructing the selected
rich projection. The simplest is the `d=0` length-only table: it has one state
per remaining length and at most one deduplicated action per consumed length.

If `U` is an upward-conservative coarse bound, then for every rich projected
action `a`:

```text
candidate(a, state)
    <= action_score(a) + restart + U(project(child(a, state)))
```

Evaluate one promising action fully to obtain a feasible lower incumbent
`best`. Any other action whose upper envelope is no greater than `best` can be
discarded without loading or constructing its rich child. Group actions by
consumed length and order each group by descending score. For a length-only
`U`, the child term is constant within the group, so one failed threshold
rejects the rest of that score-ordered group.

The stored child values cannot directly supply that incumbent: they are floats
rounded upward to remain admissible, so a candidate calculated from one is
also an upper value, not a guaranteed feasible lower value. The implemented
experiment therefore keeps a companion downward-conservative float for every
expanded child. Treating the existing upward-rounded maximum as `best` would
incorrectly skip an edge.

A cascade of progressively stronger projections could certify more losers.
There are two safe stopping modes:

- continue until every omitted action is certified, preserving the exact rich
  recurrence; or
- stop early and store the coarse envelope for the unresolved portion,
  producing a weaker but still admissible bound.

Never store the maximum of only the expanded candidates as an upper bound. It
is a lower bound on the projected maximum and can prune valid results.

The shadow and live experiments above establish a 30--62% certificate rate and
a 32--66% reduction in successful-transition volume. They also establish the
scheduler constraint: skipping an edge can omit a child slot that concrete DFS
later queries directly. Synchronous exact construction on that query preserves
correctness but regresses the 20-thread runs. The next implementation should
retain the certificate and change coverage scheduling, not add a production
skip to the current recursive loop. Whole score-ordered suffixes remain an
unmeasured secondary improvement.

### Composite action dominance

The current deduplication compares only actions with identical single-action
deltas. An action `a` is also globally dominated if a sequence of other actions
has the same total projected delta and at least its adjusted score:

```text
delta(a) = sum(delta(b_i))

score(a) + restart
    <= sum(score(b_i) + restart)
```

Consider any complete projected solution that uses `a`. Replacing `a` with the
other actions preserves the solution's total consumption and does not decrease
its score. The whole replacement solution can then be put into canonical
forced-letter order, because some remaining action always consumes the rarest
remaining exact letter. The replacement actions alone need not be executable
consecutively at the point where `a` appeared; they may interleave with the
original suffix. This whole-solution argument is what makes the removal exact.

The restart charge on every replacement action may make domination uncommon.
Run a bounded offline decomposition DP over action-sized deltas first and
report both raw removals and expected scan-weighted removals. The latter is
more informative because deleting an action in a rarely reached bucket has
little construction value.

### Conditional and local dominance

Global equal-delta dominance is deliberately strict. A coarse table can prove
additional dominance for one state even when no global replacement exists:

```text
upper(a, state) <= exact_candidate(b, state)
```

This is the same certificate mechanism applied pairwise. Cache the winning
action ID temporarily with each constructed state, try winners from neighboring
mixed-radix states first, and then certify the other actions. If winner regions
are stable, this becomes a cheap policy-plus-certificate computation rather
than a full maximum over every edge.

A raw Pareto rule such as "consume fewer letters with a higher score" is not
safe. Consuming fewer letters leaves more negative-cost work for the child.
Any such dominance test needs either an equal-delta replacement or an explicit
bound on the residual consumption.

## Construct fewer states

### Search-driven bounded refinement

Lazy construction can go further than delaying the current recursive closure:

1. Start concrete DFS with a nearly free length-only or other complete coarse
   bound.
2. Once the top-N sink has a score floor, record projected states for which the
   coarse bound failed to prune.
3. Refine states according to query count, bound gap, and estimated subtree
   leverage.
4. Limit refinement work and retain a coarse upper envelope for every
   unresolved action and state.

Every lookup therefore has an admissible answer. Ready rich entries improve
the bound; missing or unfinished entries fall back to the coarse table. This
does not recreate the coverage holes of the old partial exact cache.

The refinement can also run on otherwise idle preprocessing threads while the
single-threaded concrete DFS proceeds. The concrete search must never spin on
a `COMPUTING` rich entry: it uses the fallback, optionally enqueues the key,
and continues. Completed entries are already published child-first, so they
can be consumed safely. This changes wall-clock structure as well as work
selection: useful construction overlaps search, and irrelevant construction
can be cancelled when the DFS completes.

The fallback simulation adds two constraints. First, use a complete small
exact projection as the default missing-entry answer; a 9- or 10-bit modular
quotient left enough high-leverage misses to expand 3.4--9.5x more nodes even
while pruning 97.8--99.9% of hole lookups. Second, queue priority cannot be
query count alone. A first miss near the top of a concrete subtree may need to
preempt many frequently requested shallow-leverage states. DFS depth, bound
gap, and growth observed since the request are cheap candidate signals.

If a parent is computed using fallback child values, it remains admissible but
must be marked inexact and revisited after its children improve. Keeping exact
and fallback-derived entry states distinct avoids silently treating a stale
upper envelope as the exact rich recurrence.

### Backward construction from completable states

The recurrence is a shortest-path problem after negating scores, with the empty
bag as its goal. The current top-down builder starts at the projected root and
visits forward-reachable states, including states whose eventual value is
`-infinity`. Reverse construction starts with the empty bag and adds actions,
so it generates only states with a finite completion.

The forced-letter canonicalization has a simple reverse condition. If an
action's bucket is `b`, it may be added to a child only when the child contains
no exact letter rarer than `b`. Wildcard-only actions may be added only while
the child has no exact letters. Processing increasing total-letter layers then
finalizes every child before its parents.

This direction is not automatically better. It can generate finite states
within the root box that are not reachable by subtracting actions from the
root. The comparison is:

```text
current builder: forward-reachable, including dead states
reverse builder: goal-reachable, including root-irrelevant states
ideal work set:  intersection of the two
```

The existing counter omits fitting transitions whose child is `-infinity`, so
it cannot estimate this tradeoff. Count those edges and finite/dead states
before implementing a full reverse builder. A small shadow reverse reachability
bitset by total-letter layer can then estimate the intersection.

### Bidirectional relevance and a perimeter

If neither direction is sparse enough, keep compact reachability marks from
both sides and evaluate values only in their intersection. Another variant
builds a small reverse perimeter of completable states and lets top-down
construction stop when it reaches that perimeter.

This resembles a partial pattern database, but its fallback contract is
important here: a state outside the selected intersection still needs the
coarse admissible bound. The perimeter is a work-selection device, not
permission to return "no bound."

## Change the dynamic-programming organization

### Max-plus shift-and-reduce

For a fixed projected action, `child_key = parent_key - delta` is a shifted
dense-array access over a rectangular mixed-radix region. The recurrence can
therefore be viewed as a max-plus shift-and-reduce:

```text
H[parent] = max over eligible a of
    adjusted_score[a] + H[parent - delta[a]]
```

Rather than recursively scanning actions for one state, process total-letter
layers and apply an action to a run or tile of eligible parent states. The
mixed-radix digit limits describe those runs without per-state requirement
loops. Tile ownership avoids atomic maximum updates, and the shifted child
loads become predictable enough for vectorization and prefetching.

This does not reduce the mathematical edge count. It is attractive if the
reachable/finite density within a layer is high, because it replaces recursive
bag mutation, cache-miss calls, and atomic first-owner synchronization with
streaming work. It is unattractive for sparse layers. Report per-layer density
and valid-edge density before choosing between parent-major, action-major, and
tiled traversal.

The small maximum number of words suggests another formulation,
`F[k][delta] = best score using k actions`, followed by a maximum over `k`.
That is also a sequence of max-plus convolutions. It is worth keeping in the
inventory for SIMD or accelerator experiments, but it does not obviously
remove more work than the layered recurrence on a CPU.

### Nonblocking task-DAG scheduling

Within the existing recursive traversal, a worker that encounters a child
owned by another worker spins. If spin time is material, replace waiting with a
continuation: register the parent as dependent on the child, work on another
ready state, and resume the parent when the child publishes.

This retains sparse top-down construction and exact values, unlike a dense
layered pass. It is substantially more complex than the current recursion, so
instrument ownership failures and spin cycles first. If those are small, edge
reduction or packed arithmetic has more leverage.

## Improve the abstraction per unit of construction

### Several small complementary projections

Instead of paying the Cartesian-product cost of one large table, build two or
more small projections over different letter sets:

```text
bound(bag) = min(P1(project1(bag)),
                 P2(project2(bag)),
                 ...)
```

Each `Pi` is independently an upper bound, so their minimum remains admissible.
Complementary projections can retain different constraints while their total
state and action counts remain below those of one large projection. During
concrete DFS, check the empirically strongest table first and stop as soon as
one table proves a prune.

The selection objective should be estimated nodes avoided per construction
transition, not average bound value or table bytes alone. A second projection
should be chosen specifically on sampled states where the existing projection
is weak, rather than by another independent rare-letter rule.

### Additive cost-partitioned projections

Complementary projections can potentially combine more strongly than a
minimum. Every future action has negative adjusted log score:

```text
weight(a) = class_score(a) + restart < 0
cost(a) = -weight(a) > 0
```

Split each action cost among projections:

```text
cost(a) = sum(cost_i(a)), with cost_i(a) >= 0
```

Let each relaxed projection compute the minimum completion cost under its
assigned `cost_i`. For any concrete completion, the sum of the projected
minimum costs is no greater than that completion's exact cost. Negating the
sum therefore gives an admissible score upper bound:

```text
score_bound(bag) = -sum(projected_min_cost_i(project_i(bag)))
```

Positive cost assigned to a projection where the action delta is zero is
unexploitable: the abstract optimizer can omit that self-loop. It is still
admissible, but it wastes cost that another projection could use, so a useful
partition should assign that component zero. Uniform splitting is therefore
only a correctness baseline, not necessarily a sensible allocation. Saturated
or post-hoc cost partitioning may preserve more useful cost in each
abstraction, but its own construction overhead must be included.

### Adaptive group splitting

The current automatic shape is a corpus-rarest exact prefix and one wildcard
group. More general shapes include arbitrary exact masks, several wildcard
groups, and exact dimensions for common but high-impact letters.

Treat this as counterexample-guided abstraction refinement:

1. build a very small projection;
2. sample concrete states where its bound narrowly failed to prune;
3. identify the wildcard letter or group split that most separates the
   high-scoring aliased actions on those states;
4. refine only while predicted pruning value exceeds construction cost.

Useful split scores include reduction in projected-action collisions, reduction
in the candidate upper-envelope gap, and separation of the current winning
action from its competitors. This targets the actual query distribution and
avoids using projection depth as the research variable.

### Weighted and modular scalar projections

A broader cheap family maps the bag to a nonnegative weighted sum:

```text
key_w(bag) = sum(weight_w[letter] * count[letter])
```

Every concrete transition maps to subtraction of its weighted delta. Allowing
all abstract sequences that fit the scalar key is a relaxation, so a complete
one-dimensional table is an upper bound. Moderate integer weights distinguish
some letter mixtures while preserving a small contiguous table and strong
action deduplication.

Several weight vectors can be combined by minimum or cost partitioning. A
related quotient tracks total letters plus a modular multiset signature; total
letters supplies the acyclic layer while signature collisions weaken the
bound safely. Random weights are useful only as a baseline. Better weights
should be selected to separate high-score actions that collide in the current
projection.

The final-query shadow experiment now implements the quotient variant with
four deterministic 6-bit signatures. Its minimum preserves 92.0% of rich
prunes on the 28-letter case and 78.9% on the 40-letter `d=16` case, while
also adding a small number of complementary prunes. The result justifies a
parameter/weight-selection sweep but not immediate use as the sole online
fallback.

## Share work across many states

### Symbolic or decision-diagram construction

Many mixed-radix states may have the same finite/dead status, the same winning
action, or bound values differing only by a shared offset. A multi-valued or
edge-valued decision diagram can represent such state sets or value functions
without visiting each state independently. Transition application then acts
on sets of states rather than scalar keys.

This is a genuine construction-time idea, not just table compression, but it
is high risk. Dictionary scores may destroy value sharing even when
reachability compresses well, and decision-diagram variable order is critical.
Before implementing a package, measure:

- hashes of repeated subtables along each mixed-radix dimension;
- run lengths of equal finite/dead status and winning action;
- entropy of `H_rich - U_coarse` after conservative quantization; and
- decision-diagram node counts for reachability alone.

If only reachability compresses, use a symbolic finite-state mask to guide an
ordinary numeric DP. If value residuals also compress, an edge-valued diagram
becomes plausible.

### Coarse baseline plus residual

Store or construct the difference from a cheap complete table rather than the
absolute rich value:

```text
gap(state) = U_coarse(state) - H_rich(state) >= 0
```

The gap is likely to have more repeated zeros and small values than `H_rich`.
By itself this saves storage, not edge work. Its construction value appears
only when combined with conservative quantization, repeated-subtable sharing,
or bounded refinement that stops once the gap is large enough to prove a
concrete prune.

## Cheapen the remaining transition kernel

### Packed projected-state arithmetic

The successful kernel currently subtracts every exact requirement from the
worker bag, updates the support mask, recurses or loads, and restores the bag.
For the modest number of exact dimensions, pack counts into byte or guarded
nibble lanes and pack each action's exact delta in the same format. One or two
word/SIMD subtracts can then update the state; guard bits detect underflow, and
lane-zero detection reconstructs the support needed for the next bucket.

The mixed-radix score key still subtracts by one scalar delta. A prototype
should compare packed subtract/restore with the current support-mask plus
repeated-requirement fit path on the successful kernel, not only on rejected
actions.

### Fixed-point conservative scores

Quantize every adjusted action score upward to a fixed-point integer and run
the projected DP entirely in integer max-plus arithmetic. If at most `K`
actions can remain, either round each action toward positive infinity or add a
single `K`-dependent error allowance at lookup. The result is a slightly
looser but admissible bound.

This removes per-state floating error envelopes and `nextafter`, makes values
deterministic, and may make tiled SIMD reductions simpler. It is secondary
unless arithmetic or vectorization is shown to matter; atomic child loads and
memory traffic remain.

### Fit indexes and specializations

Smaller experiments within the recursive builder include:

- specialize zero- and one-exact-requirement actions;
- narrow packed requirements;
- sub-bucket by wildcard length and repeated-count shape;
- intersect precomputed action bitsets for count thresholds;
- cache support-compatible action lists by exact presence mask; and
- prefetch likely child slots after coarse score ordering.

These target scanned-but-rejected actions. Bitset or trie indexes can make a
multidimensional fit query cheaper, but they do not reduce the number of
successful child transitions. Promote them only if the new scan/failure
counters identify enough headroom.

## Amortize or reuse construction

For repeated identical queries, persist a completed projected table keyed by
the root bag, extraction options, restart score, projection definition, and an
index/version fingerprint. A long-lived process can also reuse immutable
per-class score and letter metadata, but root filtering, projection deltas, and
action deduplication remain query-specific.

Persistence does not help a one-shot command and invalidation is easy to get
wrong, so it should not displace algorithmic work without evidence of a
repeated-query workload. Background refinement is more generally useful
because it can exploit otherwise idle cores even for one query.

## Measurements still needed

The opt-in diagnostics now cover total action scans, fit failures by reason,
fitting-to-dead edges, ready child hits, first-owner claims, ownership
conflicts, dependency-spin iterations, and finite/dead states and edges split
by remaining-letter layer.
The remaining useful diagnostics are:

- the winning action's position in score and coarse-envelope order;
- whole coarse-envelope suffixes rejected after candidate ordering;
- bound-gap histograms and stronger-fallback disagreement on the now-measured
  concrete-DFS query stream;
- overlap among forward-reachable, reverse-completable, and final-query states;
- low-layer perimeter preparation cost and dead lookups avoided when its
  measured finite-state set controls the forward recurrence;
- latency from a rich-hole request to publication, nodes exposed during that
  latency, and subtree leverage of fallback failures; and
- per-bucket action, state, scan, fit, and winner distributions.

New hot-path counters should follow the implemented worker-local pattern and
be merged after construction so instrumentation does not add contention.

The best initial probes do not require a new production builder:

1. Prototype a parallel layered or queued state scheduler that retains the
   measured hierarchical certificates without blocking concrete DFS on a rich
   miss. Use a complete small exact projection for pending entries and report
   request-to-publication latency plus nodes exposed before publication.
2. Run composite-dominance analysis during action preparation without deleting
   actions.
3. Retain the measured layer-seven reverse-completable set as a bitset and
   use it to reject unmarked low-layer children in shadow and live modes.
4. Validate coarse-calibrated logical-edge predictions on more bag
   compositions, then use a bounded concrete-DFS pilot or sampled richer
   lookups to estimate a candidate depth's final-node savings.
5. Hash value and reachability subtables to reject or justify symbolic work.

## Research order

The recommended order is based on ability to remove work and cost of learning,
not on additional projection-depth timings:

0. Make the length-group certificate the default concrete-search loop and
   re-measure depth selection underneath it. It changes the cost model every
   other item in this list is optimizing against, and on the unrelated bag it
   already removes bound quality as the binding constraint. Then decide when to
   pay for the score-descending permutation, whose locality cost is real.

1. Replace synchronous rich-miss construction in the certificate prototype
   with a parallel layered or nonblocking demand scheduler. Use a complete
   smaller exact projection while a requested rich entry is unfinished, and
   prioritize requests by estimated subtree leverage rather than frequency
   alone.
2. Extend the implemented scan, dead-child, ownership, certificate, layer, and
   final-query diagnostics with winner order.
3. Apply the measured layer-seven reverse perimeter to the forward builder,
   then measure composite action dominance and richer forward/reverse overlap.
4. Prototype a setup-versus-search selector using the implemented analytic
   edge preflight and a bounded pruning-value pilot. Then try search-driven
   lookup using the selected smaller projection as the always-available
   fallback and allow nonblocking background refinement.
5. Compare the existing forward recursion with reverse finite-state generation
   and a layered max-plus kernel on a small representative case.
6. Use actual prune misses to select a complementary projection or one adaptive
   group split. Evaluate construction transitions per useful prune.
7. Try packed state arithmetic and fixed-point values on whichever scheduler
   survives the higher-level experiments.
8. Pursue symbolic value construction only if repeated-subtable measurements
   show substantial structure.
9. Add persistent reuse only for a demonstrated repeated-query workload.

## Related research directions

The terminology above comes from pattern databases and abstraction heuristics,
but the proposals still need local validation:

- [Searching Without a Heuristic: Efficient Use of Abstraction][switchback]
  motivates on-demand hierarchical abstraction rather than exhaustive
  precomputation.
- [Online Saturated Cost Partitioning for Classical Planning][online-scp]
  is a useful analogue for moving abstraction work online and extending it
  during search.
- [On Creating Complementary Pattern Databases][complementary-pdb] selects new
  projections to complement observed weaknesses of existing ones.
- [Automated Pattern Database Design][automated-pdb] treats projection choice
  as an optimization problem and uses symbolic PDB construction.
- [External Symbolic Heuristic Search with Pattern Databases][symbolic-pdb]
  describes decision-diagram state-set operations for PDBs.
- [Counterexample-guided Cartesian abstraction refinement and saturated cost
  partitioning][cegar] supplies the closest research vocabulary for adaptive
  wildcard-group splitting.

[switchback]: https://ojs.aaai.org/index.php/AAAI/article/download/7563/7424
[online-scp]: https://ojs.aaai.org/index.php/ICAPS/article/view/15976
[complementary-pdb]: https://www.ijcai.org/proceedings/2017/601
[automated-pdb]: https://dl.aaai.org/Papers/Workshops/2006/WS-06-08/WS06-08-003.pdf
[symbolic-pdb]: https://cdn.aaai.org/ICAPS/2005/ICAPS05-006.pdf
[cegar]: https://edoc.unibas.ch/entities/publication/d96e1300-319e-4c33-9607-ebe5cea1517c
