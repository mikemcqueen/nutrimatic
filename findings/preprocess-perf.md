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
reuse both the bound and candidate list for a remaining bag. This is why the
search can become extremely fast after a noticeable setup pause.

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

This argument assumes IEEE-754 binary64 operations in round-to-nearest mode,
the default round-to-nearest behavior on the final `long double` path, finite
score operands, and no compiler transformation that violates the stated
evaluation model. Let `H*` be the exact-arithmetic relaxed bound and suppose
inductively that every stored child bound is at least `H*` for that child. The
base case is exact: the empty bag stores zero. For every edge `i`, the error
allowance guarantees

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

Upward conversion and the extra ulps make the stored parent at least `C + E`.
It therefore bounds every edge and, by induction on the number of letters
remaining, is an admissible recursive bound. The final prune comparison retains
its separate depth-scaled padding for regrouping with the accumulated path
score.

This is a short implementation argument, not yet a complete cross-platform
floating-point proof. Stronger justification and targeted rounding tests remain
work.

### Sparse-table exhaustion

The default 64 MiB cache budget gives score bounds at most one quarter of the
budget. When dense storage does not fit, the sparse score table has 1,048,576
slots and a 50% load limit: 524,288 stored states.

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

The CLI reports setup at the phase boundary:

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

The remaining obvious CPU target is support-mask screening. Many
mixed-radix states have different multiplicities but the same presence mask,
yet candidate construction rescans the same rare-letter bucket for every
state. A support-only cache keyed by `bag_mask` could reuse that first-stage
filter. It needs careful arena-budget accounting and must retain ascending
class order for the canonical phase-2 traversal.

### Corrected 28-letter measurement

For the normalized bag

```text
aaaabeeeeghhiilmnnrrrstttwww
```

phase 1 produced 288,713 entries, 96,508 classes, and 1,904,614 trie
nodes. Phase-2 setup retained 533,865 bounded states after 387,268,276
successful transitions:

```text
# phase 2: precomputed 533865 bounded states in 26.949s
```

With `-n 1`, the subsequent search took 0.001431 seconds.

For the normalized 30-letter bag

```text
aaaabdeeeeghhiilmnnrrrstttwwww
```

phase 1 produced:

```text
459162 entries, 166852 classes, 3054340 trie nodes
```

The sparse table again filled and was discarded. Before the follow-up changes,
the boundary message reported 32.975 seconds of precompute. It now reports:

```text
# phase 2: precomputed 524288 bounded states in 21.307s
```

That is a 35.4% reduction in discarded setup work, but it does not mitigate the
failure mode. A 40-second bounded validation run was stopped after the unpruned
search reached 543.1 million nodes and 70.1 million solutions.

Against the original seven-step measurement sequence, steps 1--6 are complete.
Step 7 confirmed exhaustion, while the policy change remains open.

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

No final output had yet been written. The current implementation makes the
doomed prepass faster, but still loses all score pruning when the table fills.

## Remaining work

1. Strengthen the floating-point justification and tests. In particular,
   document supported rounding/compiler assumptions and add adversarial tests
   around rounding boundaries and recursive depth.
2. Choose and validate a sparse-exhaustion policy: predict or avoid
   exhaustion, increase the bound budget, or retain a useful partial table
   instead of discarding all bounds.
3. Prototype a budgeted support-only candidate cache keyed by `bag_mask`, then
   compare its setup savings against its memory use and phase-2 search cost.
