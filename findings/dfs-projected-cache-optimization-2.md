# Reducing projected score-bound preprocessing time

## Summary

The optimization target is total wall time on long inputs, especially 48 or
more letters. The score cache is not constrained to the historical 64 MiB
default: roughly 1–2 GiB can be dedicated to it when that reduces latency.
Memory is therefore a constraint on candidate configurations, not the
objective and not a reason by itself to prefer compact storage.

The largest measured optimization opportunity is to quotient concrete anagram
classes into distinct projected actions before constructing the projected
score bound.

The projected recurrence tracks only:

```text
(remaining exact-letter counts, remaining wildcard count)
```

but the pre-quotient implementation still traversed all concrete anagram
classes.
Many concrete classes consume the same projected resources and therefore lead
to the same child state. Among classes with identical projected consumption,
only the class with the highest score can contribute to the maximum.

A temporary prototype on the 40-letter `S6` workload reduced preprocessing:

| exact depth | original transitions | quotient transitions | original setup | quotient setup |
|---:|---:|---:|---:|---:|
| 10 | 557,404,675 | 7,968,461 | 3.684s | 0.235s |
| 14 | 7,942,601,788 | 900,312,291 | 63.242s | 9.002s |
| 15 | 27,133,454,068 | 5,919,322,956 | 226.549s | 75.620s |

The number of computed abstract states was unchanged. A complete `d=10`,
top-1 run produced byte-identical output and identical final DFS counters
before and after quotienting.

The initial prototype was removed after measurement. A production
implementation has now been added with projection-specific action records,
representative buckets, work diagnostics, and a temporary opt-out for direct
comparison.

### Implementation status

The production quotient is enabled by default. The current implementation:

- sorts `(projected delta, class ID)` pairs and retains the highest-scoring
  representative for each delta;
- assigns representatives to projection-specific exact-letter buckets or the
  wildcard-only bucket;
- stores flat delta, exact support, repeated exact requirements, lengths,
  partial score, and fixed rounding-error arithmetic in a compact projected
  action;
- reports concrete classes, distinct projected actions, candidate tests,
  fitting transitions, and successful transitions; and
- accepts `NUTRIMATIC_PROJECTED_ACTION_QUOTIENT=0` as a temporary validation
  opt-out.

The automatic depth selector still chooses the largest projected dense table
that fits the memory budget. It does not yet include action fanout or expected
search cost.

The plain-float bottom-up wildcard-vector evaluator is now implemented and
enabled by default for complete projected tables. It processes exact bags in
increasing exact-letter-total layers, owns each wildcard vector in one worker,
and keeps the completed table as plain floats for the final DFS. The former
recursive atomic evaluator remains available with
`NUTRIMATIC_PROJECTED_BOTTOM_UP=0` for differential validation.

This makes the first direct 48-letter depth calibration practical. For top-1,
`d=13` completed the whole program in 5.21 seconds, versus 6.33 seconds at
`d=12`, 6.83 seconds at `d=14`, and 67.66 seconds at `d=15`. At `d=13`,
bottom-up reduced phase-2 setup from 5.88 seconds to 0.95 seconds and
whole-program wall time from 10.10 seconds to 5.21 seconds with identical
output and final search counters. For top-10 the winner moved to `d=14`,
whose 35.27-second phase 2 beat 71.36 seconds at `d=13` and 124.65 seconds at
`d=12`. The measured 63.98-second `d=15` setup alone rules it out for that
target.

Follow-up fit-filtering prototypes show that grouping actions by exact support
helps the current top-down recurrence, but subdividing those groups by
wildcard length does not. At `d=15`, support grouping reduced setup from 36.58
seconds to 24.66 seconds. Adding wildcard-length groups reduced scans further
but increased setup to 26.37 seconds.

A bottom-up wildcard-vector prototype established the production design. It
filters an action against one exact bag once, applies it across all fitting
wildcard counts, and processes exact bags in dependency layers.
In guarded same-binary top-1 comparisons it reduced setup from 6.80 seconds to
1.39 seconds at `d=14`, and from 58.97 seconds to 9.70 seconds at `d=15`.
It produced identical output and final DFS counters; a `d=15` top-1,000 run
also reproduced the existing output hash and all search counters. The
prototype source was removed after measurement.

Follow-up analysis found an exact, closed-form work estimator for that
bottom-up kernel. Summing a small product over projected actions reproduced
the prototype's action-scan and fitting-transition counters exactly at all
three measured depths. This makes action-aware evaluator selection practical
without first running the dynamic program.

The estimator also strengthens the case for combining support grouping with
the vector kernel. At `d=15`, support grouping would reduce exact-bag action
scans from 11.15 billion to 1.43 billion. Enumerating the necessary support
submasks costs only 129 million probes because it happens once per exact bag,
not once per wildcard state. The 12.58 billion wildcard updates remain, so
the realistic combined-work reduction is about 40%, not the raw 7.8x scan
reduction. Indexing exact multiplicities after support would improve that
combined proxy by only another 3%.

A guarded follow-up implemented that combination temporarily. On a 38-letter
workload, support grouping reduced vector-kernel action scans by 6.95x at
`d=14` and 7.87x at `d=15`, but setup improved by only about 12%. The simple
event proxy had predicted a 37–43% reduction. Support grouping is still a
useful secondary optimization, but scan and wildcard-update events cannot be
weighted equally when predicting wall time.

A simpler support-mask sidecar is promising enough to precede support
grouping in the production experiment sequence. A temporary top-down
prototype tested exact support from a contiguous mask array before loading
the 48-byte action record. It reduced setup by 9.8% at `d=10`, 8.2% at
`d=14`, and 15.1% at `d=15`, with identical output and all existing
deterministic counters. A split mask/cold-action layout can retain the current
48 bytes per action rather than duplicating the mask.

The same workload also demonstrates that depth selection must be
evaluator-aware. Top-down evaluation favored `d=14`: `d=15` took 36.57 seconds
end to end versus 23.31 seconds. Bottom-up evaluation reversed the choice:
`d=15` took 12.42 seconds versus 17.43 seconds at `d=14`. Every mode and depth
produced the same retained output.

The exact-symbol set is another selector dimension. A guarded top-down
top-1,000 sweep kept the first 14 rarest symbols exact and varied the 15th.
Choosing `l:2` instead of the usual next-rarest `o:4` reduced the 40-letter
total from 68.60 seconds to 58.95 seconds, a 14.1% improvement. This does not
supersede the rarest prefix for the recommended bottom-up evaluator: exact
`l` leaves 33.2% more final DFS nodes, and its 37.29-second measured search
alone exceeds the established 34.52-second bottom-up `o` total. The static
vector-work estimator also works for arbitrary exact sets and predicts that
`l` would roughly halve setup work, not erase that search deficit.

A final same-binary follow-up resolves two production design questions. The
bottom-up kernel scales substantially better than the recursive kernel:
`d=14` setup fell from 10.13 seconds on one thread to 1.14 seconds on 20
threads. Persistent workers across exact-total layers were indistinguishable
from recreating workers per layer, so a worker-pool rewrite is not required
for initial bring-up. Plain float table access, however, reduced setup by
24.1% at `d=14` and 26.6% at `d=15`, even though the prototype charged an
extra full copy into the existing atomic table. Production bottom-up
evaluation should therefore own plain float storage directly.

Plain storage also made `d=16` competitive. In a guarded same-binary
top-1,000 comparison, `d=15` totaled 34.52 seconds and `d=16` totaled 34.01
seconds with identical output. A 0.52-second, 1.5% difference is smaller than
the timing variation seen elsewhere, while `d=16` uses 2.65x the value-table
memory. Treat them as tied for this workload and prefer `d=15` as the robust
default unless representative calibration consistently values the deeper
bound.

A temporary reduced-precision prototype rounded every published projected
value upward to IEEE binary16 or bfloat16 while retaining the existing
32-bit slots. Binary16 preserved every output hash and increased final nodes
by 0.6–2.9% in the useful 28-letter top-100/top-1,000 comparisons, plus 0.8%
on an unrelated 29-letter workload. Bfloat16 increased nodes by 4.3–15.8%.
The extra three fraction bits matter much more than bfloat16's unnecessary
exponent range.

Those results establish pruning accuracy, not a reason to pack. With a
1–2 GiB working budget, the 40-letter float payloads through `d=18`
(683.44 MiB) fit, and `d=19` (1.56 GiB) fits near the upper end. Packing
`d=16` merely to cross 64 MiB is no longer valuable. Keep binary16 only as a
same-depth wall-time experiment: it earns production priority if reduced
memory traffic repays conversion and weaker pruning. It should not block the
plain-float vector kernel, selector calibration, or the initial float
persistence format. Bfloat16 is not recommended.

A new 34-letter top-1 capacity comparison reinforces that distinction. A
32.04 MiB `d=15` float projection completed phase 2 in 59.94 seconds; the
74.75 MiB all-exact projection completed in 156.06 seconds. The larger table
reduced search by only 1.1 milliseconds. This is a current top-down result,
not a prediction for the vector kernel, but it directly disproves selecting a
depth merely because a larger memory budget makes it fit.

Projection depth is also strongly top-N dependent. A guarded production
top-down sweep on the 28-letter workload selected `d=7` for top-1, `d=9` or
`d=10` for top-10, `d=12` for top-100, and effectively tied `d=12` with
`d=13` for top-1,000. The bound construction work at a fixed depth was
identical for every target; only the downstream search payoff changed.
Result-count and score-floor policy must therefore be inputs to depth
selection, while a persisted completed table can remain independent of them.

A temporary raw-table persistence prototype validated that reuse directly.
On the 38-letter `d=14` workload, compute plus dump took 7.896 seconds of
phase-2 setup; a warm raw load took 0.135 seconds and reproduced byte-identical
top-1 output and all final DFS counters. The table built for top-1 was then
reused for top-1,000 and reproduced the established output hash and search
counters. Persistence should follow the production plain-table vector kernel:
the read-only value layout can be serialized or mapped directly without
retaining the current atomic representation.

Coarse-to-fine construction was less successful. A temporary coarse
certificate skipped 32–66% of successful deep transitions and reduced
20-thread setup by 36–50% on three comparisons, but barely reduced the number
of deep states. The added or deferred work made setup plus final search 17%,
57%, and 99% slower. On one thread it moved enough parallel setup work into
the serial search to give a 13% total improvement, but that mode is already
superseded by the regular bottom-up kernel. Do not put coarse certificates in
the initial production evaluator.

Very small coarse projections are now deployment-specific fallbacks rather
than a general priority. Four temporary 6-bit modular projections reported
about 420 KiB of table-plus-delta storage and independently pruned
91.9–98.7% of all bound queries on three workloads, close to the rich
projection. They occasionally pruned queries the rich projection did not. A
length-only fallback was much weaker and added no unique prune beside the rich
bound. With 1–2 GiB available, revisit modular tables only if they reduce
wall time or a separately supported low-memory deployment needs them.

## Workload

The measurements used:

```sh
source ~/code/nutrimatic/.env/bin/activate
source ./s.sh
export IDX=~/code/nutrimatic/idx/wiki-merged.5.index

NUTRIMATIC_PROJECTED_SCORE_D=d \
build/dfs-anagrams "$IDX" "${S6:0:40}" \
    -m 4 -n 1 -C 32 -F -p 100000 -T 20
```

The normalized input and phase-1 result were:

```text
aabbcddeeeeeeffhiiikkllnnnooooqstuuuvwyz
914379 entries
490329 classes
6231011 trie nodes
```

The 20 present symbols, in corpus-rarity order with input multiplicities,
were:

```text
q:1 z:1 f:2 w:1 v:1 y:1 b:2 c:1 k:2 h:1
u:3 t:1 d:2 s:1 o:4 l:2 i:3 n:3 a:2 e:6
```

The intended production workload extends to 48 or more letters, where a
single current-binary run takes tens of minutes. New capacity experiments
therefore use shorter prefixes to avoid spending that time before the
bottom-up evaluator exists. They are evidence about evaluator and storage
tradeoffs, not substitutes for final 48+ validation.

The ultimate metric is whole-program wall time. Most tables below report
phase-2 setup plus phase-2 search because phase 1 is unchanged within a
depth/storage A/B; adding the common phase-1 time does not change the winner.
Final production validation must report the full invocation time on 48+
letters.

The machine currently has enough free memory to dedicate roughly 1–2 GiB to
the score table. Larger-budget comparisons use `-C 2048` so forced float
depths are not rejected by the historical default. The first focused
capacity boundary used:

```sh
NUTRIMATIC_PROJECTED_SCORE_D=d \
build/dfs-anagrams "$IDX" "${S6:0:34}" \
    -m 4 -n 1 -C 2048 -F -p 100000 -T 20
```

It normalized to:

```text
aabbddeeeeeeffhiillnnnoooostuuvwyz
411220 entries
190545 classes
2821573 trie nodes
```

## Current projected recurrence

For an exact-letter set `D`, the projected state is:

```text
key_D     = mixed-radix remaining counts for letters in D
wild_left = total remaining letters outside D
flat key  = key_D * (W + 1) + wild_left
```

Each concrete class is converted to:

```text
exact consumption vector
wildcard consumption count
flat projected delta
```

The recurrence is:

```text
A(x) = max over projected-fitting concrete classes c:
           score(c) + restart + A(x - delta(c))
```

The implementation already computes a collision-free flat delta for every
class. Nevertheless, each state scans the concrete class bucket, tests the
class against the abstract state, subtracts its exact requirements, performs a
recursive atomic memo lookup, restores the worker state, and evaluates the
candidate score.

At `d=15`, the current run computed 3,411,183 states but processed
27,133,454,068 successful transitions: about 7,954 successful transitions per
computed state. Failed fit tests are additional work and are not included in
that counter.

## Exact projected-action quotient

Let two concrete classes have the same projected consumption:

```text
delta(c1) = delta(c2)
```

They have the same abstract fit condition and lead to the same child state. If:

```text
score(c1) <= score(c2)
```

then:

```text
score(c1) + restart + A(x - delta)
    <=
score(c2) + restart + A(x - delta)
```

Therefore `c1` can never determine the projected maximum and may be discarded
from the projected recurrence.

This transformation is exact for the projected bound. It does not alter the
projection or weaken its pruning quality. The full concrete class list remains
available to the final DFS.

The current scores are logarithms of positive integer counts. They are
nonnegative, so the retained maximum-score class also has at least as large a
class-score contribution to the existing absolute rounding-error allowance as
any discarded class with the same child. The existing upward-rounding proof
therefore remains compatible with quotienting.

### Static action counts

Grouping the 490,329 concrete classes by projected delta produced:

| exact depth | wildcard letters | distinct projected actions | concrete/action ratio |
|---:|---:|---:|---:|
| 10 | 27 | 12,499 | 39.23x |
| 11 | 24 | 24,712 | 19.84x |
| 12 | 23 | 36,624 | 13.39x |
| 13 | 21 | 59,676 | 8.22x |
| 14 | 20 | 85,240 | 5.75x |
| 15 | 16 | 151,440 | 3.24x |
| 16 | 14 | 222,643 | 2.20x |

The transition reduction can be larger than the global class/action ratio.
Later forced-letter buckets have much higher collision rates, and reachable
state traversal spends substantial time in those later buckets.

For `d=15`, selected per-bucket ratios included:

```text
bucket 0:   1.85x
bucket 6:   4.57x
bucket 9:   8.73x
bucket 11: 15.37x
bucket 13: 18.02x
wild only: 36.00x
```

This explains why the measured successful-transition reduction at `d=15` was
4.58x rather than the global 3.24x.

### Prototype

The temporary prototype:

1. grouped concrete classes by the existing flat `score_key_delta`;
2. retained the class ID with the largest `best_member_log_score`;
3. sorted the retained representatives by their rarest required exact letter;
4. retained a separate wildcard-only bucket; and
5. preserved descending class-length order within each bucket.

It reused the selected concrete class record as the projected representative.
That is valid because classes with the same flat projected delta have the same:

- exact requirements;
- wildcard length;
- total length; and
- exact support mask.

The prototype's hash-table construction time was included in the reported
setup time.

### Measured results

#### `d=10`

```text
projected capacity: 48,384 states
computed states:    25,192
```

| implementation | setup | successful transitions |
|---|---:|---:|
| concrete classes | 3.683991s | 557,404,675 |
| projected quotient | 0.235136s | 7,968,461 |

This is a 69.95x transition reduction and a 15.67x setup reduction.

A complete top-1 run produced the same SHA-256 stdout hash in both modes:

```text
908e08189cf9f5d0d65340bf39235c0cfde87f9922db357bbba2fbb63129ef74
```

Both runs also reported:

```text
22,709,308 DFS nodes
93 solutions
5 spellings expanded
1 retained
```

#### `d=14`

```text
projected capacity: 1,741,824 states
computed states:      849,189
```

| implementation | setup | successful transitions |
|---|---:|---:|
| concrete classes | 63.242077s | 7,942,601,788 |
| projected quotient | 9.002499s | 900,312,291 |

This is an 8.82x transition reduction and a 7.02x setup reduction.

#### `d=15`

```text
projected capacity: 7,050,240 states
computed states:    3,411,183
```

| implementation | setup | successful transitions |
|---|---:|---:|
| concrete classes | 226.549499s | 27,133,454,068 |
| projected quotient | 75.620405s | 5,919,322,956 |

This is a 4.58x transition reduction and approximately a 3.00x setup
reduction. The original timing is the supplied `results/s6.40.C32`
measurement, while the quotient timing used the restored post-cache-removal
tree. The transition counts are deterministic and the wall-time difference is
large enough that this build difference does not affect the conclusion.

### Later unquotiented reruns

Later restored-tree, top-1,000 runs showed substantial wall-time variation
without changing the deterministic state and transition counts:

| depth | capacity | computed states | transitions | setup | search |
|---:|---:|---:|---:|---:|---:|
| 15 | 7,050,240 | 3,411,183 | 27,133,454,068 | 326.450s | 38.130s |
| 16 | 18,662,400 | 8,787,405 | 51,881,790,162 | 764.141s | 19.078s |

The later `d=15` run had the same state and transition counts as the earlier
226.549-second setup run, but took 326.450 seconds to preprocess. Absolute
wall times in this document should therefore be treated as individual
measurements, not stable predictions. The deterministic work counts and the
large quotient reductions are the stronger evidence.

An independent `dfs-anagrams` process may also be active on the machine.
Earlier timing runs did not consistently check for it and may include CPU or
memory-bandwidth contention. Future timing comparisons should first run:

```sh
pgrep -a -x dfs-anagrams
```

and defer the measurement if it reports another process. This does not affect
the deterministic state, candidate, fit, or transition counts. The later
support-group and bottom-up A/B comparisons below checked immediately before
both halves.

The `d=16` run also reinforces that choosing the largest table that fits the
memory budget is not a good time-based policy. Its tighter bound roughly
halved final search time relative to that later `d=15` run, but preprocessing
grew to more than 51.8 billion successful transitions and dominated the
end-to-end result.

### Production `d=10` comparison

A same-session top-1 comparison of the production compact action
implementation and its opt-out produced:

| implementation | actions | setup | successful transitions |
|---|---:|---:|---:|
| concrete actions, quotient off | 490,329 | 5.708552s | 557,404,675 |
| compact projected actions | 12,499 | 0.296500s | 7,968,461 |

Both modes computed 25,192 projected states and reported:

```text
22,709,308 DFS nodes
93 solutions
5 spellings expanded
1 retained
```

Their stdout SHA-256 hashes were identical:

```text
908e08189cf9f5d0d65340bf39235c0cfde87f9922db357bbba2fbb63129ef74
```

The production setup was 19.25x faster than the same-session opt-out. It was
also faster than the earlier 0.305-second representative-ID production
measurement and the 0.235-second temporary prototype was close enough that
wall-time noise dominates that small difference.

## Depth scaling

Before quotienting, forced-depth measurements with 20 preprocessing threads
were:

| depth | capacity | computed states | transitions | setup |
|---:|---:|---:|---:|---:|
| 10 | 48,384 | 25,192 | 557,404,675 | 3.684s |
| 11 | 172,800 | 87,428 | 1,717,051,848 | 9.528s |
| 12 | 331,776 | 166,224 | 2,541,817,988 | 16.074s |
| 13 | 912,384 | 448,377 | 5,461,965,342 | 40.951s |
| 14 | 1,741,824 | 849,189 | 7,942,601,788 | 63.242s |
| 15 | 7,050,240 | 3,411,183 | 27,133,454,068 | 226.549s |

Table bytes alone are a poor preprocessing-cost predictor. The `d=13` table
uses only about 3.5 MiB but already needs 5.46 billion successful
transitions.

The `d=14` to `d=15` step makes `o:4` exact. The state space grows by about
4.05x. After quotienting, transitions grow from 0.90 billion to 5.92 billion,
or 6.57x, because making `o` exact also distinguishes actions which previously
collided.

The automatic selector should eventually estimate both projected state count
and projected action fanout, rather than selecting the largest table fitting
the memory budget.

## Thread scaling

A warm thread sweep used the unquotiented `d=10` recurrence with 557,404,675
successful transitions:

| threads | setup | speedup | efficiency |
|---:|---:|---:|---:|
| 1 | 23.668s | 1.00x | 100% |
| 2 | 13.590s | 1.74x | 87% |
| 4 | 8.082s | 2.93x | 73% |
| 8 | 5.377s | 4.40x | 55% |
| 10 | 4.849s | 4.88x | 49% |
| 20 | 4.194s | 5.64x | 28% |

The final ten hardware threads improved wall time by only 13.5%. More
threading remains useful for minimum wall time, but it cannot compensate for
the transition count.

The current parallel strategy distributes fitting root candidates and lets
their recursive closures converge in a shared atomic table. Each child access
performs an atomic load and, for an unseen state, a compare-and-exchange.
Converging root subproblems also wait on states another worker is calculating.

An instruction-only Callgrind profile of a smaller `d=5` projection attributed
about 9.44 billion instructions to projected preprocessing. Approximately
7.68 billion, or 81% of that total, were attributed to the projected
candidate-transition handler. This supports reducing action transitions before
tuning synchronization or state-level rounding.

## Implementation results and next sequence

With the 1–2 GiB allowance and 48+ wall time as the target, the revised
forward sequence is:

1. retain the implemented plain-float bottom-up wildcard-vector evaluator;
2. replace largest-fitting depth selection with result-target-aware
   calibration, using the new 48-letter top-1 and top-10 boundaries as the
   first presets;
3. measure the split support sidecar, followed only if useful by grouped
   ranges and a complete observed-presence-list layout;
4. add versioned float persistence for repeated identical projections;
5. run an actual packed-binary16 same-depth wall-time A/B, then compare
   neighboring precision/depth pairs only if packing wins; and
6. leave coarse modular bounds to a low-memory product mode or a future
   measured throughput experiment.

This sequence does not prioritize an encoding or index merely because it
reduces bytes. Compact action layouts remain important where they reduce
loads and improve locality; packed value storage must demonstrate the same
wall-time benefit.

### 1. Projected-action quotient: implemented

After projected deltas are constructed:

1. make an array of `(score_key_delta, class_id)`;
2. sort by delta;
3. retain the highest-scoring class in each delta group;
4. assign each retained action to its rarest exact-letter bucket, or the
   wildcard-only bucket; and
5. sort by descending total length within each bucket.

Sorting IDs needs little temporary memory and is more predictable than a large
`unordered_map`.

The implementation uses projection-specific bucket ranges and a projected
length-search helper. The existing `first_length_candidate()` helper indexes
the concrete `fit_classes` array directly and therefore could not be reused
for the ordered action vector.

During production validation, the implementation retains a temporary opt-out
switch for the quotient. This permits the same binary to compare quotient and
unquotiented output, DFS counters, projected-state counts, and thread
behavior. It can be removed after the focused comparisons pass.

Useful diagnostics are:

```text
concrete class count
distinct projected action count
attempted candidate tests
fitting transitions including dead children
successful transitions
```

Per-bucket concrete/action counts remain a useful optional diagnostic but are
not yet reported.

### 2. Compact projected-action record: implemented

The projected recurrence does not need wildcard identities or all concrete
requirements. The production action contains:

```text
flat projected delta
exact support mask
offset/count for repeated exact requirements
wildcard length
total length
class score plus restart
fixed rounding-error component
```

This avoids repeatedly walking concrete wildcard requirements during
projected fit testing, subtraction, and restoration. It also removes the
indirection through several concrete-class arrays used by the prototype.

For the `d=15` input, only six exact symbols have multiplicity greater than
one:

```text
f b k u d o
```

Most actions therefore need only an exact support mask and few or no repeated
exact-count checks.

Precomputing:

```text
partial_score = class_score + restart
error_base    = abs(class_score) + abs(restart)
```

avoids recalculating invariant arithmetic for every successful transition.
The child magnitude and final `+ 1` are still applied per transition in the
same order as the original rounding-error calculation.

### 3. End-to-end depth sweep: completed

The selector target is:

```text
projected setup time + final DFS search time
```

not preprocessing alone.

In individual quotient prototype runs, measured setup was approximately:

```text
d=14:  9.0s
d=15: 75.6s
```

One unquotiented `d=15` run took about 20.2 seconds to search, while the later
restored-tree run took 38.1 seconds with identical DFS counters. Quotienting
leaves the bound unchanged and should leave matched-build search work
unchanged, but the observed timing spread makes cross-run arithmetic too noisy
to select a depth confidently.

The same-session compact-action top-1,000 sweep produced:

| depth | actions | computed states | candidate tests | fitting transitions | successful transitions |
|---:|---:|---:|---:|---:|---:|
| 13 | 59,676 | 448,377 | 4,648,877,468 | 423,826,039 | 397,143,388 |
| 14 | 85,240 | 849,189 | 12,759,904,105 | 955,069,092 | 900,312,291 |
| 15 | 151,440 | 3,411,183 | 93,064,759,838 | 6,141,176,817 | 5,919,322,956 |

| depth | setup | search | total | final DFS nodes |
|---:|---:|---:|---:|---:|
| 13 | 2.417543s | 137.672081s | 140.089624s | 1,359,487,854 |
| 14 | 5.301802s | 97.458439s | 102.760241s | 862,949,213 |
| 15 | 44.530226s | 28.340842s | 72.871068s | 342,949,072 |

All three depths produced the same top-1,000 stdout SHA-256:

```text
5cf8a34f71e78a47270897fb32161526807fb2ff906580708986f8e42f0ec601
```

They also expanded 15,900 spellings and retained 1,000 results. `d=15` is the
best end-to-end depth among those tested despite its much larger preprocessing
cost. The setup/search tradeoff confirms that neither table bytes nor setup
time alone is a sufficient selector target.

### 4. Projection-specific support filtering: measured and prototyped

After quotienting, action support can be cached or indexed by exact presence
mask. There are at most:

```text
2^d
```

such masks, and many projected multiplicity states share one mask.

Temporary counters split the existing fit predicate into cumulative stages.
Every row had already passed the bucket's total-length cutoff:

| depth | candidate tests | exact support fits | + wildcard fits | + repeated counts fit |
|---:|---:|---:|---:|---:|
| 14 | 12,759,904,105 | 1,798,162,293 | 1,419,147,752 | 955,069,092 |
| 15 | 93,064,759,838 | 12,704,444,042 | 9,559,828,221 | 6,141,176,817 |

At `d=15`, exact support alone rejects 86.35% of length-filtered candidates.
Wildcard capacity then rejects 24.75% of the support-fitting candidates, and
exact multiplicity rejects 35.76% of those remaining. Only 6.60% of all
length-filtered tests fit completely, while 96.39% of fitting transitions
reach a finite child.

This makes exact support the largest single rejector, but it also shows that a
support-only index cannot remove all fit work.

#### Non-replicating support-group prototype

A temporary exact prototype:

1. compressed each exact support mask to `d` bits;
2. reordered projected actions by full support mask and descending total
   length;
3. stored a dense start offset for every one of the `2^d` support groups; and
4. at each state enumerated support submasks containing the forced rarest
   present letter.

The action records were not duplicated. The prototype retained the same
computed states, fitting and successful transitions, DFS counters, and output.
Its deterministic index costs were:

| depth | nonempty action support groups | observed state masks | subset probes | nonempty group probes | scanned actions |
|---:|---:|---:|---:|---:|---:|
| 14 | 7,947 | 4,649 | 105,063,137 | 98,113,882 | 1,798,172,798 |
| 15 | 13,762 | 8,781 | 737,765,908 | 673,735,882 | 12,704,458,345 |

The small difference between the scanned-action count and the support-fit
counter is the separately evaluated root. Compared with the current layout,
support grouping reduced scanned actions 7.10x at `d=14` and 7.33x at
`d=15`.

The dense group-start table itself is small: 128 KiB at `d=14` or 256 KiB at
`d=15` with 64-bit offsets. It grows exponentially, so a production version
needs a maximum supported `d` and a fallback to the current bucket layout.

The process-guarded top-1 `d=14` A/B measured:

| layout | setup | candidate tests |
|---|---:|---:|
| current length-sorted bucket | 4.893190s | 12,759,904,105 |
| full-support groups | 4.204327s | 1,798,172,798 |

This is a 14.1% setup reduction, much smaller than the 7.10x candidate-test
reduction. Submask enumeration, one length binary search per support group,
and the unchanged 955 million fitting transitions limit the gain. Treat this
timing as one guarded A/B, not a stable machine-wide prediction.

#### Support plus wildcard-length grouping

A second temporary prototype subdivided each exact-support group by wildcard
length. Actions were sorted by:

```text
(compressed exact support, descending wildcard length,
 descending total length)
```

Each `(support, wildcard length)` group retained one contiguous action range.
At a state, the recurrence enumerated fitting support submasks, skipped groups
whose wildcard length exceeded `wild_left`, and performed the total-length
search within each remaining group. Actions and action IDs were not
duplicated.

This removes wildcard-fit failures from the scanned-action count, but greatly
increases the number of groups:

| depth | support groups | support/wildcard groups |
|---:|---:|---:|
| 14 | 7,947 | 46,389 |
| 15 | 13,762 | 70,275 |

Process-guarded top-1 continuation measurements produced:

| depth | layout | scanned actions | setup |
|---:|---|---:|---:|
| 14 | current buckets | 12,759,904,105 | 5.486194s |
| 14 | support groups | 1,798,172,798 | 4.250910–4.360758s |
| 14 | support/wildcard groups | 1,419,158,257 | 4.424136–4.424373s |
| 15 | current buckets | 93,064,759,838 | 36.582814s |
| 15 | support groups | 12,704,458,345 | 24.662396s |
| 15 | support/wildcard groups | 9,559,842,524 | 26.373162s |

The `d=15` rows used the same temporary binary. Support grouping reduced setup
32.6% relative to the current layout. Wildcard subdivision then removed
3.145 billion scans, or 24.75% of the support-filtered work, but made setup
6.9% slower than support grouping alone. The paired `d=14` runs agree:
wildcard subdivision removed 21.08% of scans but was about 2.8% slower than
the mean support-only time.

All three layouts computed the same states, fitting transitions, successful
transitions, final DFS counters, and top-1 output. The prototype source was
removed after measurement.

The result is useful beyond this particular layout: candidate-test count is
not a sufficient selector for fit indexes. A finer partition can lose even
when it removes billions of predicate calls, because group enumeration,
repeated small binary searches, and worse locality replace one linear scan.

#### Contiguous support-mask sidecar

The support-group experiments remove rejected actions from the traversal but
replace a linear scan with submask enumeration, range lookups, binary
searches, and, in the vector prototype, indirect action-ID loads. A simpler
structure-of-arrays experiment retained the existing action order and added
one contiguous `uint64_t` exact-support mask per action.

For every length-filtered action, the temporary top-down kernel:

1. loaded the mask from the sidecar;
2. rejected it if it was not a subset of the current exact presence mask; and
3. loaded the existing 48-byte action only after support fit, then checked
   wildcard capacity and repeated exact counts.

The existing candidate-test counter was incremented before the sidecar check,
so all reported deterministic counters remained directly comparable. The
prototype built the sidecar even in its disabled baseline; its construction
and memory footprint are included in both halves.

Every run checked immediately beforehand for another `dfs-anagrams` process.
The `d=10` and `d=14` rows are four-run and three-run medians. The `d=15` row
is the mean of two opposite-order pairs:

| depth | sidecar disabled | sidecar prefilter | setup reduction |
|---:|---:|---:|---:|
| 10 | 0.300668s | 0.271336s | 9.8% |
| 14 | 5.295030s | 4.860717s | 8.2% |
| 15 | 40.281683s | 34.192083s | 15.1% |

The two individual `d=15` comparisons were:

```text
baseline 40.928985s -> sidecar 34.551687s
sidecar 33.832479s  <- baseline 39.634380s
```

All runs produced the established top-1 SHA-256:

```text
908e08189cf9f5d0d65340bf39235c0cfde87f9922db357bbba2fbb63129ef74
```

They also preserved computed states, candidate tests, fitting and successful
transitions, DFS nodes, solutions, spelling expansions, and retained results.
The sidecar changes only which array is touched before the exact-support
reject.

The prototype intentionally duplicated the mask and therefore added eight
bytes per action. Production need not pay that cost. Removing the mask from
the cold action leaves 36 bytes of fields; storing that record at eight-byte
alignment rounds it to 40 bytes. The eight-byte mask sidecar then preserves
the current total of 48 bytes per action while making rejected scans dense.
This requires confirming that the current explicit 16-byte action alignment
has no separate ABI or code-generation requirement.

The result is not yet a bottom-up measurement, but it changes the experiment
priority. Try the split support/cold-action layout in the plain vector kernel
before adding support groups. It retains sequential indexing and avoids
submask enumeration and action-ID indirection. If it reproduces the
low-teens improvement already measured for grouped vector evaluation, prefer
the sidecar. Keep grouping only if a same-binary vector A/B shows a further
gain after the split layout.

#### Complete lists per presence mask under a larger budget

An on-demand list for every presence mask actually reached by the `d=14` run
would contain 6,773,698 32-bit action IDs, or 25.8 MiB before vector and
allocator overhead. At `d=15`, 17,294,448 IDs require 66.0 MiB. The largest
single `d=15` list contains 36,289 IDs.

That replication was previously treated as prohibitive relative to a 32 MiB
score-cache budget. It is not prohibitive under the actual 1–2 GiB working
budget. Capacity alone is therefore no longer a reason to reject this layout.

Those byte counts cover masks observed by the top-down closure. A full
bottom-up evaluator visits every stored exact bag and can require more
presence masks, so 25.8–66.0 MiB must not be used as its arena estimate. The
exact bottom-up size is statically countable by summing, for each action
support, the stored presence masks that contain it. Report that value before
allocation and reject or cap the layout if the full arena plus value table
exceeds the working allowance.

It still has unmeasured costs: list construction, allocator or arena metadata,
and an indirect cold-action load for every retained ID. The grouped vector
prototype already shows that removing almost seven-eighths of action scans
improves setup by only about 12%, so a complete-list cache should not jump
ahead of the plain vector kernel or split sidecar. It is now a valid contained
competitor to non-replicating support groups, however. Store all IDs in one
flat arena, build lists only for observed masks, include build time in the
A/B, and keep it only if total setup falls beyond the simpler layouts.

#### Refined recommendation

Make a split support-mask/cold-action layout the first support-filtering
optimization:

1. scan exact supports from a contiguous `uint64_t` sidecar;
2. load wildcard length, repeated requirements, score, and delta only after
   support fit;
3. remove the duplicated support from the cold record and target 40 bytes at
   eight-byte alignment; and
4. validate that layout in both the top-down fallback and plain vector kernel.

Keep non-replicating support-only grouping as a compatible secondary
experiment:

1. store the compressed `d`-bit support key in each projected action;
2. keep one descending-total-length range per nonempty support;
3. use 32-bit group offsets where the action count permits;
4. retain the current layout when `2^d` metadata or subset enumeration is too
   expensive.

With the larger memory budget, add a complete observed-presence-list layout
to the same secondary A/B. Compare it against grouping at equal depth rather
than assuming that the measured top-down arena is disqualifying. Compute the
larger bottom-up arena exactly before enabling it. Its priority comes from any
measured reduction in support-filtering wall time, not from cache density.

The later bottom-up prototype below produced a much larger improvement without
support grouping. Do not productionize support grouping solely as an
optimization of the current top-down traversal. Instead, retain the current
action layout for validation, measure the split sidecar with bottom-up
evaluation, and prototype support-grouped ranges only if profiling or the A/B
shows remaining support-scan cost.

Do not add wildcard length to the local grouping key. It reduces the most
visible work counter but regresses measured setup at both tested depths.
Wildcard-vector batching remains the better place to amortize wildcard
filtering because it can reuse one exact-bag action filter across all wildcard
counts instead of adding per-state group traversal.

Per-exact-symbol bitsets remain a plausible fallback experiment: they require
little base metadata and avoid action duplication, but they introduce a
bitset combine/scan for every state. Reintroducing the former general
candidate-cache machinery is unnecessary.

### 5. Bottom-up wildcard-vector evaluation: implemented

The production evaluator now follows the measured design:

1. allocate the complete projected value payload as plain `float`;
2. build lists of stored exact keys by total remaining exact letters;
3. evaluate the wildcard-only vector from low to high wildcard count;
4. dynamically schedule the exact bags in one dependency layer across the
   requested preprocessing threads;
5. filter an action once against the exact bag, then update every fitting
   wildcard count;
6. publish each owned wildcard vector directly into the plain table; and
7. calculate the omitted root from its already-complete children.

The layer lists currently use one 32-bit exact-key ID per stored exact bag.
That metadata is modest in the winning measured regions but is not included
in the reported score-cache payload. Before enabling very deep projections
near the 1–2 GiB value-table limit, either charge this side metadata to the
working allowance or generate layers without retaining every key.

The recursive atomic evaluator remains available as a same-binary opt-out:

```sh
NUTRIMATIC_PROJECTED_BOTTOM_UP=0
```

The focused test suite now compares bottom-up with that evaluator for retained
spellings and final DFS counters, in addition to the existing quotient
differential.

The flat layout stores every wildcard count contiguously for one exact bag:

```text
key = exact_key * wild_span + wild_left
```

For a fixed exact bag:

- the forced exact-letter bucket is fixed;
- exact-fit decisions are identical for all wildcard counts; and
- an action's child wildcard entries form a shifted contiguous range.

A temporary bottom-up kernel:

1. process exact bags in increasing exact-letter total;
2. schedule exact bags in the same layer dynamically across 20 threads;
3. filter projected actions against the exact bag once;
4. update all fitting wildcard counts together;
5. publish every wildcard value after the action pass; and
6. evaluate the wildcard-only vector in increasing wildcard order.

Every action consumes the forced exact letter while an exact letter remains,
so its child lies in an earlier exact-total layer. The wildcard-only base layer
depends only on smaller wildcard counts.

The prototype retained the existing atomic float allocation and access
helpers, but needed no recursive lookup, compare-and-exchange, computing
sentinel, or wait loop. The later storage A/B below found that plain floats
are worthwhile and should be used by the production bottom-up evaluator.

#### Guarded A/B results

Each pair used the same temporary binary and checked for another
`dfs-anagrams` process immediately before each half:

| depth | evaluator | computed states | action-fit tests | fitting transitions | successful transitions | setup |
|---:|---|---:|---:|---:|---:|---:|
| 10 | top-down | 25,192 | 51,697,880 | 8,574,747 | 7,968,461 | 0.281s |
| 10 | bottom-up vector | 48,384 | 4,884,280 | 15,846,204 | 14,610,881 | 0.262s |
| 14 | top-down | 849,189 | 12,759,904,105 | 955,069,092 | 900,312,291 | 6.803s |
| 14 | bottom-up vector | 1,741,824 | 1,368,221,595 | 1,999,879,676 | 1,894,006,955 | 1.394s |
| 15 | top-down | 3,411,183 | 93,064,759,838 | 6,141,176,817 | 5,919,322,956 | 58.967s |
| 15 | bottom-up vector | 7,050,240 | 11,154,835,445 | 12,580,372,385 | 12,184,378,227 | 9.700s |

The action-fit columns describe the analogous outer scan but are not exactly
the same event: top-down counts length-filtered action tests per state, while
bottom-up counts action tests per exact bag and the small wildcard-only
base-vector work. Fitting and successful transitions both count action/state
pairs and are directly comparable.

At `d=14`, the vector kernel computed 2.05x as many states and 2.10x as many
successful transitions, but reduced setup 4.88x. At `d=15`, it computed 2.07x
as many states and 2.06x as many successful transitions, but reduced setup
6.08x. Reusing exact fitting across wildcard counts and eliminating recursive
atomic coordination more than compensated for full-table coverage.

`d=10` was effectively tied. Its small reachable closure does not provide
enough work to amortize full-table evaluation and layer scheduling. A
production policy should retain the top-down kernel for small projections, or
select between kernels using estimated exact-bag/action work.

Every top-1 pair produced byte-identical output and identical final DFS
counters. The bottom-up `d=15`, top-1,000 run additionally reproduced:

```text
stdout SHA-256:
5cf8a34f71e78a47270897fb32161526807fb2ff906580708986f8e42f0ec601

342,949,072 DFS nodes
24,804 solutions
15,900 spellings expanded
1,000 retained
```

Its setup was 8.064 seconds and search was 30.958 seconds, for 39.022 seconds
end to end. The setup difference from the 9.700-second top-1 run is another
example of the machine-wide timing variation already noted above; the
deterministic work counts were identical.

#### Production recommendation

Keep the implemented bottom-up wildcard-vector kernel as the default for
complete projected tables, with the recursive atomic evaluator and plain
action layout retained as validation baselines:

1. retain quotient projected actions and the omitted root plane;
2. continue pre-bucketing exact keys by total exact letters in the winning
   depth region, but account for or eliminate that metadata near the memory
   limit;
3. retain plain float values rather than atomic sentinel words;
4. retain simple per-layer dynamic scheduling; persistent workers did not
   improve the measured kernel;
5. filter support and repeated exact counts once per exact bag;
6. update the contiguous wildcard vector with action-outer loops;
7. round and publish once per projected state, preserving the existing
   upward-bound proof;
8. split exact-support masks from cold action data and validate the sidecar
   before adding support-grouped ranges behind an internal layout switch; and
9. retain top-down recursion as a fallback when the projected table is
   incomplete, the wildcard span is too small, or estimated bottom-up work is
   worse.

Correctness is established with the current action layout. Measure the split
support/cold-action vector kernel before enabling a new layout by default. A
later same-binary prototype measured a consistent but modest setup improvement
from support grouping. Keep the current layout as a validation baseline and
avoid making the grouped layout the default without a production A/B.

A full viability mask is now lower priority. At `d=15`, 96.85% of bottom-up
fitting transitions reached a finite child, so it can skip at most a small
fraction of the measured transition work unless it enables a more substantial
vectorization technique. Follow-up layer instrumentation localizes the
exceptions near the terminal boundary; a small exact terminal perimeter is a
more plausible later companion to a coarse low-memory fallback than a
full-table viability mask.

The prototype source was removed after measurement.

#### Production evaluator results

The implemented plain evaluator reproduced the established 40-letter,
`d=15`, top-1,000 output hash and every deterministic preprocessing and search
counter:

```text
stdout SHA-256:
5cf8a34f71e78a47270897fb32161526807fb2ff906580708986f8e42f0ec601

7,050,240 projected states
11,154,835,508 candidate tests
12,580,372,385 fitting transitions
12,184,378,227 successful transitions
342,949,072 final DFS nodes
24,804 solutions
15,900 spellings expanded
1,000 retained
```

Its measured setup was 9.653 seconds and search was 22.388 seconds, for
32.041 seconds of phase 2. This run is slower in setup than the best temporary
plain-table measurement, but faster in final search than the earlier
comparison; the exact counters and output establish equivalence while the
existing machine-wide timing variation still applies.

On the 28-letter `d=12`, top-1,000 workload, a same-binary production
comparison measured:

| evaluator | setup | search | final DFS nodes |
|---|---:|---:|---:|
| recursive atomic | 0.155s | 0.544s | 11,331,110 |
| bottom-up plain | 0.048s | 0.516s | 11,331,110 |

Both outputs had the established SHA-256, and their solution, spelling, and
retained-result counters matched. Bottom-up computed the full 134,784-state
table rather than the recursive evaluator's 71,899-state reachable closure.

#### First 48-letter calibration

The first production-length workload used `${S6:0:48}` and normalized phase 1
to:

```text
1,019,709 entries
576,151 classes
6,877,673 trie nodes
```

Every run checked for another `dfs-anagrams` process before starting. The
top-1 sweep was:

| depth | value bytes | setup | search | phase 2 | whole program | final DFS nodes |
|---:|---:|---:|---:|---:|---:|---:|
| 11 | 2,488,320 | 0.374s | 7.568s | 7.942s | 10.24s | 137,751,860 |
| 12 | 4,810,752 | 0.516s | 3.478s | 3.994s | 6.33s | 65,463,961 |
| 13 | 9,289,728 | 0.945s | 1.968s | 2.913s | 5.21s | 32,783,668 |
| 14 | 33,177,600 | 3.769s | 0.905s | 4.674s | 6.83s | 14,803,938 |
| 15 | 176,504,832 | 63.982s | 0.163s | 64.145s | 67.66s | 2,077,314 |

All five outputs had SHA-256:

```text
81a58ba9a06885b4ca5582e5a1019cb5371f4a260349bf9a44519649301c7441
```

The result is not merely another case where the deepest table loses. The
`d=14` to `d=15` step multiplies the value payload by 5.32 and measured setup
by 16.98, while saving only 0.74 seconds of top-1 search. Selecting `d=13`
instead of `d=15` reduced whole-program wall time by 12.99x.

At the winning `d=13`, the recursive evaluator measured 5.876 seconds of
setup and 1.939 seconds of search, or 7.814 seconds for phase 2 and 10.10
seconds for the whole program. The bottom-up values and downstream work were
identical, but phase 2 fell to 2.913 seconds and the whole program to 5.21
seconds. The production evaluator therefore improves the actual 48-letter
metric, not only isolated preprocessing.

The top-10 sweep moved the best tested depth:

| depth | setup | search | phase 2 | final DFS nodes |
|---:|---:|---:|---:|---:|
| 12 | 0.503s | 124.147s | 124.650s | 2,340,351,527 |
| 13 | 0.888s | 70.477s | 71.365s | 1,319,523,027 |
| 14 | 3.770s | 31.499s | 35.269s | 569,663,868 |

All three outputs had SHA-256:

```text
df9f9127b9ac1ca7fa9861217ea3637ae4e2876f942288e74328bf193384eddf
```

There is no need to spend another full run on `d=15` for this target: its
63.982-second setup from the target-independent top-1 build already exceeds
the entire measured `d=14` phase 2, even before adding search.

These measurements provide the first direct long-input selector presets:

```text
48-letter S6 prefix, top-1:  start calibration at d=13
48-letter S6 prefix, top-10: start calibration at d=14
```

They are calibration points, not universal constants. Input multiplicities,
action quotient size, score floor, and result target remain selector inputs.
They do show that a target-aware neighboring-depth sweep can be worth an
order of magnitude more wall time than choosing the largest table allowed by
memory.

They also reorder the next optimization priorities:

1. implement a conservative target-aware depth policy before another complex
   action index; the top-1 `d=13` setup is already below one second, so even a
   perfect setup-only optimization can save less than the measured depth
   choice;
2. calibrate top-100 and top-1,000 separately, pruning a candidate as soon as
   its measured or estimated setup exceeds the best complete neighboring
   phase-2 time;
3. retain the support sidecar experiment for deeper, setup-heavy targets,
   where its low-teens gain can still be material;
4. account for the 32-bit layer-key lists in deep memory preflight rather than
   reporting only the value payload; and
5. add persistence after selector calibration, because it helps repeated
   bags but cannot repair a poor first-run depth.

The top-10 sweep demonstrates a useful safe rejection rule. Projection setup
does not depend on `-n`, so the completed top-1 setup at one depth is a lower
bound on every target at that same depth. Once that lower bound exceeds the
best shallower setup-plus-search result by a margin larger than timing noise,
the deeper target run can be skipped without knowing its final DFS node
count. This rejected `d=15` for top-10 and avoided an unnecessary
minute-scale run.

#### Exact static work estimator

Bottom-up work can be counted from the projected actions without evaluating
the table.

For an action in forced exact bucket `r`, let:

```text
m_i = maximum stored count for exact rank i
a_i = the action's exact requirement at rank i
l_i = 1 at r and 0 at later ranks
```

Ranks before `r` are fixed at zero. The root exact digit uses one less than
its input maximum because the root plane is omitted. The number of stored
exact bags in which the action fits is:

```text
product over i >= r of:
    max(0, m_i - max(l_i, a_i) + 1)
```

Using only `a_i = 1` for symbols in the action support gives the
support-fitting count. Using `a_i = 0` gives the unfiltered bucket-scan count.
If the action consumes `w` wildcard letters and the wildcard maximum is `W`,
each multiplicity-fitting exact bag produces:

```text
W - w + 1
```

fitting action/state transitions. The separately evaluated root and the
wildcard-only base vector are small special cases. Summing these quantities
over the action list costs `O(projected actions * exact depth)`.

A temporary implementation produced:

| depth | exact bags | unfiltered scans | support-submask probes | support-fitting scans | multiplicity-fitting pairs | fitting action/state transitions |
|---:|---:|---:|---:|---:|---:|---:|
| 10 | 1,728 | 4,884,280 | 45,563 | 1,051,708 | 792,103 | 15,846,204 |
| 11 | 6,912 | 37,132,572 | 318,938 | 7,831,716 | 5,513,034 | 98,401,067 |
| 12 | 13,824 | 107,003,826 | 956,813 | 18,304,968 | 13,298,748 | 232,673,318 |
| 13 | 41,472 | 496,224,427 | 4,784,063 | 78,217,410 | 54,883,964 | 884,155,653 |
| 14 | 82,944 | 1,368,221,595 | 14,352,188 | 175,444,911 | 127,691,196 | 1,999,879,676 |
| 15 | 414,720 | 11,154,835,445 | 129,169,688 | 1,430,801,732 | 1,003,116,929 | 12,580,372,385 |
| 16 | 1,244,160 | 45,857,045,040 | 645,848,438 | 5,185,121,768 | 3,454,991,684 | 39,415,581,368 |

The `d=10`, `d=14`, and `d=15` unfiltered-scan and transition totals match
the earlier prototype counters exactly. The estimator source was removed
after collecting the diagnostics.

Dense support-submask enumeration is also statically countable. For maximum
counts `m_i`, its probe count for forced bucket `r` is:

```text
m_r * product over i > r of (1 + 2*m_i)
```

summed over buckets, plus the wildcard-only mask. At `d=15`, this is 129.17
million probes for 414,720 exact bags, versus the 737.77 million probes made
by the support-grouped top-down prototype. Wildcard-vector batching reduces
this indexing overhead 5.7x because all wildcard counts reuse one submask
enumeration.

For a simple operation proxy, the plain vector kernel performs:

```text
unfiltered scans + fitting action/state transitions
```

Support grouping changes that to:

```text
submask probes + support-fitting scans
    + fitting action/state transitions
```

At `d=14`, the proxy falls from 3.368 billion to 2.190 billion events, a
35.0% reduction. At `d=15`, it falls from 23.735 billion to 14.140 billion,
a 40.4% reduction. Multiplicity indexing could reduce the `d=15`
support-fitting scans by another 427.7 million, but that is only 3.0% of the
support-grouped combined proxy. Do not add a multiplicity-group index before
profiles show that its extra grouping and traversal are cheaper.

#### Support-grouped vector measurement

A temporary non-replicating layout:

1. compressed the selected exact ranks into `d` support bits;
2. stored each exact action once as a 32-bit ID in its exact-support group;
3. kept a dense `2^d + 1` group-start array; and
4. enumerated support submasks containing the forced exact bit once per exact
   bag.

Wildcard-only actions retained their existing increasing-wildcard evaluation.
Every grouped run used the same binary as its plain-vector baseline and
checked for another `dfs-anagrams` process before each half.

On the 38-letter prefix of `S6`:

| depth | layout | action scans | fitting action/state transitions | setup |
|---:|---|---:|---:|---:|
| 14 | plain vector | 2,135,344,414 | 2,778,025,512 | 1.503494s |
| 14 | support groups | 307,188,075 | 2,778,025,512 | 1.311030s |
| 15 | plain vector | 9,061,939,238 | 8,954,967,914 | 4.832537–4.938820s |
| 15 | support groups | 1,151,088,647 | 8,954,967,914 | 4.230691–4.384408s |

The three `d=15` pairs averaged 4.881204 seconds for the plain layout and
4.301826 seconds for support groups, an 11.9% improvement. The single `d=14`
pair improved 12.8%. Every pair produced byte-identical top-1 output and
identical state, fitting-transition, successful-transition, final-DFS,
solution, and spelling counters.

The action-scan totals match the static support-fitting estimates exactly.
The group traversal additionally made 25,833,938 submask probes at `d=14` and
129,169,688 at `d=15`. Including those probes, the combined proxy predicted
36.7% and 43.2% reductions respectively, substantially more than the measured
setup gains.

This does not invalidate the exact estimator; it shows that its event types
have different costs. Wildcard updates perform child loads, score arithmetic,
max updates, and rounding-error tracking, while a rejected plain-layout scan
often performs only a support-mask test. Group enumeration and indirect
action-ID loads also add work not represented by the simple sum.

The refined production recommendation is:

- implement and validate the plain vector kernel first;
- split support masks from cold action data as the first layout optimization;
- add non-replicating support ranges only as a contained follow-up A/B;
- expect roughly a low-teens setup gain from the measured grouped layout, not
  the raw 7–8x scan reduction; and
- keep multiplicity grouping deferred until a profile shows it can repay its
  additional indexing and locality costs.

#### Thread scaling and layer scheduling

A later reconstruction of the plain-action vector kernel measured
preprocessing-thread scaling directly. Every run checked immediately
beforehand for another `dfs-anagrams` process and produced the established
top-1 output hash:

```text
908e08189cf9f5d0d65340bf39235c0cfde87f9922db357bbba2fbb63129ef74
```

The `d=14` sweep was:

| threads | setup | speedup | efficiency |
|---:|---:|---:|---:|
| 1 | 10.130470s | 1.00x | 100% |
| 2 | 5.199011s | 1.95x | 97% |
| 4 | 2.699166s | 3.75x | 94% |
| 8 | 1.678793s | 6.03x | 75% |
| 10 | 1.561114s | 6.49x | 65% |
| 20 | 1.138171s | 8.90x | 45% |

This is materially better scaling than the earlier recursive thread sweep,
which reached only 5.64x at 20 threads. The vector kernel has no shared
unseen/computing arbitration or recursive wait loops. Its dependency barriers
occur only between exact-total layers, while exact bags within a layer are
dynamically scheduled.

The larger `d=15` sweep retained useful scaling through 20 threads:

| threads | setup |
|---:|---:|
| 4 | 17.902873s |
| 8 | 10.901029s |
| 10 | 9.680837s |
| 20 | 6.987721s |

The 20-thread run was 2.56x faster than the 4-thread run and 1.39x faster than
the 10-thread run. Twenty threads should remain the default when minimum
latency matters, but the 10-thread result is a reasonable resource-saving
mode: it releases half the hardware threads for a 2.69-second setup penalty
on this workload.

The initial reconstruction created and joined workers at every exact-total
layer. A guarded persistent-worker prototype instead created workers once and
used two barriers per layer. At `d=14`, three paired runs had medians of
1.224217 seconds with recreated workers and 1.215471 seconds with persistent
workers, a difference below the timing noise. At `d=15`, two-pair means were
6.699634 and 6.718968 seconds respectively. Output and all deterministic
counters matched.

Persistent workers are therefore not a prerequisite for production. The
number of layers is small enough that action and wildcard-update work
dominates thread lifecycle. Prefer the simplest correct layer scheduler
during initial bring-up; introduce a reusable pool only if it fits an existing
project-wide abstraction or later profiles show a regression on smaller
workloads.

#### Plain versus atomic table storage

The bottom-up dependency order makes state publication one-way:

- one worker owns each exact bag and its contiguous wildcard vector;
- all child exact bags are in completed earlier layers; and
- workers only read, never update, another bag in the current layer.

The atomic unseen/computing sentinels and acquire/release operations required
by recursive top-down evaluation are therefore unnecessary for bottom-up
evaluation.

A temporary A/B added a separate plain `float` vector for bottom-up loads and
stores. To keep the existing search path unchanged, it then copied the entire
plain vector into the atomic table before evaluating the omitted root. Thus
the measured plain time includes both an extra table allocation and a full
post-computation copy:

| depth | atomic table | plain table plus atomic copy | improvement |
|---:|---:|---:|---:|
| 14 | 1.325192s median | 1.006068s median | 24.1% |
| 15 | 7.590834s mean | 5.570218s mean | 26.6% |

There were three paired `d=14` runs and two paired `d=15` runs. Every run
produced the established top-1 hash and identical state, candidate, fitting,
successful-transition, final-DFS, solution, and spelling counters.

The gain is large enough to change the production recommendation. Select
table representation together with the evaluator:

1. allocate atomic words only for top-down construction, where workers race
   to claim recursively discovered states;
2. allocate plain floats for bottom-up construction and keep them plain
   during the subsequent read-only DFS;
3. share rounding and bounds-access helpers at the semantic level rather than
   forcing both evaluators through the same atomic storage type; and
4. do not copy from a plain table into an atomic table in production.

The prototype's double allocation temporarily exceeded the nominal score
cache budget. A production tagged storage choice uses only one four-byte word
per state, so it preserves the existing table-byte accounting.

#### Reduced-precision value storage

The plain-table result raises a separate question: whether each completed
bound needs all 24 fraction bits of IEEE binary32. A temporary prototype kept
the physical 32-bit atomic array but rounded every published value upward to
one of two emulated 16-bit formats:

- IEEE binary16: 5 exponent bits and 10 stored fraction bits; or
- bfloat16: 8 exponent bits and 7 stored fraction bits.

Quantization happened at every recurrence state, so the measurements include
error accumulated through child values rather than merely rounding the final
DFS lookup. The conversion first rounded to the target format and advanced
one target-format value toward positive infinity when necessary. By induction,
each stored child remained at least its exact recurrence value, so the
projected bound remained admissible.

Guarded sweeps on the 28-letter workload produced:

| target | depth | float nodes | binary16 nodes | binary16 increase | bfloat16 nodes | bfloat16 increase |
|---:|---:|---:|---:|---:|---:|---:|
| top-100 | 7 | 22,561,657 | 22,766,801 | 0.9% | 24,437,933 | 8.3% |
| top-100 | 12 | 1,870,903 | 1,910,720 | 2.1% | 2,135,244 | 14.1% |
| top-100 | 14 | 463,138 | 476,695 | 2.9% | 536,191 | 15.8% |
| top-1,000 | 7 | 98,867,939 | 99,494,828 | 0.6% | 103,122,442 | 4.3% |
| top-1,000 | 12 | 11,331,110 | 11,472,500 | 1.2% | 12,251,197 | 8.1% |
| top-1,000 | 14 | 3,086,650 | 3,150,224 | 2.1% | 3,404,276 | 10.3% |

At top-1, binary16 left the `d=12` and `d=14` node counts unchanged. It
increased the shallow `d=7` count from 57,192 to 59,992, or 4.9%, where
search still took only milliseconds. On the unrelated 29-letter `d=13`,
top-1,000 workload it increased nodes from 47,935,597 to 48,333,675, or
0.8%.

Every float, binary16, and bfloat16 run reproduced the established
per-target output hash. At a fixed depth the projected state, candidate,
fitting, and successful-transition counts were unchanged; only the upward
stored values and downstream pruning differed. The temporary source was
removed after measurement.

Binary16 is clearly the stronger two-byte candidate. Its three additional
fraction bits cost far less pruning than bfloat16, while the completion scores
do not need bfloat16's float-sized exponent range. Overflow can remain
correctness-safe in production: positive overflow becomes positive infinity,
and a negative value below the finite binary16 range can be rounded upward to
the largest-magnitude finite negative value. Either case weakens rather than
invalidates the bound. Preserve negative infinity as the exact dead-state
encoding.

The storage consequence is material on the 40-letter workload:

| depth | states | float payload | binary16 payload |
|---:|---:|---:|---:|
| 15 | 7,050,240 | 26.89 MiB | 13.45 MiB |
| 16 | 18,662,400 | 71.19 MiB | 35.60 MiB |
| 17 | 59,719,680 | 227.81 MiB | 113.91 MiB |
| 18 | 179,159,040 | 683.44 MiB | 341.72 MiB |
| 19 | 418,037,760 | 1,594.69 MiB | 797.34 MiB |

The `d=18` and `d=19` rows are exact preflight size calculations, not
completed table builds.

`d=19` and all-exact `d=20` have the same state count on this input because
the only remaining wildcard symbol at `d=19` is determined by total remaining
length. The action quotient likewise has no collisions there.

Under a hard 64 MiB budget, packing would change the available frontier from
`d=15` to `d=16`. That is not the current constraint. Float already reaches
`d=18` within a roughly 1 GiB allowance and `d=19` within 2 GiB, before
accounting for action and other process storage. The question is therefore
whether half-size loads reduce total wall time at the same depth, or whether a
deeper binary16 configuration beats the best float configuration. Merely
making another depth fit is not a benefit.

The prototype deliberately did not claim a packed-table timing. Values were
quantized but stored as 32-bit floats, so it measured pruning loss without
measuring either the reduced memory traffic or half-to-float conversion on
every child and DFS lookup. The test machine reports F16C but not AVX-512
FP16; conversion is available, but its cost and the attainable vectorization
still require measurement. Implement packed binary16 only after the correct
plain-float vector kernel exists:

1. keep plain float as the correctness and performance baseline;
2. compare float and packed `uint16_t` at the same depth first, so capacity
   and depth do not confound the memory-traffic result;
3. use a plain `uint16_t` value array only for complete bottom-up tables;
4. round upward explicitly rather than relying on the ambient conversion
   direction;
5. decode at the bound-access boundary and measure scalar and vectorized
   conversion on supported CPUs;
6. measure setup plus final DFS search, including the weaker pruning already
   seen after quantization;
7. select depth and precision jointly using payload bytes, estimated vector
   work, and calibrated final-search cost; and
8. fall back to float when the platform representation or conversion path is
   unsupported.

If same-depth binary16 does not reduce total wall time, stop there. Do not
productionize it solely to preserve a 64 MiB default. If it does win because
the vector kernel is bandwidth-bound, then packing remains valuable even with
ample RAM and should be evaluated at neighboring depths.

Top-down construction should remain on 32-bit atomic floats initially.
Sixteen-bit atomic availability and sentinel arbitration are platform
questions that buy less than the plain bottom-up path. Persistence must record
the value representation in its format version and checksum the encoded
payload.

#### Selector implications

The static proxy tracked large-vector setup time unusually closely:

| depth | plain-vector proxy | measured setup | proxy rate |
|---:|---:|---:|---:|
| 14 | 3.368 billion | 1.394s | 2.42 billion/s |
| 15 | 23.735 billion | 9.700s | 2.45 billion/s |

This rate is machine- and implementation-specific, but the 1.3% agreement
shows that the proxy is much more informative than table bytes, state count,
or action count alone. The tiny `d=10` case remains dominated by fixed costs
and should not be calibrated with the large-work rate.

These rates describe the first atomic-table prototype. The later plain-table
reconstruction processed 3.368 billion proxy events in 1.006 seconds at
`d=14` and 23.735 billion in 5.570 seconds at `d=15`, or 3.35 and 4.26
billion events per second. Plain storage both raises throughput and weakens
the earlier cross-depth rate agreement. The estimator remains exact for work
counts and useful for rejecting large differences, but its event-to-time
calibration must be specific to the selected storage and evaluator.

Use the estimator in two places:

1. report projected bottom-up scans, fitting transitions, and the combined
   work proxy in diagnostics;
2. choose between the top-down and vector evaluators using projected work,
   wildcard span, and table coverage rather than state count alone.

It can also reject a clearly over-expensive depth, but the storage result
narrowed the former `d=15`/`d=16` conclusion enough to require measurement.

The larger memory allowance does not change the selector objective. A guarded
current-binary top-1 comparison on the 34-letter proxy forced two float
depths with `-C 2048`:

| depth | projection | value bytes | setup | search | total | final DFS nodes |
|---:|---|---:|---:|---:|---:|---:|
| 15 | 15 exact, 8 wildcard letters | 33,592,320 | 59.931341s | 0.004344s | 59.935685s | 37,108 |
| 17 | all 17 symbols exact | 78,382,080 | 156.052395s | 0.003208s | 156.055603s | 29,843 |

Both produced:

```text
9.824e-22 foundation belize yosef would have been
```

The larger table used 2.33x the value memory, reduced DFS nodes by 19.6%, and
saved 1.136 milliseconds of search, but increased setup by 96.12 seconds. Its
total was 2.60x worse. `d=16` has the same state and action space as `d=17`
on this input: its sole wildcard symbol is exactly recoverable from remaining
total length, so that redundant run was stopped after preflight.

The current largest-fitting automatic policy would choose `d=15` under
64 MiB and `d=17` under 2 GiB on this input. Thus simply raising the configured
allowance would select the 2.60x slower result.

This comparison uses the current recursive evaluator, whose setup is the
known bottleneck, and top-1 strongly favors shallow setup. It must not be
extrapolated into a fixed `d=15` recommendation for the bottom-up evaluator
or other result targets. It does establish the policy: raising the cache
allowance from 64 MiB to 1–2 GiB expands the candidates to measure; it does
not instruct the selector to choose the deepest float table.

A guarded same-binary, top-1,000 comparison with 20 threads and a 128 MiB
budget produced:

| depth | value bytes | setup | search | total | final DFS nodes |
|---:|---:|---:|---:|---:|---:|
| 15 | 28,200,960 | 6.068348s | 28.456284s | 34.524632s | 342,949,072 |
| 16 | 74,649,600 | 19.408372s | 14.597693s | 34.006065s | 183,381,681 |

Both runs produced the established top-1,000 SHA-256:

```text
5cf8a34f71e78a47270897fb32161526807fb2ff906580708986f8e42f0ec601
```

They also expanded 15,900 spellings and retained 1,000 results. The deeper
bound almost halved final DFS nodes and search time, but its setup increased
3.20x. The net `d=16` win was only 0.519 seconds, or 1.5%, which is smaller
than the observed run-to-run variation. Its value table is also 2.65x larger
and does not fit the 32 MiB budget used by the earlier sweeps.

Treat `d=15` and `d=16` as effectively tied on this workload. Prefer `d=15`
as the provisional default because it is less exposed to setup-rate
variation. The 2.65x memory difference is only a tie-breaker under the
available 1–2 GiB budget, not the deciding objective. Select `d=16` when
representative empirical calibration consistently shows a latency win. This
reinforces, rather than weakens, the recommendation not to select the deepest
memory-fitting projection automatically.

The estimator does not predict how much a deeper bound reduces final DFS
search. It therefore cannot choose projection depth by itself. A practical
selector should first reject depths whose static setup proxy is clearly
uncompetitive, then compare a small neighborhood of plausible depths using
representative search calibration. That neighborhood need not include the
deepest memory-fitting table. Retain an explicit depth override until a
reliable search-benefit model exists.

#### Cross-workload depth/evaluator result

These runs used the workload command above with `${S6:0:28}` or
`${S6:0:38}`, `-n 1000`, and `-C 32`. Guarded current-binary runs on the
28-letter prefix produced:

| depth | setup | search | total | final DFS nodes |
|---:|---:|---:|---:|---:|
| 10 | 0.094365s | 1.851214s | 1.945579s | 28,809,173 |
| 11 | 0.135066s | 1.121715s | 1.256781s | 17,854,432 |
| 12 | 0.199318s | 0.732972s | 0.932290s | 11,331,110 |
| 13 | 0.533963s | 0.394865s | 0.928828s | 6,059,101 |
| 14 | 1.595900s | 0.203737s | 1.799637s | 3,086,650 |

All five outputs had SHA-256:

```text
de32cca977192c1ab65b56329c9a7f8f97e25441fbea1e39c049b7ebf8c33ee6
```

`d=12` and `d=13` are effectively tied, while the deepest tested `d=14`
loses decisively. The bound keeps improving through `d=14`, but the last
setup increase exceeds the remaining search saving.

The 38-letter prefix makes evaluator selection change the best depth:

| evaluator | depth | setup | search | total | final DFS nodes |
|---|---:|---:|---:|---:|---:|
| top-down | 14 | 7.631454s | 15.681364s | 23.312818s | 203,485,601 |
| top-down | 15 | 29.053492s | 7.512081s | 36.565573s | 105,067,790 |
| bottom-up vector | 14 | 1.575290s | 15.854029s | 17.429319s | 203,485,601 |
| bottom-up vector | 15 | 4.960373s | 7.461765s | 12.422138s | 105,067,790 |

All four outputs had SHA-256:

```text
398abaeaeb5245dfe071f1f11d933742591230b685a6be2ac724abc35a4ffec4
```

The static estimator predicted the bottom-up action-scan and fitting
transition counts exactly at both depths. More importantly, bottom-up changes
the correct depth choice from `d=14` to `d=15`. A selector that chooses depth
using top-down setup cost and then independently chooses an evaluator can
therefore be wrong. It must score `(depth, evaluator)` pairs jointly.

These sweeps still do not supply a static model of final DFS nodes. Until one
exists, keep the depth override and use neighboring-depth empirical
calibration for representative workloads. The estimator is sufficient to
discard expensive evaluator/depth pairs, but not to rank pairs whose setup
and expected search savings are close.

#### Top-N sensitivity

Projection setup is independent of the result target, but the search work
that repays it is not. To isolate that effect, a guarded current-production
top-down sweep used the 28-letter prefix, 20 preprocessing threads, a 32 MiB
budget, and varied only projection depth and `-n`.

Each cell is setup plus search time from one guarded same-binary sweep:

| depth | top-1 | top-10 | top-100 | top-1,000 |
|---:|---:|---:|---:|---:|
| 7 | 0.0338s | 0.2261s | — | — |
| 8 | 0.0391s | 0.1449s | 0.9224s | 4.7936s |
| 9 | 0.0541s | 0.1105s | 0.5905s | 2.8601s |
| 10 | 0.0708s | 0.1193s | 0.4217s | 1.9752s |
| 11 | 0.1499s | 0.1272s | 0.3388s | 1.2737s |
| 12 | 0.1783s | 0.2106s | 0.2851s | 0.9142s |
| 13 | 0.6272s | 0.5188s | 0.5916s | 0.9320s |
| 14 | 1.6377s | 1.6663s | 1.6780s | 1.8373s |

Depths `d=0` through `d=6` were also tested for top-1 and top-10; none beat
the displayed `d=7` and `d=9` choices.

All tested depths produced byte-identical retained output for a given `-n`.
The deterministic preprocessing counters at each depth were also identical
across result targets, as expected.

Three-run medians around the closest decisions were:

| target | shallower depth | median total | deeper depth | median total |
|---:|---:|---:|---:|---:|
| 1 | 7 | 0.0345s | 8 | 0.0431s |
| 10 | 9 | 0.1105s | 10 | 0.1193s |
| 100 | 11 | 0.3600s | 12 | 0.2946s |
| 1,000 | 12 | 0.9185s | 13 | 0.9320s |

The top-10 and top-1,000 pairs are close enough to treat as ties rather than
stable machine-wide wins. The trend is nevertheless clear: as more results
are retained, final search grows and repays a deeper projection. Conversely,
using the deepest tested `d=14` for top-1 took about 47 times as long as the
best shallow choice even though it visited fewer DFS nodes.

Depth selection must therefore include at least:

```text
(input/projection statistics, evaluator and storage, result target,
 score-floor policy)
```

Do not calibrate a single preferred depth from top-1,000 timings and apply it
to top-1 or top-10 queries. Until a reliable final-search model exists, keep
the explicit depth override and maintain small empirical presets for the
important result-target classes. When neighboring choices are within timing
noise, prefer the shallower projection for lower setup variance and memory
use.

This does not complicate persistent-table identity. The completed projected
bound depends on the index, bag, scoring model, and projection, but not on
`-n` or the current sink floor. One persisted deeper table can therefore be
reused by later queries with different result targets even though a first-run
selector might choose different depths for them.

#### Exact-symbol-set sensitivity

All earlier depth sweeps define depth `d` as the first `d` present symbols in
corpus-rarity order. That is a useful default, but it couples two different
properties:

- rarer exact symbols tend to expose infeasibility and tighten the final DFS
  bound earlier; and
- an exact symbol with input multiplicity `m` multiplies the exact state
  product by `m + 1` while removing `m` letters from the wildcard span.

The latter can make the next-rarest symbol expensive. On the 40-letter
workload, the usual `d=15` step makes `o:4` exact. A temporary same-binary
override instead kept the first 14 symbols exact and selected one of the six
remaining symbols as the 15th. Every run used top-1,000, 20 preprocessing
threads, and a 64 MiB budget, and checked for another `dfs-anagrams` process
before starting:

| 15th exact symbol | projected states | actions | setup | search | total | final DFS nodes |
|---|---:|---:|---:|---:|---:|---:|
| `o:4` | 7,050,240 | 151,440 | 40.940s | 27.662s | 68.602s | 342,949,072 |
| `l:2` | 4,727,808 | 135,837 | 21.658s | 37.291s | 58.949s | 456,753,779 |
| `i:3` | 5,971,968 | 148,941 | 31.543s | 36.686s | 68.229s | 437,025,148 |
| `n:3` | 5,971,968 | 140,795 | 29.557s | 40.548s | 70.105s | 475,529,743 |
| `a:2` | 4,727,808 | 145,236 | 23.544s | 37.973s | 61.517s | 440,687,010 |
| `e:6` | 8,709,120 | 159,429 | 57.247s | 30.416s | 87.663s | 366,441,679 |

All six outputs had the established SHA-256:

```text
5cf8a34f71e78a47270897fb32161526807fb2ff906580708986f8e42f0ec601
```

They also expanded 15,900 spellings and retained 1,000 results. Equal
multiplicity did not imply equal behavior: `l` and `a` had identical table
sizes, but `l` built fewer actions, required less setup, and visited 3.5%
more DFS nodes while still finishing 2.57 seconds sooner. Likewise, `i` and
`n` had identical table sizes but differed by 1.88 seconds of setup and 10.5%
of final DFS nodes. Table bytes are useful for rejecting candidates, not for
ranking them.

For the current recursive top-down evaluator, exact `l` was the clear winner
in this boundary family. It gave up 10.9 seconds of search relative to exact
`o`, but saved 19.3 seconds of setup, reducing the total by 9.65 seconds or
14.1%.

The smaller 28-letter workload did not reproduce that reversal. Keeping its
first 12 exact symbols and varying the 13th produced:

| 13th exact symbol | setup | search | total | final DFS nodes |
|---|---:|---:|---:|---:|
| `o:2` | 0.536s | 0.400s | 0.937s | 6,059,101 |
| `i:2` | 0.566s | 0.386s | 0.953s | 6,021,894 |
| `n:3` | 0.612s | 0.443s | 1.055s | 6,744,581 |
| `e:5` | 0.841s | 0.349s | 1.191s | 5,219,033 |

Exact `o` and `i` were effectively tied, with the normal rarest-prefix choice
slightly ahead. All four runs reproduced the established 28-letter
top-1,000 hash. The different winner regions across two input lengths argue
against replacing rarity order with a fixed multiplicity rule.

The closed-form bottom-up estimator applies unchanged to an arbitrary exact
set. The 40-letter candidates produced:

| 15th exact symbol | vector action scans | wildcard updates | combined proxy |
|---|---:|---:|---:|
| `o:4` | 11.155B | 12.580B | 23.735B |
| `l:2` | 6.147B | 6.923B | 13.070B |
| `i:3` | 8.893B | 9.993B | 18.886B |
| `n:3` | 8.363B | 9.366B | 17.728B |
| `a:2` | 6.553B | 7.240B | 13.793B |
| `e:6` | 16.401B | 16.445B | 32.846B |

This is an important evaluator interaction. Exact `l` has only 55.1% of the
usual exact-`o` vector proxy, but its measured 37.291-second final search
already exceeds the established plain-bottom-up exact-`o` total of 34.525
seconds. The table evaluator does not change bound values or final DFS work.
Although a direct bottom-up exact-`l` timing was not available after the
temporary kernel was removed, even zero `l` setup would not beat that
measured `o` total. Its 456.8 million final nodes are also 33.2% above `o`.
The rarest prefix therefore remains the stronger candidate for the production
bottom-up evaluator on this workload.

An arbitrary exact subset is semantically valid: projected actions still
consume an exact vector plus a wildcard total, and support bucketing uses the
rarest exact symbol present in each action. The omitted-root-plane
optimization needs more care. Keep the globally rarest present symbol exact
so the first DFS class consumes it, or retain the selected root's full plane
until it has been consumed; otherwise early search nodes can fall outside the
stored effective range and temporarily receive no bound.

Refine the selector model to score:

```text
(exact-symbol set, evaluator and storage, result target, score-floor policy)
```

rather than depth alone. Do not exhaustively enumerate all subsets at query
time: quotient construction itself depends on the set. Start with the rarity
prefix and a small one-swap neighborhood at the boundary, reject candidates
by table bytes and the exact static vector estimator, and use representative
search calibration for the survivors. Keep rarity order as the robust
production default until the bottom-up evaluator exists and cross-workload
calibration demonstrates a repeatable arbitrary-set win. In particular, do
not delay bottom-up bring-up to add a top-down-only `l` special case.

The temporary exact-set override and estimator diagnostics were removed after
measurement.

### 6. Coarse-to-fine projected bounds: measured, not recommended

A shallow quotient projection is cheap:

```text
d=10 quotient setup: about 0.24s
```

It initially appeared useful as a complete fallback while a deeper projection
was built only where its tighter value mattered. Two temporary prototypes
tested the premise more directly: small coarse bounds as independent search
fallbacks, and coarse certificates during deep top-down construction. The
prototype source was removed after measurement.

#### Coarse fallback strength

The simplest fallback tracked only remaining total length. Shadow lookups
during three complete top-1,000 searches found:

| workload and rich projection | final bound queries | length-only prunes | share |
|---|---:|---:|---:|
| 28 letters, `d=12` | 11,331,052 | 7,801,022 | 68.9% |
| 28 letters, `d=14` | 3,086,592 | 1,592,898 | 51.6% |
| unrelated 29 letters, `d=13` | 47,935,349 | 41,127,421 | 85.8% |

Every length-only prune was also made by the rich projection. Taking the
minimum of the two bounds therefore added no pruning on any of these runs.
Length alone can be an inexpensive emergency fallback, but it is not a useful
companion bound.

A stronger experiment built four deterministic coarse projections using
6-bit signatures. The diagnostic accounting reported:

```text
59,392 table bytes + 370,776 per-class delta bytes = 430,168 bytes
```

The four quotient action sets contained 925–935 actions each and required
about 6.66 million candidate scans in total to construct. Shadow lookups
produced:

| workload and rich projection | rich prunes | four-table modular prunes | rich only | modular only |
|---|---:|---:|---:|---:|
| 28 letters, `d=12` | 11,307,201 | 10,943,333 | 369,344 | 5,476 |
| 28 letters, `d=14` | 3,080,422 | 2,835,409 | 245,565 | 552 |
| unrelated 29 letters, `d=13` | 47,897,732 | 47,301,059 | 604,217 | 7,544 |

Thus the small modular family independently pruned 96.6%, 91.9%, and 98.7% of
all bound queries, close to the rich table's 99.8%, 99.8%, and 99.9%. The
nonzero `modular only` column also confirms that the bounds are complementary
rather than merely weaker copies.

This was shadow instrumentation, not a clean end-to-end standalone A/B.
Multiple table lookups and combination with the rich bound have a runtime
cost, and the reported 420 KiB excludes ordinary shared action-preparation
storage. Nevertheless, the pruning-per-byte result is strong enough to retain
as the preferred low-memory experiment. At `d=14`, the reported modular
table-plus-delta storage is 12.8% of the 3.20 MiB rich value table. At `d=12`,
it is already close to the 527 KiB rich table, so the rich projection remains
the obvious choice when it fits.

#### Coarse certificates during deep construction

The next prototype used the coarse bound to certify that a fitting deep action
could not improve the incumbent value at its parent. A validation mode
performed every certificate check but still evaluated the deep child. The
on-demand mode skipped every certified child evaluation and retained the
coarse fallback for an absent deep entry.

Process-guarded 20-thread comparisons were:

| workload | mode | setup | search | total | successful deep transitions | deep states claimed |
|---|---|---:|---:|---:|---:|---:|
| 28 letters, `d=12` | check only | 0.329s | 1.187s | 1.516s | 23,327,436 | 71,899 |
| 28 letters, `d=12` | skip certified | 0.180s | 1.596s | 1.776s | 12,598,401 | 71,231 |
| 28 letters, `d=14` | check only | 2.181s | 0.345s | 2.525s | 213,302,595 | 430,123 |
| 28 letters, `d=14` | skip certified | 1.398s | 3.635s | 5.033s | 144,436,679 | 428,748 |
| 29 letters, `d=13` | check only | 1.925s | 5.534s | 7.459s | 215,610,693 | 239,529 |
| 29 letters, `d=13` | skip certified | 0.963s | 10.741s | 11.704s | 73,253,366 | 234,005 |

The skipped mode removed 46.0%, 32.3%, and 66.0% of successful setup
transitions and saved 45.4%, 35.9%, and 50.0% of setup time. It reduced the
number of claimed deep states by only 0.9%, 0.3%, and 2.3%, however. Most
dependency states were still required through another action; the certificate
mostly removed parallel edges rather than the closure itself.

The final search more than repaid the setup saving. Total phase-2 time
increased 17.2% at `d=12`, 99.3% at `d=14`, and 56.9% on the unrelated
workload. Every pair retained identical output, DFS nodes, solutions,
spellings, and result count. The 28-letter output SHA-256 was:

```text
de32cca977192c1ab65b56329c9a7f8f97e25441fbea1e39c049b7ebf8c33ee6
```

The same `d=14` comparison on one preprocessing thread went in the other
direction:

| mode | setup | search | total |
|---|---:|---:|---:|
| check only | 17.880s | 0.343s | 18.224s |
| skip certified | 5.549s | 10.318s | 15.867s |

Skipping saved 12.33 seconds of serial setup and added 9.97 seconds to search,
for a 12.9% total reduction. This is not a good production niche: the
bottom-up kernel already addresses serial recursive setup without imposing
that search penalty, and the 20-thread production configuration makes the
trade clearly negative. The result does explain the mechanism—the
certificate moves or replaces highly parallel setup work with work on the
serial search path rather than eliminating enough of the deep state graph.

Do not add coarse certificates, on-demand deep refinement, or a second rich
projection to the initial bottom-up implementation. Under the available
1–2 GiB allowance, the four-table modular family is not on the main
optimization path. Revisit it only for a separately supported low-memory mode
or if a complete end-to-end A/B shows that its tiny, cache-resident lookups
reduce wall time despite a rich table fitting. Compare it against
projection-off and the best shallow ordinary projection with all lookup
overhead enabled.

#### Terminal perimeter

Layer diagnostics found that deep dead states are highly concentrated near
the empty bag. On the 28-letter `d=14` run, 11,563 of 11,615 dead states
(99.55%) had at most seven remaining letters. Almost every incoming
dead-child transition targeted the same region. Reverse enumeration through
seven letters found only:

| projection | finite terminal states | share of projected value slots |
|---|---:|---:|
| 28 letters, `d=12` | 5,811 | 4.31% |
| 28 letters, `d=14` | 16,951 | 2.02% |
| unrelated 29 letters, `d=13` | 12,698 | 2.78% |

This narrows the earlier viability-mask idea. If a standalone modular fallback
needs tightening, pair it experimentally with a compact exact terminal
perimeter rather than a viability bit for every projected state. It could
resolve the irregular short remainder where modular collisions matter most
and reject known-dead children before a fallback lookup. It is still a later
bounded-memory experiment: dead children were 9.8% of fitting transitions at
28-letter `d=14`, and only 3.15% on the established 40-letter `d=15`
bottom-up run.

### 7. Persist projected tables for repeated queries

For repeated runs with the same:

```text
index/corpus counts
input bag
minimum word length
restart model
projection
```

a completed projected table could be serialized and memory-mapped. This does
not improve the first run, but it can eliminate repeated minute-scale setup
while changing top-N or output parameters.

#### Raw load/dump prototype

A temporary prototype copied the current projected atomic words to a raw
32-bit file after top-down construction. On load, it read and validated the
exact expected length, copied the words into the score table, and skipped
projected recurrence evaluation. The prototype source was removed after
measurement.

The process-guarded workload used the 38-letter prefix, `d=14`, top-1, 20
preprocessing threads, and a 32 MiB budget:

| mode | phase-2 setup | search | final DFS nodes |
|---|---:|---:|---:|
| compute and dump | 7.896448s | 0.088995s | 1,464,594 |
| warm raw load | 0.134649s | 0.084236s | 1,464,594 |

Both runs produced byte-identical stdout with SHA-256:

```text
61c93f1782641daf8fd0a3a49e477c5fd949224f0943e9910da154aba72b11d5
```

They also found 34 solutions, expanded 11 spellings, and retained one result.
The loaded setup was 58.6x faster than compute plus dump. This comparison
includes action preparation, allocation, the raw read, and copying into the
atomic table; only phase 1 remains outside the reported phase-2 setup.

The raw file held 2,350,080 four-byte words, or 9,400,320 bytes. The recursive
top-down builder had computed 1,151,524 reachable values:

```text
1,198,556 unseen/unreachable sentinels
   13,918 dead-end negative-infinity values
1,137,606 finite completion bounds
```

`gzip -1` reduced this particular file to 3,779,798 bytes in 0.17 seconds.
That compression ratio partly reflects the top-down table's untouched
unreachable half. The recommended bottom-up evaluator fills the entire
effective table, so do not assume it will compress equally well. Compression
also prevents direct read-only mapping; the initial production format should
favor a raw versioned value encoding and startup latency over minimum disk
size.

#### Cross-target reuse

The raw table constructed during the top-1 run was loaded without rebuilding
for a top-1,000 query. It reproduced the previously established result:

```text
stdout SHA-256:
398abaeaeb5245dfe071f1f11d933742591230b685a6be2ac724abc35a4ffec4

203,485,601 DFS nodes
14,453 solutions
14,976 spellings expanded
1,000 retained
```

Loaded phase-2 setup was 0.150475 seconds and search was 15.399488 seconds.
This directly confirms that `-n` is a first-run depth-selection input, not
part of the identity of a completed table at a selected depth. Progress
frequency and preprocessing thread count are likewise not table-value inputs.
Evaluator, action layout, and quotient construction path also need not enter
the identity as long as they implement the same versioned recurrence and
rounding semantics.

#### Production format and validation

Persistence affects pruning correctness: a stale or corrupted value that is
too low can discard valid results. A production file therefore needs more
than the prototype's exact-length check. Use a versioned header containing at
least:

```text
magic, format version, byte order, float/rounding format
index-content fingerprint and scoring-model version
normalized remaining input bag and minimum word length
restart value/model
ordered exact-symbol projection, radices, wildcard span, entry count
root-plane convention and optional root bound
payload length and strong checksum
```

The index fingerprint must cover the corpus counts that determine class
scores; a path, modification time, or entry count is insufficient. The exact
symbol order and radices should be stored explicitly rather than inferred
from a future selector implementation.

Write a new table to a temporary file, finish the header and checksum only
after every value is published, then atomically rename it into the cache.
Reject any identity, version, length, checksum, or completion mismatch and
recompute normally. Do not use a partially written table as an admissible
bound.

Implement float persistence after the plain-float bottom-up evaluator; do not
block it on packed binary16. Float values can remain file-backed and read-only
during DFS, avoiding both the prototype's atomic stores and a second full-size
copy. Make the header extensible enough to add binary16 later if its real
same-depth wall-time A/B wins. Give float and any future binary16 encoding
distinct format versions and rounding identifiers; do not silently convert a
mapped payload at load time. Charge the logical mapped payload against the
selected score-cache allowance even though clean file-backed pages are
reclaimable. Keep a buffered-read fallback where direct mapping is
unavailable.

The initial format should store the complete raw value array. Sparse encoding
is less attractive once bottom-up evaluation fills all entries and would add
an index or reconstruction pass before random DFS lookup. Optional compressed
archival can be added later if disk footprint matters more than startup
latency.

## Lower-priority ideas

### More preprocessing threads

Twenty threads still minimized measured wall time, but supplied only 5.64x
speedup and had 28% parallel efficiency on the thread-sweep workload. Improve
the work count and memory access pattern before investing in more elaborate
root-task scheduling.

### Rounding and `nextafter`

The production `d=15` run made about 8.5 million `nextafter` calls while
processing 5.92 billion successful transitions and 93.06 billion candidate
tests. Invariant score/error arithmetic is now stored in the compact action,
and rounding remains primarily once per computed state. State-level
`nextafter` tuning is not a leading target.

### Plain lazy construction

Earlier exact-bound experiments found that laziness avoided:

```text
4 of 143,693 states
```

on the 25-letter workload and was slightly slower. A requested bound still
requires its dependency closure. Laziness becomes more promising only when a
cheap complete projection is available on every miss.

### GPU implementation

The current recursive, shared-atomic traversal is not a suitable GPU kernel.
GPU evaluation should be reconsidered only after quotienting and a regular
bottom-up wildcard-vector formulation exist.

## Production validation

Focused validation now covers:

1. a synthetic wildcard-only projection with equivalent concrete classes and
   different scores;
2. quotient versus opt-out retained output, state counts, DFS counters, and
   transition reduction;
3. equality against exhaustive output on the existing small projected test;
4. forced `d=0`, an intermediate `d`, and all-exact projection;
5. one and multiple preprocessing threads;
6. the 40-letter `d=10` quotient/opt-out comparison;
7. the 40-letter `d=13`, `d=14`, and `d=15` top-1,000 sweep with identical
   output hashes; and
8. concrete-class, projected-action, candidate-test, fitting-transition, and
   successful-transition diagnostics; and
9. 28-letter and 40-letter boundary exact-symbol-set sweeps with identical
   per-workload output hashes and spelling counts; and
10. coarse-certificate check-only and skip comparisons at two depths and on
    an unrelated input, with identical output and final DFS counters; and
11. float, upward-rounded binary16, and upward-rounded bfloat16 comparisons
    across three depths and two inputs, with identical retained outputs; and
12. a guarded 34-letter, `-C 2048`, top-1 capacity comparison between
    `d=15` and all-exact `d=17`, with identical retained output; and
13. 40-letter `d=18` and `d=19` preflight state and payload-size calculations.

The build and focused Meson smoke suite pass, including the 14-letter index
validation and CLI differential test.

The quotient leaves:

```text
computed projected-state count
final DFS node count
solutions visited
retained spellings
```

unchanged relative to the unquotiented projection. Its successful-transition
count falls because dominated parallel edges have been removed.

A direct top-1,000 opt-out comparison at `d=14` or `d=15` remains possible,
but it would intentionally repeat the earlier minute-scale unquotiented
preprocessing. The fast `d=10` direct comparison plus identical deeper output
hashes and counters provide the current production smoke coverage.

## Conclusion

Optimize for total whole-program wall time on long inputs, not for adherence
to a 64 MiB cache size. Phase-2 setup plus search is the local comparison
metric because phase 1 is invariant within these A/Bs. A 1–2 GiB score-table
allowance is available when it actually lowers latency. Table bytes should
reject configurations that exceed that allowance and break close timing ties;
they should not rank otherwise viable depths.

Projected preprocessing was dominated by evaluating concrete class edges that
the projection had already made equivalent. Quotienting those classes is now:

- exact;
- local to projected-bound construction;
- compatible with the existing flat projected key;
- independently useful at every projection depth; and
- validated in the focused unit and CLI smoke suite.

The compact production action reduces the same-session `d=10` setup from
5.71 seconds to 0.30 seconds. In the completed top-1,000 sweep, `d=15` was the
best tested end-to-end depth at 72.87 seconds.

Bottom-up wildcard-vector evaluation is now implemented with direct plain
float ownership. The production 40-letter `d=15`, top-1,000 run completed
phase 2 in 32.04 seconds and reproduced the established output hash and every
deterministic counter. The recursive evaluator remains available through an
environment opt-out for same-binary validation.

The earlier implementation study found that plain float storage reduced the
vector kernel by 24.1% at `d=14` and 26.6% at `d=15`, despite charging an
extra copy into the old atomic representation. The production evaluator now
avoids that copy. Persistent cross-layer workers remain unnecessary: paired
measurements found no repeatable worker-pool gain.
Bottom-up preprocessing also scaled 8.90x from one to 20 threads at `d=14`,
substantially better than the recursive kernel.

The first direct 48-letter calibration makes selector work the highest-value
next optimization. At top-1, `d=13` completed the whole program in 5.21
seconds, versus 67.66 seconds at `d=15`; this is a 12.99x wall-time reduction
from choosing the right production depth. At `d=13`, bottom-up also halved
whole-program time relative to the recursive evaluator, 10.10 seconds to
5.21 seconds. At top-10 the best tested depth moved to `d=14`, whose 35.27
second phase 2 beat 71.36 seconds at `d=13`; `d=15` was excluded because its
63.98-second setup alone was already slower.

A later support-mask sidecar reduced the current top-down setup by 8.2% at
`d=14` and 15.1% at `d=15` without changing any existing counter. This is a
simpler first layout experiment than support grouping: split the masks from
the cold action fields, validate it in the vector kernel, and add grouped
ranges only if they still win a same-binary A/B.

With plain storage, a same-binary top-1,000 comparison put `d=15` at 34.52
seconds total and `d=16` at 34.01 seconds. That 1.5% difference is noise-sized,
so the selector should treat them as tied and provisionally default to
`d=15` for lower setup variance. Its lower memory is a secondary tie-breaker,
not the objective.

The new 34-letter capacity comparison is more decisive for the current
top-down evaluator: increasing the float table from 32.04 MiB at `d=15` to
74.75 MiB at all-exact `d=17` changed total top-1 phase 2 from 59.94 seconds
to 156.06 seconds. Search fell by only 1.1 milliseconds. A larger allowance
expands the selector's candidate set; it does not justify choosing its largest
member.

Binary16 remains a possible throughput optimization, not a capacity
requirement. Upward quantization at every projected state preserved all
tested outputs and increased final nodes by only 0.6–2.9% in the useful
28-letter top-100/top-1,000 cases and 0.8% on an unrelated workload. Bfloat16
increased nodes by 4.3–15.8% and is not recommended. The 40-letter float
tables through `d=18` fit within about 1 GiB and `d=19` fits within 2 GiB.
After the plain-float vector baseline, compare actual packed binary16 with
float at the same depth. Keep it only if lower memory traffic outweighs
conversion and weaker pruning in total wall time; do not implement it merely
to make `d=16` fit 64 MiB.

The selector must also account for the retained-result target. On the
28-letter production sweep, the best region moved from `d=7` at top-1 through
`d=9`/`d=10` at top-10 to `d=12` at top-100 and `d=12`/`d=13` at top-1,000.
At top-1, the deepest tested `d=14` was about 47x slower end to end than
`d=7`. Calibrate depth presets separately by result-target class; when
neighboring totals are noise-sized, prefer the shallower table.

Exact-symbol selection is evaluator-specific as well. With the current
top-down kernel, replacing the 40-letter `d=15` boundary symbol `o:4` with
`l:2` reduced top-1,000 setup plus search by 14.1%. The same swap leaves 33.2%
more final DFS nodes, however, and its search alone is slower than the
established plain-bottom-up `o` total. Retain the rarest prefix for initial
bottom-up production, then evaluate a small boundary-swap neighborhood with
the static vector estimator and representative search calibration. Do not
select exact symbols from multiplicity or table size alone.

Subdividing by wildcard length is not recommended: it reduced the remaining
`d=15` scans by 24.75% but increased setup to 26.37 seconds, with the same
direction at `d=14`. Non-replicating support grouping is a stronger companion
to the bottom-up kernel. On the 38-letter workload it reduced vector action
scans by 6.95–7.87x and setup by about 12%, while preserving output and all
deterministic counters. Retain the plain layout as the initial production and
validation baseline and measure the support sidecar next. The larger budget
also makes a complete presence-list cache a valid contained competitor to
grouped ranges. The observed top-down masks required 25.8–66.0 MiB, but a
bottom-up arena can be larger and must be counted exactly first. Include its
build time and retain it only for a measured wall-time win within the working
allowance.

Selector tuning should choose both projection depth and evaluator using
the exact static bottom-up work estimate where applicable, then validate
against setup plus final-search time rather than cache capacity alone. The
38-letter sweep shows why the choice must be joint: top-down selected `d=14`,
while bottom-up selected `d=15` and reduced the best measured total from 23.31
seconds to 12.42 seconds. The estimator predicts exact work counts, not
downstream pruning benefit or equal per-event costs, so depth selection still
needs neighboring-depth comparison.

Coarse-to-fine construction should not be in that initial selector. Coarse
certificates cut successful deep transitions by 32–66%, but reduced claimed
states by only 0.3–2.3% and made the 20-thread end-to-end runs 17–99% slower.
Four 6-bit modular tables remain relevant only to a separately supported
low-memory mode, or if a future complete A/B finds a wall-time win from their
cache-resident lookup. Their 420 KiB footprint is not itself valuable under
the current allowance. Do not combine a length-only table with the rich
bound; it added no unique pruning.

Persistence is now validated rather than speculative. A raw `d=14` warm load
reduced phase-2 setup from 7.896 seconds for compute plus dump to 0.135
seconds, preserved top-1 output and counters, and reused the top-1-built table
for the established top-1,000 result. Implement a versioned, checksummed,
atomically published float format immediately after the plain-float bottom-up
kernel; do not wait for packed binary16. The DFS can map it read-only without
an atomic-table copy, and a later encoding version can be added if packed
binary16 wins its wall-time A/B. Additional parallelization remains a later
experiment for first-run latency.

## REVIEW ISSUES

### Open: account for bottom-up layer IDs in the memory budget

The production bottom-up evaluator currently retains one additional 32-bit
exact-key ID per stored exact bag while the full float value table is
allocated. This auxiliary layer index is not charged to
`score_cache_budget`.

For a mostly or entirely exact projection, the wildcard span approaches one,
so the number of exact bags approaches the number of projected value slots.
The layer IDs can then add nearly another four bytes per state and
approximately double preprocessing memory beyond the configured `-C` budget.
Allocation may fail after the value table has already been allocated, causing
`run()` to clear the score bounds and fall back to an impractically large
unbounded search.

Affected implementation:

```text
source/dfs-search.cpp: bottom-up exact-layer construction
```

Resolve this before relying on deep projections near the configured memory
limit. The two proposed approaches are:

1. include the exact-layer index and its allocation overhead in preflight and
   `score_cache_budget` accounting, falling back to the recursive evaluator
   when the combined bottom-up working set does not fit; or
2. generate dependency layers without retaining one ID for every exact bag,
   preserving the value-table-only memory profile at the cost of additional
   key-generation work.

Any fix should avoid silently dropping to score-bound mode off when the
requested bottom-up working set cannot be allocated.
