# `dfs-anagrams` phase-2 preprocessing performance

## Current behavior and implementation

### What happens after phase 1

The pause between

```text
# phase 1 complete: ...
```

and

```text
# phase 2: precomputed N bounded states in ...s
```

is phase-2 setup. `DfsAnagramSearch::run()`:

1. Encodes the remaining letter bag as a mixed-radix integer key.
2. Builds separate packed `FitClass` and `ScoreClass` arrays.
3. Allocates the score-bound, support-only, and fitting-candidate caches.
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

The prepass also populates the fitting-candidate cache. Before testing repeated
letter counts, candidate construction consults a smaller cache keyed by the
bag's presence mask. Bags with different multiplicities but the same symbols
therefore reuse the support-mask screening result. The subsequent DFS can reuse
the bound and full candidate list for a remaining bag. This is why the search
can become extremely fast after a noticeable setup pause.

The index file is not accessed during this setup. `IndexReader` mmaps the
index, but phase 1 extracts all data needed by `DfsClassList` before the phase-1
completion message. Locking the index mapping in memory therefore cannot speed
up this phase-2 prepass.

Setup is excluded from phase-2 node and solution counts, which are reset after
`compute_score_bound()` returns. The bound-state count is now reported in the
phase-boundary precompute message, not in the phase-2 completion statistic. The
first DFS progress message means that the real DFS has visited
`100,000 * progress_factor` nodes.

The theoretical mixed-radix state count is

```text
product over symbols s of (input_multiplicity[s] + 1)
```

Only states reachable through fitting classes are computed, but their count
and transition fanout can grow rapidly with the bag.

### Current per-state rounding

Candidate edges are evaluated in `double`. Each successful edge computes

```text
computed_i =
    (best_member_log_score[i] + restart_log_rate) + child_bound[i]
```

and an explicit conservative absolute-error allowance from the three operand
magnitudes. The implementation retains `max(computed_i)` and
`max(error_allowance_i)`, adds those maxima once in `long double`, converts
upward to `double`, and adds two defensive ulps. Thus `long double` and
`nextafter()` work are paid once per non-dead state, not once per transition.
The memo table itself stores one 8-byte `double` per state.

Score pruning now fails open unless `double` is IEEE-754 binary64,
`long double` has at least binary64 precision, the runtime rounding mode is
round-to-nearest, and the translation unit was not compiled with
`__FAST_MATH__`. Supported builds must also leave reassociation and other
unsafe math transformations disabled, as the Meson build does.

For finite operands without overflow, let `u = DBL_EPSILON / 2`. The two
round-to-nearest additions have absolute error bounded by the corresponding
exact operand magnitudes times `u`; the second bound also includes the first
addition's error. The implementation calculates the sum of the three absolute
operand magnitudes, adds one to dominate subnormal absolute error, and
multiplies by `4 * DBL_EPSILON = 8u`. This covers the two additions plus
rounding while the magnitude itself is accumulated. If the magnitude or score
calculation overflows, the allowance or candidate becomes positive infinity,
which is conservative.

Let `H*` be the exact-arithmetic relaxed bound and suppose inductively that
every stored child bound is at least `H*` for that child. The base case is
exact: the empty bag stores zero. For every edge `i`, the error allowance
guarantees

```text
exact_sum_i <= computed_i + error_allowance_i
```

when `exact_sum_i` uses the already-admissible stored child. Addition is
monotone, so that exact sum is at least the same edge evaluated with
`H*(child)`. If `C = max(computed_i)` and
`E = max(error_allowance_i)`, then for every `i`,

```text
exact_sum_i <= computed_i + error_allowance_i <= C + E
```

The `long double` addition is evaluated with at least binary64 precision.
Conditional upward conversion followed by two unconditional upward ulps makes
the stored parent at least `C + E`, including the final addition/conversion
error. It therefore bounds every edge and, by induction on the number of
letters remaining, is an admissible recursive bound. The final prune comparison
retains its separate depth-scaled padding for regrouping with the accumulated
path score.

Tests exercise a multi-level bound with a floor one ulp below the best
exhaustive score and verify that the best path is not pruned. They also switch
the runtime to downward rounding and verify that score-bound construction is
disabled, then restore round-to-nearest. Dense and sparse retained-result tests
continue to compare with exhaustive search.

### Score-table allocation and sparse exhaustion

The default 64 MiB cache budget now permits a dense score array to use up to
one half of the budget. Dense storage has no hash keys or empty slots, and a
retained table is substantially more valuable than candidate-cache capacity.
When dense storage does not fit, the sparse score table retains its previous
one-quarter budget. At the default budget it has 1,048,576 slots and a 50%
load limit: 524,288 stored states.

If that table fills during eager construction, the implementation discards the
entire score table, returns its memory to the candidate cache, and runs phase 2
without score pruning. A message such as

```text
# phase 2: precomputed N bounded states in ...s
```

then counts states computed before the table was discarded. It does **not**
mean that `N` bounds remain available to the DFS.

## Completed optimization and validation results

Work completed on 2026-07-24:

- Added counters for successful bound transitions and `nextafter()` calls.
- Split phase-2 setup time from real DFS time.
- Moved conservative upward rounding from every transition to every stored
  state.
- Validated byte-identical stdout on the reference workloads and compared
  retained dense and sparse results with exhaustive search in tests.
- Recounted `nextafter()` calls after the change.
- Replaced per-edge `long double` evaluation with `double` plus an explicit
  conservative error envelope.
- Reconfirmed sparse-table exhaustion on the 30-letter workload.
- Profiled the post-change setup path with Callgrind instruction, cache, and
  branch simulation.
- Split each 32-byte class record into separate 16-byte fit and score records,
  so support-mask scans touch four fit records per cache line.
- Made the bag-mask update after class subtraction branchless.
- Wrote fitting candidate IDs directly into the aligned cache arena instead of
  staging them in a vector and copying them.
- Used the existing descending class-length order to binary-search past classes
  that are too long for the remaining bag.
- Measured the corrected 28-letter workload.
- Added a budgeted support-only candidate cache keyed by `bag_mask`.
- Measured its setup savings, memory use, and effect on the subsequent DFS.
- Allowed a dense score table, but not a sparse one, to use up to half of the
  total cache budget.
- Re-ran the 30-letter workload and retained its complete bound graph.

At that stage, the CLI reported setup at the phase boundary:

```text
# phase 2: precomputed 143693 bounded states in 3.714s
```

and detailed counters after the search:

```text
# phase 2 timing: 3.714487 s setup, 0.399275 s search, 43424967 successful bound transitions, 345673 nextafter calls
```

One warm run of each reference workload before and after the follow-up changes
measured:

| Normalized bag | Letters | States | Transitions | Before | After | Reduction |
|---|---:|---:|---:|---:|---:|---:|
| `aadeeefiimnorsstttu` | 19 | 18,380 | 2,744,344 | 0.189 s | 0.139 s | 26.5% |
| `aabdeeefiimnorsstttu` | 20 | 34,845 | 7,489,507 | 0.508 s | 0.382 s | 24.8% |
| `aaabeeeeghhiimnrrrsttwwww` | 25 | 143,693 | 43,424,967 | 3.714 s | 2.831 s | 23.8% |

Across three interleaved development measurements of the 25-letter workload,
the median fell from 3.675 seconds to 2.771 seconds, a 24.6% reduction. The
state, transition, and `nextafter()` counts were unchanged. Stdout remained
byte-identical, dense and sparse score-bound tests compared retained spellings
with exhaustive search, and the indexed CLI differential test passed.

### Profile findings

An instruction-only Callgrind run on the 25-letter workload attributed 94.4%
of all executed instructions to bound construction. A cache-and-branch
simulation on the 20-letter workload found:

- candidate-list construction was 34.1% of all instructions;
- it caused 82.7% of L1 data-read misses;
- the support-mask scan tested 216.1 million class records, of which only
  9.3 million ultimately fit;
- the conditional bag-mask update after subtraction was highly unpredictable.

Splitting the class layout reduced simulated L1 data misses from 144.1 million
to 92.1 million, a 36.1% reduction, without increasing last-level misses.
Replacing the conditional bag-mask update removed 51.5 million conditional
branches and 11.34 million mispredictions; the simulated misprediction rate
fell from 4.3% to 3.6%.

Native 25-letter medians for the individual cumulative prototypes were:

| Version | Precompute | Change from previous |
|---|---:|---:|
| Committed baseline | 3.675 s | — |
| Split fit/score layout | 3.612 s | -1.7% |
| Branchless bag-mask update | 2.921 s | -19.1% |
| Direct candidate-arena writes | 2.854 s | -2.3% |
| Skip too-long class prefix | 2.771 s | -2.9% |

### Support-only candidate cache

Candidate construction now reuses support-mask screening across multiplicity
states. The support cache is a fixed sparse hash table keyed by `bag_mask` plus
an aligned arena of 32-bit global class IDs. Each entry scans the complete
rarest-symbol bucket and retains the classes whose support is a subset of the
mask. The IDs remain in global class order and therefore also retain descending
class-length order. Full per-bag candidate construction binary-searches past
too-long support candidates and tests only repeated-letter requirements.

The support cache reserves 1/16 of the candidate-cache budget after the score
table is allocated. One quarter of that reservation is the maximum metadata
share; the rest is the ID arena. Allocation and admission failures retain the
original support-and-multiplicity scan. Cache statistics are reported
separately:

```text
# phase 2 caches: 2048 support masks, 2164816 support bytes, 89739 candidate entries, 60322556 candidate bytes, 143693 bound entries, 2764800 bound bytes
```

The 1/16 split was selected against 1/8, 1/24, and 1/32 prototypes. The 1/32
version made the 25-letter setup a few milliseconds faster but exhausted its
support arena after 2,026 of 2,048 masks and slowed the real DFS. The 1/16
version retained all masks while returning roughly 4 MiB more to the full
candidate arena than 1/8.

Three interleaved warm runs used `-m 4 -n 10000`. "Before" is the immediately
preceding implementation and "after" includes the 1/16 support cache:

| Normalized bag | Before setup | After setup | Setup reduction | Before search | After search |
|---|---:|---:|---:|---:|---:|
| `aadeeefiimnorsstttu` | 0.134780 s | 0.091227 s | 32.3% | 0.034154 s | 0.035017 s |
| `aabdeeefiimnorsstttu` | 0.369778 s | 0.244672 s | 33.8% | 0.074303 s | 0.077663 s |
| `aaabeeeeghhiimnrrrsttwwww` | 2.793691 s | 2.012010 s | 28.0% | 0.299393 s | 0.306876 s |

The small search-time increases are the cost of reserving space that would
otherwise hold full per-bag candidate lists. On the 25-letter workload, total
phase-2 time still fell from 3.093084 to 2.318886 seconds, or 25.0%. Stdout was
byte-identical for every comparison.

### Corrected 28-letter measurement

For the normalized bag

```text
aaaabeeeeghhiilmnnrrrstttwww
```

phase 1 produced 288,713 entries, 96,508 classes, and 1,904,614 trie
nodes. Phase-2 setup retained 533,865 bounded states after 387,268,276
successful transitions. The support cache reduced a warm `-n 1` setup from
27.271 to 21.550 seconds, or 21.0%:

```text
# phase 2: precomputed 533865 bounded states in 21.550s
```

The subsequent search took 0.001654 seconds. The cache retained 3,328 support
masks using 3,503,080 charged bytes.

For the normalized 30-letter bag

```text
aaaabdeeeeghhiilmnnrrrstttwwww
```

phase 1 produced:

```text
459162 entries, 166852 classes, 3054340 trie nodes
```

Under the old quarter-budget policy, the sparse table filled at 524,288 states
and was discarded. The support cache reduced that doomed prepass from 21.307 to
15.561 seconds, after which an unpruned 30-second validation still reached 430
million nodes and 49.96 million solutions without completing.

The theoretical dense table is 27,648,000 bytes, so it fits inside half of the
existing 64 MiB total budget. Allowing dense score storage to use that half
retains the complete 1,406,323-state graph:

```text
# phase 2: precomputed 1406323 bounded states in 89.761s
# phase 2 timing: 89.760621 s setup, 0.003205 s search, 1501837804 successful bound transitions, 3502860 nextafter calls
# phase 2 caches: 3213 support masks, 2466304 support bytes, 82576 candidate entries, 36994560 candidate bytes, 1406323 bound entries, 27648000 bound bytes
```

The prepass is much longer because it now computes the entire reachable graph
and its 1.50 billion fitting transitions instead of stopping at the sparse
limit. It converts a non-terminating practical failure into a bounded one-time
setup: the subsequent top-1 DFS visited 62,432 nodes, found 13 solutions, and
finished in 0.003 seconds. All cache charges still total exactly 64 MiB.

Against the original seven-step measurement sequence, steps 1--6 are complete.
Step 7 first confirmed exhaustion and now validates the dense-allocation
policy.

## Historical experiments

The experiments in this section describe removed prototypes and the
pre-2026-07-24 implementation. They are retained as evidence, not as the
current behavior.

### Eager versus lazy construction

A temporary `--lazy-score-bound` prototype delayed computing `H(B)` until:

- the node was below the root;
- the top-N spelling heap was full and exposed a score floor; and
- the DFS actually needed the bound.

Each lazy request still computed and memoized the full transitive dependency
closure below that bag. The reported count was therefore the number of bound
states actually computed, not merely the number of direct DFS lookups.

The prototype reported

```text
N bound states computed lazily
```

The prototype and CLI option were removed after measurement.

Measurements used `idx/wiki-merged.5.index`, `-m 4`, and top 10,000:

| Normalized bag | Letters | Eager states | Lazy states | Avoided |
|---|---:|---:|---:|---:|
| `aadeeefiimnorsstttu` | 19 | 18,380 | 17,979 | 401 |
| `aabdeeefiimnorsstttu` | 20 | 34,845 | 34,353 | 492 |
| `aaabeeeeghhiimnrrrsttwwww` | 25 | 143,693 | 143,689 | 4 |

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

### Pre-change transition costs

The old implementation performed the following for every fitting class
transition:

1. Subtracted the class's packed letter requirements.
2. Recursively loaded or computed the child bound.
3. Restored the bag.
4. Added class score, restart score, and child bound in `long double`.
5. Rounded the candidate upward into `double`.
6. Called `nextafter()` two unconditionally and sometimes three times.

On the measured Linux x86-64 build, `long double` used x87 80-bit extended
precision stored in a padded 16-byte object:

```text
sizeof(long double) = 16
LDBL_MANT_DIG       = 64
DBL_MANT_DIG        = 53
```

An instrumented 25-letter run with `-n 1` measured:

```text
35,711 classes
143,693 bound states
35,742 total log() calls
107,003,050 total nextafter() calls
```

The recurrence itself called no logarithms. Almost every `log()` was the
one-time calculation of `log(best_member_count)` for an extracted class; two
more calculated the restart rate. Additional logarithms occurred later while
expanding non-leading class members into output spellings. The expensive
preprocessing work therefore scaled with bound-graph edges, not logarithms or
only the number of memoized states.

### Original exhaustion observation

The same normalized 30-letter bag reached the sparse table's 524,288-state
limit under the old implementation. An earlier unpruned search was stopped
after:

```text
2,110,000,000 phase-2 nodes
351,578,227 solutions
```

No final output had yet been written. That observation predates the expanded
dense-table allowance, which now avoids sparse mode for this bag. Inputs whose
dense table exceeds half of the total budget can still enter the sparse
fail-open path described above.

## Validation status

The optimization, cache-budget policy, rounding preconditions, and reference
workloads described above are implemented and covered by the smoke, deep-bound,
14-letter, CLI, and indexed differential tests.
