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
reuse both the bound and candidate list for a remaining bag. This is why the
search can become extremely fast after a noticeable setup pause.

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

The CLI reports setup at the phase boundary:

```text
# phase 2: precomputed 143693 bounded states in 3.714s
```

and detailed counters after the search:

```text
# phase 2 timing: 3.714487 s setup, 0.399275 s search, 43424967 successful bound transitions, 345673 nextafter calls
```

One warm run of each reference workload measured:

| Normalized bag | Letters | States | Successful transitions | `nextafter()` calls | Precompute |
|---|---:|---:|---:|---:|---:|
| `aadeeefiimnorsstttu` | 19 | 18,380 | 2,744,344 | 44,121 | 0.189 s |
| `aabdeeefiimnorsstttu` | 20 | 34,845 | 7,489,507 | 84,441 | 0.508 s |
| `aaabeeeeghhiimnrrrsttwwww` | 25 | 143,693 | 43,424,967 | 345,673 | 3.714 s |

The 25-letter call count fell from 107,003,050 to 345,673. Stdout was
byte-identical to the pre-change binary for all three workloads. Dense and
sparse score-bound tests also compare retained spellings with an exhaustive
search, and the full indexed test suite passed.

For the normalized 30-letter bag

```text
aaaabdeeeeghhiilmnnrrrstttwwww
```

phase 1 produced:

```text
459162 entries, 166852 classes, 3054340 trie nodes
```

The sparse table again filled and was discarded. The boundary message reported
32.975 seconds of precompute before unpruned DFS began; the validation run was
stopped after 45 seconds. This confirms exhaustion, but does not mitigate it.

Against the original seven-step measurement sequence, steps 1--4 and 6 are
complete. Step 5's call recount is complete, but profiling the remaining setup
cost is not. Step 7 confirmed exhaustion, while the policy change remains open.

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

1. Profile the post-change precompute path to identify the remaining costs.
   The `nextafter()` recount is done; this is the unfinished part of the
   original step 5.
2. Strengthen the floating-point justification and tests. In particular,
   document supported rounding/compiler assumptions and add adversarial tests
   around rounding boundaries and recursive depth.
3. Choose and validate a sparse-exhaustion policy: predict or avoid
   exhaustion, increase the bound budget, or retain a useful partial table
   instead of discarding all bounds.
4. Run and record the corrected 28-letter workload measurement using the
   normalized bag `aaaabeeeeghhiilmnnrrrstttwww`.
