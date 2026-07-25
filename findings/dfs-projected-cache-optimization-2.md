# Reducing projected score-bound preprocessing time

## Summary

The largest measured optimization opportunity is to quotient concrete anagram
classes into distinct projected actions before constructing the projected
score bound.

The projected recurrence tracks only:

```text
(remaining exact-letter counts, remaining wildcard count)
```

but the current implementation still traverses all concrete anagram classes.
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

The prototype was removed after measurement. The working source was restored,
rebuilt, and passed the focused DFS unit and CLI smoke tests.

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

## Recommended implementation sequence

### 1. Add the projected-action quotient

After projected deltas are constructed:

1. make an array of `(score_key_delta, class_id)`;
2. sort by delta;
3. retain the highest-scoring class in each delta group;
4. assign each retained action to its rarest exact-letter bucket, or the
   wildcard-only bucket; and
5. sort by descending total length within each bucket.

Sorting IDs needs little temporary memory and is more predictable than a large
`unordered_map`.

Useful diagnostics are:

```text
concrete class count
distinct projected action count
per-bucket concrete/action counts
attempted candidate tests
fitting transitions including dead children
successful transitions
```

### 2. Add a compact projected-action record

The projected recurrence does not need wildcard identities or all concrete
requirements. A projection-specific action can contain:

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
error_base    = abs(class_score) + abs(restart) + 1
```

also avoids recalculating invariant arithmetic for every successful
transition.

### 3. Re-sweep end-to-end depth after quotienting

The selector target is:

```text
projected setup time + final DFS search time
```

not preprocessing alone.

With quotienting, measured setup was approximately:

```text
d=14:  9.0s
d=15: 75.6s
```

The existing unquotiented `d=15` search took about 20.2 seconds. If quotienting
leaves the bound unchanged, grouped `d=15` should take roughly 96 seconds for
setup plus search. Grouped `d=14` could therefore spend approximately 87
seconds in final search and still tie it.

Measure `d=13`, `d=14`, and `d=15` with top 1,000 and compare byte-identical
stdout. The best depth may change again after the compact-action work.

### 4. Prototype bottom-up wildcard-vector evaluation

The flat layout stores every wildcard count contiguously for one exact bag:

```text
key = exact_key * wild_span + wild_left
```

For a fixed exact bag:

- the forced exact-letter bucket is fixed;
- exact-fit decisions are identical for all wildcard counts; and
- an action's child wildcard entries form a shifted contiguous range.

A bottom-up kernel could:

1. process exact bags in increasing exact-letter total;
2. compute wildcard counts in increasing order;
3. filter projected actions against the exact bag once;
4. update all fitting wildcard counts together;
5. use ordinary float loads and stores rather than atomic recursion; and
6. parallelize exact bags within one exact-total layer.

Every action consumes the forced exact letter while an exact letter remains,
so its child lies in an earlier exact-total layer. The wildcard-only base layer
depends only on smaller wildcard counts.

The main risk is evaluating unreachable states. At `d=15`, the current
top-down recurrence computes 3.41 million of 7.05 million effective states.
A bottom-up pass might evaluate about 2.07x as many states. Quotienting and
batched wildcard evaluation need to compensate for that extra coverage.

A small viability bitmask per exact bag could record which wildcard counts
have finite completions and skip dead vector positions.

### 5. Add projection-specific support filtering if needed

After quotienting, action support can be cached or indexed by exact presence
mask. There are at most:

```text
2^d
```

such masks, and many projected multiplicity states share one mask.

Possible implementations include:

- a small on-demand support-list cache;
- bitsets of actions requiring each exact symbol; or
- batching all wildcard counts and multiplicities for the same exact support.

This should be measured after quotienting. Reintroducing the former general
candidate-cache machinery is unnecessary.

### 6. Explore coarse-to-fine projected bounds

A shallow quotient projection is cheap:

```text
d=10 quotient setup: about 0.24s
```

It could serve as a complete fallback while a deeper projection is built only
where its tighter value is likely to matter. A deeper-cache miss would use the
coarse projected bound rather than no bound.

This enables:

- lazy deeper refinement near the score floor;
- exact residual refinement only for close calls; and
- cheap upper-bound screening of low-scoring deeper actions.

Ordinary lazy construction previously avoided almost none of the bound graph,
but it had no cheaper complete fallback and therefore still needed each
requested dependency closure.

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

## Lower-priority ideas

### More preprocessing threads

Twenty threads still minimized measured wall time, but supplied only 5.64x
speedup and had 28% parallel efficiency on the thread-sweep workload. Improve
the work count and memory access pattern before investing in more elaborate
root-task scheduling.

### Rounding and `nextafter`

The `d=15` run made about 8.5 million `nextafter` calls but processed 27.1
billion successful transitions. Rounding is already performed primarily once
per computed state. Per-transition invariant score/error arithmetic may be
worth precomputing, but state-level `nextafter` tuning is not a leading target.

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

## Validation required for a production quotient

Minimal focused validation should include:

1. a synthetic input where several concrete classes have the same projected
   delta but different scores;
2. equality of retained output between quotient and unquotiented projection;
3. equality against exhaustive output on the existing small projected test;
4. forced `d=0`, an intermediate `d`, and all-exact projection;
5. identical output across one and multiple preprocessing threads;
6. the 40-letter `d=14` and `d=15`, top-1,000 workload; and
7. reporting both concrete-class and projected-action counts.

The quotient should leave:

```text
computed projected-state count
final DFS node count
solutions visited
retained spellings
```

unchanged relative to the unquotiented projection. Its successful-transition
count is expected to fall because dominated parallel edges have been removed.

## Conclusion

Projected preprocessing is currently dominated by evaluating concrete class
edges that the projection has already made equivalent. Quotienting those
classes is:

- exact;
- local to projected-bound construction;
- compatible with the existing flat projected key;
- independently useful at every projection depth; and
- strongly supported by the 40-letter measurements.

It should precede selector tuning, bottom-up evaluation, support indexing, or
additional parallelization. After quotienting, the next decision should be
made from a new `d=13..15` end-to-end sweep rather than from cache capacity
alone.
