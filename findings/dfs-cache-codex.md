# `dfs-anagrams` cache-size performance cliffs

## Implementation update

The dense experiments proposed below are now implemented in this branch:

- the score cache has its own rarest-most-significant mixed-radix key;
- complete tables omit the root symbol's unused top slab;
- exact-double storage is preferred when it fits, followed by
  upward-rounded float storage;
- smaller budgets become an exactly sized, tag-free float prefix; its bounds
  are constructed lazily after phase 2 enters the dependency-closed prefix
  with an active score floor.

The sparse score table described later in this document was the baseline for
the investigation and has been replaced by the dense-prefix fallback.
Candidate and support cache keying is unchanged.

## Summary

`dfs-anagrams` does not degrade smoothly as `--candidate-cache-mib` (`-C`)
shrinks. The principal cliff comes from the score-bound cache, not the fitting
candidate cache.

The shared `-C` budget funds three structures:

1. A support-mask cache that rejects classes containing absent symbols.
2. A fitting-candidate cache keyed by the complete remaining bag.
3. A completion-score cache holding the exact relaxed bound `H(bag)`.

The first two reduce work per DFS node. The third can eliminate nearly the
entire DFS tree.

Candidate-cache exhaustion is local and gradual: an uncacheable state falls
back to scanning its forced-symbol bucket. Sparse score-cache exhaustion is
also fail-open, but it does **not** discard completed bounds. Preprocessing
stops when the table reaches its admission limit, retains the completed
postorder entries already stored, and starts phase 2. A score-cache hit can
still prune; a miss simply runs unpruned.

The remaining performance cliff is therefore a coverage problem rather than
an all-or-nothing loss of the sparse table. The retained postorder entries tend
to be lower in the state graph and may not cover the high-leverage states at
which pruning would eliminate large subtrees.

## Historical small-cliff measurement

The measured query was the 21-letter fragment whose sorted letter multiset is:

```text
abdeeeeffiinnoosuuvwy
```

The two runs used the wiki index, `-p 1000`, and the default top 10,000. They
were recorded with the former allocation/exhaustion policy and should not be
read as measurements of the current retained-sparse implementation:

| `-C` | Setup | Search | DFS nodes | Solutions visited | Bound entries retained |
|---:|---:|---:|---:|---:|---:|
| 2 MiB | 0.0396 s | 34.9478 s | 1,104,125,791 | 240,864,573 | 0 |
| 3 MiB | 0.1952 s | 0.0899 s | 956,615 | 11,892 | 38,594 |

End-to-end wall times were 34.34 seconds and 0.34 seconds respectively, about
a 101x difference. The score bound reduced the DFS node count by approximately
1,154x.

The candidate cache does not explain the improvement:

| `-C` | Support bytes | Candidate entries | Candidate bytes | Bound bytes |
|---:|---:|---:|---:|---:|
| 2 MiB | 131,072 | 11,648 | 1,966,080 | 0 |
| 3 MiB | 82,488 | 10,856 | 1,782,720 | 1,244,160 |

The slower historical run retained more candidate-cache data. What it lacked
was score-bound coverage. Under the current behavior, an exhausted sparse
table remains allocated and its completed entries remain available, so the
corresponding current-policy result must be measured separately.

## How bag states are encoded

For each distinct symbol `s`, let its input multiplicity be `m[s]`. The
remaining bag is encoded as a collision-free mixed-radix key. Its theoretical
state count is:

```text
S = product over symbols s of (m[s] + 1)
```

Only states reachable through fitting anagram classes are evaluated, but dense
storage is sized for all `S` theoretical states.

For the 21-letter example:

```text
S = 155,520
dense score bytes = 8 * S = 1,244,160
```

Dense score storage currently has priority and may use the entire `-C` budget,
so the minimum total budget is:

```text
1,244,160 bytes = 1.187 MiB
```

The CLI accepts whole MiB, making `-C 2` the first value that admits the dense
table.

Ignoring the negligible 64-byte alignment adjustment, the minimum integer
setting for dense score bounds is:

```text
minimum -C = ceil(8 * S / 2^20)
```

For all-distinct inputs, `S = 2^letters`. Under the present full-budget policy,
26 distinct letters require about 512 MiB and 30 require about 8 GiB.

## Dense opportunities below the full-table threshold

### The cache is indexed by bag state, not DFS path

`H(B)` depends only on the remaining bag. Different DFS paths and
`entry_point` values that reach the same bag share one bound entry. A dense
optimization should therefore select or compact mixed-radix bag states, not
try to assign slots to path nodes.

Every class subtraction decreases the mixed-radix key. This makes a
downward-closed set of keys particularly useful: once search enters the set,
all descendants remain in it and recursive bound construction can memoize its
entire dependency closure.

### First optimization: omit the unused root slab

Let the initially forced rarest symbol have multiplicity `m`. The root itself
is never pruned against the top-N score floor: after the special `-HUGE_VAL`
dead-end case, `should_prune()` returns false when `path.empty()`. Every root
candidate contains the forced symbol, so every non-root state has a remaining
count strictly less than `m` for that symbol.

The current full dense array nevertheless allocates all `m + 1` planes,
including the plane where that count is still `m`. Only the root can reach that
plane before the first forced subtraction. Compacting it out gives:

```text
effective dense states = S * m / (m + 1)
effective dense bytes  = 8 * S * m / (m + 1)
```

This is not partial coverage: it retains every score bound that phase 2 can use
for score-floor pruning. It also preserves eager recursive preprocessing and
its existing root-candidate parallelism. The root value can be calculated as
the reduction of its child values without occupying a table slot, preserving
the `-HUGE_VAL` dead-root shortcut as well.

The cleanest indexing form is to give rarer symbols the most-significant
mixed-radix digits for the score cache. The root then has key `S - 1`, while
every root child has key below `S * m / (m + 1)`. A dense prefix of exactly that
size covers all non-root descendants. A separate score key may be preferable
to changing the fitting-candidate cache's existing locality.

On the wiki index, the initially forced symbol is a singleton in every
reference fragment in the state-count table. The exact-double root-slab
optimization therefore halves their dense allocation:

| Letters | Theoretical states | Current double | Root-slab double | Root-slab float |
|---:|---:|---:|---:|---:|
| 21 | 155,520 | 1.187 MiB | 0.593 MiB | 0.297 MiB |
| 22 | 207,360 | 1.582 MiB | 0.791 MiB | 0.396 MiB |
| 24 | 622,080 | 4.746 MiB | 2.373 MiB | 1.187 MiB |
| 25 | 1,244,160 | 9.492 MiB | 4.746 MiB | 2.373 MiB |
| 27 | 2,239,488 | 17.086 MiB | 8.543 MiB | 4.271 MiB |
| 28 | 4,478,976 | 34.172 MiB | 17.086 MiB | 8.543 MiB |
| 30 | 13,436,928 | 102.516 MiB | 51.258 MiB | 25.629 MiB |
| 32 | 23,514,624 | 179.402 MiB | 89.701 MiB | 44.851 MiB |
| 35 | 78,382,080 | 598.008 MiB | 299.004 MiB | 149.502 MiB |

Thus the 30-letter fragment would fit exact-double dense bounds in 64 MiB
without any loss of coverage. The 32-letter fragment would fit if this
compaction were combined with a 32-bit value.

### Second optimization: an upward-rounded 32-bit dense value

The recurrence can remain in `double` while the table stores an IEEE-754
`float` rounded toward positive infinity. On lookup, conversion back to
`double` is exact. This preserves admissibility: quantization can make a bound
too high and therefore miss a prune, but cannot make it too low and incorrectly
prune a result.

The dense value becomes four bytes, including 32-bit unseen/computing NaN
sentinels for parallel preprocessing. This doubles capacity and retains direct
indexing and lock-free atomic construction on ordinary targets. Finite
negative values that convert to `-infinity` need the same explicit upward step
toward `-FLT_MAX`; `+infinity` remains a safe bound.

This should be a fallback mode when exact-double dense storage does not fit,
not an unconditional replacement until benchmarks show the quantization does
not materially reduce prune counts. Useful validation is:

- byte-identical retained output versus exact-double and exhaustive runs;
- dense setup time;
- phase-2 nodes and bound prunes;
- the maximum and distribution of `float_bound - double_bound`.

### True partial dense: a rarest-first key prefix

If even the compact or 32-bit complete-effective table does not fit, allocate
exactly:

```text
N = floor(score_budget_bytes / value_bytes)
```

entries and cover score keys in `[0, N)`. With rarest symbols in the
most-significant digits, low keys mean that the rare symbols which drive the
DFS have already been depleted. Because every subtraction decreases the key,
the prefix is dependency-closed:

```text
parent key < N  =>  every child key < N
```

Lookup needs only `key < N` followed by `values[key]`; there is no stored key,
tag, hash, collision, or probe. Cutting through a radix plane is also safe, so
this uses every available slot instead of reducing capacity only in whole
symbol-sized steps.

This selection policy deliberately trades bounds at earlier states for
complete dense coverage below the point where search enters the prefix. That
is more plausible than using the current rarest-first *least-significant*
encoding with `key < N`, which would primarily constrain the counts of common
symbols and tend to admit states later.

The hard part is construction. Calling the existing eager recursion at the
root while declining to memoize every key at or above `N` can repeatedly
recompute the uncached upper DAG. Two safer prototypes are:

1. Implement the root-slab case first. Only the root is outside the table, so
   the current parallel prepass remains efficient.
2. For a smaller prefix, construct a missing bound lazily when phase 2 first
   reaches `key < N` and has a usable score floor. All recursive dependencies
   of that state are inside the dense prefix and can be memoized normally.

An alternative is to iterate all theoretical keys `0..N-1` in increasing
order, because every dependency has a lower key. That has predictable bounded
memory, but it evaluates unreachable states; the existing workloads show that
reachable states can be a small fraction of `S`, so this is unlikely to be the
first choice.

Instrumentation for the partial-prefix prototype should report, by DFS depth
or letters remaining:

- first entry into the prefix;
- distinct dense entries computed;
- dense lookup hits and misses;
- prunes and estimated nodes skipped;
- time spent constructing lazy bounds.

### Paged dense storage

A two-level dense table is another useful experiment. Split the theoretical
array into fixed-size pages, keep a small directory by high key bits, and
allocate a dense value page only when its key range is selected. It avoids a
key field per entry and permits more flexible coverage than one prefix.

Its value depends on clustering. With 4 KiB pages and 8-byte values, one page
covers 512 keys even if only a few are reachable. Page occupancy should be
measured before adopting this design. A demand-zero virtual mapping is a
related implementation technique, but the `-C` budget still needs an explicit
resident-page limit; relying on virtual overcommit alone would not honor the
cache contract.

### Why a tag beside every dense value is secondary

A direct-mapped table can use `slot = key & mask` and store the exact high
quotient as a tag. It is faster than a probed hash table and a packed quotient
may need only one to four bytes, but it is no longer dense in the important
memory sense:

- a full 64-bit key plus double value costs 16 bytes per slot;
- a 32-bit exact quotient plus double costs 12 bytes in separate arrays;
- collisions leave slots unused or require replacement;
- an exact tag is mandatory because a fingerprint collision could cause an
  unsafe prune.

This layout is worth a lookup-speed benchmark if prefix coverage proves poor,
but it should follow the no-tag root-slab, 32-bit-value, and rarest-prefix
experiments.

### Limits of “cache the most expensive states”

Earlier states generally have larger subtrees, but their exact `H` values
depend on all lower states. Selecting root-near states without retaining their
dependency closure either needs a separate construction workspace, a second
pass over the graph, or potentially exponential recomputation after eviction.
Fanout alone also does not predict whether a bound will beat the eventual
top-N floor.

For that reason, a rarest-first downward-closed prefix is a stronger initial
policy than a tagged priority cache. Profile-guided pages or priority
replacement can be reconsidered after depth-specific hit/prune instrumentation
shows where the prefix loses value.

## Dense versus sparse score storage

Score-bound allocation occurs in `DfsAnagramSearch::prepare_score_bounds()`:

```text
source/dfs-search.cpp:422
```

The current policies are:

- Dense bounds may consume the entire budget.
- If dense storage does not fit, sparse bounds also receive the entire budget.
- Sparse storage uses separate 8-byte key and 8-byte value arrays.
- Sparse capacity is a power of two.
- Admission stops at 75% occupancy.
- An exhausted sparse table and all of its completed entries are retained.
- In sparse mode the current implementation leaves no shared budget for the
  support or fitting-candidate caches.

At 2 MiB:

```text
sparse budget = 2 MiB = 2,097,152 bytes
slots = 2,097,152 / 16 = 131,072
maximum retained entries = 131,072 * 3 / 4 = 98,304
```

The historical setup reported exactly 16,384 computed states because the
former policy gave sparse storage one quarter of the budget and stopped at 50%
occupancy. Its zero retained-entry result describes the former implementation,
which cleared the table after exhaustion; it does not describe current
behavior.

At the default 64 MiB, current sparse score storage has 4,194,304 slots and
admits at most 3,145,728 states.

## What happens on sparse exhaustion

`compute_score_bound()` stores states after recursively completing their
children. When `store_score_bound()` cannot admit another sparse entry, it sets
`bound_aborted`.

After unwinding to `run()`, the current implementation:

1. Keeps the sparse table and every completed entry.
2. Clears `bound_aborted` so ordinary lookup can continue.
3. Runs phase 2 with score pruning on cache hits.
4. Treats a cache miss as “no bound available” and explores that state
   unpruned.

The relevant path is:

```text
source/dfs-search.cpp:804-819
```

This partial fallback is correct because entries are published only after the
recursive computation of their dependencies completes. A missing bound cannot
cause an unsafe prune; it only forgoes pruning. The practical question is how
much useful search-tree coverage the retained postorder subset provides.

By contrast, fitting-candidate admission failure stores a bypass marker or
uses the ordinary fit scan for that state:

```text
source/dfs-search.cpp:943-1002
source/dfs-search.cpp:1491-1556
```

That can make each node slower but does not directly change the node count.

## Why letter-count increases are discontinuous

Selected fragments have these theoretical state counts. Their letters are
shown alphabetically so the source sequence is not recorded here:

| Letters | Sorted fragment | States | Dense threshold | Derived max words |
|---:|---|---:|---:|---:|
| 21 | `abdeeeeffiinnoosuuvwy` | 155,520 | 2 MiB | 5 |
| 22 | `abdeeeeffiinnnoosuuvwy` | 207,360 | 2 MiB | 5 |
| 24 | `abddeeeeffiinnnoostuuvwy` | 622,080 | 5 MiB | 6 |
| 25 | `abddeeeeffhiinnnoostuuvwy` | 1,244,160 | 10 MiB | 6 |
| 27 | `abbddeeeeeffhiinnnoostuuvwy` | 2,239,488 | 18 MiB | 6 |
| 28 | `abbddeeeeeffhiilnnnoostuuvwy` | 4,478,976 | 35 MiB | 7 |
| 30 | `aabbddeeeeeffhiilnnnoostuuvwyz` | 13,436,928 | 103 MiB | 7 |
| 32 | `aabbddeeeeeeffhiillnnnoostuuvwyz` | 23,514,624 | 180 MiB | 8 |
| 35 | `aabbddeeeeeeffhiikllnnnoooostuuvwyz` | 78,382,080 | 599 MiB | 8 |

A newly introduced distinct symbol doubles `S`. Separately, with the default
minimum word length of four, maximum DFS depth increases at every multiple of
four letters.

Under the current full-budget dense policy, this particular 28-letter fragment
has a 35 MiB threshold and therefore remains dense at the 64 MiB default. The
depth boundary still matters, but it is no longer coupled to a dense-to-sparse
transition for this bag.

## Existing longer-input evidence

The repository already contains a measured 30-letter example:

```text
aaaabdeeeeghhiilmnnrrrstttwwww
```

Its dense score table is 27,648,000 bytes and its reachable graph contains
1,406,323 states and approximately 1.50 billion successful transitions.

With the former sparse allocation and exhaustion behavior, it filled at
524,288 states, cleared the table, and an unpruned 30-second validation reached
430 million DFS nodes without completing. This is historical evidence for the
cost of losing all bounds, not a description of current sparse exhaustion.
After allowing the dense table to use half of the existing 64 MiB budget,
preprocessing completed and the final top-1 DFS took about 0.003 seconds.

Parallel dense preprocessing reduced the same setup from 89.7 seconds on one
thread to 10.1 seconds on 20 threads. A separate 28-letter setup improved from
21.4 seconds to 2.24 seconds with an explicit 20-thread request.

See:

```text
findings/preprocess-perf.md
plans/preprocess-threads.md
```

## Immediate operational mitigations

### Calculate the dense threshold before starting

For a given bag, calculate:

```text
S = product(count + 1)
dense table bytes = round_up_to_64(8 * S)
minimum total budget = dense table bytes
```

Then choose `-C` at or above the corresponding whole-MiB value. Any remaining
budget may be available to the support and fitting-candidate caches when those
caches are enabled.

### Use preprocessing threads for dense tables

`--preprocess-threads N` (`-T N`) parallelizes dense score-bound construction.
It does not parallelize sparse construction or the final DFS.

Automatic mode currently remains single-threaded below 26 letters. The
implementation caps the actual worker count based on available hardware and
root work.

### Restrict depth when semantics permit

Increasing `--min-word-length` reduces the extracted class list and the derived
maximum depth. A future explicit `--max-words` option would provide more direct
control over the combinatorial search depth.

### Distinguish complete coverage from retained partial coverage

The final `bound entries` statistic reports retained entries. On sparse
exhaustion it remains nonzero and the warning explicitly says that completed
bounds were retained. That count alone does not show how frequently phase 2
will hit those entries, so cache-hit and prune statistics are more useful for
judging partial coverage.

## Recommended implementation changes

### 1. Preflight diagnostics are implemented; consider a guardrail

Before preprocessing, report:

- theoretical mixed-radix state count;
- dense score-table bytes;
- minimum total `-C` required by the current policy;
- selected score mode;
- sparse capacity and admission limit;
- selected candidate-cache mode.

The current implementation reports these preflight fields and, on exhaustion,
reports that completed score bounds were retained and that only cache misses
will run unpruned.

An optional `--require-complete-score-bound` or fail-fast mode could prevent a
run from silently entering a potentially weak partial-coverage fallback.

### 2. Prioritize or separate score-cache memory

The score cache has much greater leverage than the candidate cache: the small
example was faster with fewer candidate entries because score pruning removed
over 99.9% of DFS nodes.

The current working policy already lets dense or sparse score storage consume
the entire shared budget. Remaining interface and policy options include:

- Add separate `--score-cache-mib` and `--candidate-cache-mib` options.
- Decide whether to reserve a configurable minimum for support/candidate data.
- Base the split on state count and expected score mode rather than fixed
  fractions.
- Expose the current score-priority choice rather than making it implicit.

For example, the 28-letter fragment needs a 34.2 MiB dense score array and now
fits under a 64 MiB total. Under the former half-budget policy it did not.

### 3. Measure and improve retained sparse coverage

Completed sparse entries are valid admissible bounds: they are published only
after all recursive children needed to calculate them have completed.

The current implementation already stops eager construction, retains completed
entries, uses them in `should_prune()`, and continues unpruned on misses. The
next step is to measure lookup-hit and prune rates by remaining letter count or
DFS depth. That will show whether postorder admission overrepresents cheap,
low-leverage states near the bottom of the graph and will guide a partial-dense
or selective-admission policy.

### 4. Increase sparse-table utilization

Current sparse storage effectively uses:

- 16 bytes per slot;
- 4/3 slots per admitted state at the 75% admission limit;
- the entire score-cache budget when dense storage does not fit.

Before power-of-two rounding, each possible retained state corresponds to
roughly 21.3 bytes of `-C` budget, versus 8 bytes of dense score storage.

Potential improvements include:

- raising the load factor after probe-cost measurement;
- avoiding power-of-two rounding losses;
- using a more compact key or entry representation where safe;
- replacing open-addressed sparse storage with a partial-dense layout when its
  selection policy gives better coverage.

### 5. Select preprocessing threads from workload size

The current automatic threshold is raw letter count 26. A better policy could
consider:

- theoretical state count;
- dense table size;
- extracted class count;
- number of fitting root candidates.

Sparse preprocessing remains single-threaded. Parallelizing it needs a
concurrency-safe admission strategy and should be evaluated only after retained
coverage is understood.

### 6. Add a depth-aware maximum-word limit

An explicit maximum word/class count can eliminate whole DFS levels. For it to
reduce eager score-bound setup as well as final search, the bound recurrence
must account for remaining slots; merely applying the cap in the final DFS
would leave the existing full `H(bag)` prepass unchanged.

This is a semantic restriction and should be user-controlled, but it is among
the strongest available mitigations when the desired answers naturally use a
small number of words.

### 7. Persist completed dense score tables

For repeated runs of the same index, bag, minimum length, and score model, save
and memory-map the dense `H` table. The score bound is independent of the
requested top-N count, so different `-n` runs could reuse it.

This does not accelerate the first run, but it can remove minutes or hours from
iterative searches.

## Strategies already measured or unlikely to solve the cliff

### Lazy score-bound construction

A prior prototype delayed `H` construction until the top-N heap had a usable
floor. It avoided:

- 401 of 18,380 states at 19 letters;
- 492 of 34,845 states at 20 letters;
- only 4 of 143,693 states at 25 letters.

The 25-letter lazy run was slightly slower. A requested bound still needs the
complete transitive dependency closure below that bag, so laziness alone does
not materially reduce the graph on the measured workloads.

### Candidate-cache tuning alone

Candidate caching substantially reduces repeated fit tests, but it cannot
replace score pruning. The 21-letter `-C 2` run retained more candidate data
than `-C 3` and still traversed 1.10 billion nodes.

### A naive per-letter score bound

The previously tested sum of independently best per-letter scores was too
relaxed and pruned no nodes on the 19-letter reference workload. A cheaper
replacement for exact `H` would need a stronger relaxation, such as a
multi-symbol pattern database or another compatibility-aware bound.

## Suggested implementation order

1. Compact out the initially forced symbol's unused root plane while retaining
   exact-double values and the parallel dense prepass.
2. Add an upward-rounded 32-bit dense fallback and compare its prune rate with
   exact double.
3. Add a separate rarest-most-significant score key and prototype a bounded
   dense prefix with lazy, dependency-closed construction.
4. Measure reachable occupancy by dense page before deciding whether a paged
   dense layout is worthwhile.
5. Benchmark a packed-quotient direct-mapped table only if prefix coverage is
   inadequate.
6. Add a fail-fast option for runs that cannot construct the requested degree
   of score-bound coverage.
7. Make the score/candidate allocation policy configurable or adaptive.
8. Lower or replace the automatic threading threshold.
9. Add a depth-aware `--max-words` option if its semantic restriction is useful.
10. Consider persistent tables and stronger bounded-memory relaxations for
   inputs whose exact score graph cannot fit RAM.
