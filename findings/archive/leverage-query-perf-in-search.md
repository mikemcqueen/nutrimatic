# Leveraging recent `query-index` performance work in `dfs-anagrams`

## Summary

Some of the recent `query-index --require-completable` improvements transfer
directly to `dfs-anagrams`, but the existential and enumerating searches have
different stopping conditions:

- `query-index --require-completable` asks whether at least one completion
  exists after removing each candidate class. It may stop at the first
  witness.
- `dfs-anagrams`' concrete enumeration path must visit every relevant class
  path and expand its spellings. With `-n 0`, it must additionally retain,
  globally deduplicate, sort, and print every result.

The contiguous support-mask sidecar transfers directly, but the existing
`next_support_fit()` AVX2 interface does not. An initial port to concrete
enumeration produced no speedup and a possible small regression. The exact
validator has a 0.02% support-fit rate and asks for the next rare match;
concrete DFS has frequent matches and must consume all of them. Repeatedly
calling a routine that examines sixteen masks but returns only the first match
causes overlapping rescans and an out-of-line AVX2 call per match.

The concrete path instead needs an enumerating SIMD scanner: load a block
once, retain its match mask, and visit every set bit in original class order
before loading the next block. This technique was subsequently tried and
produced a modest 2–3% performance increase; the implementation lives on the
`support-mask-simd` branch.

A second low-risk transfer is to stop maintaining the projected score key
when score bounds are off. This is especially relevant to
`dfs-anagrams -n 0`, which explicitly disables score pruning but still updates
the key on every fitting transition.

For a larger `-n 0` improvement, a bounded fitting-candidate cache should be
reconsidered specifically for runs with score bounds off. A historical
prototype produced a 24.4x count-only search-core speedup. The current exact
bag signature, packed multiplicity data, and support-mask sidecar provide most
of the machinery needed for a simpler modern version.

## Terminology

In this document, **concrete enumeration path** means the production
`dfs-anagrams` traversal:

```text
DfsAnagramSearch::run()
    -> walk()
    -> visit_fitting_class()
    -> walk()
```

This is the live optimized implementation in `source/dfs-search.cpp`, around
`run()` at line 1603 and `walk()` at line 3415. It is not the reference
`walk_unoptimized()` implementation near line 3703.

The **exact validation path** is the separate existential search used by
`query-index --require-completable`:

```text
find_completable_classes()
    -> exact_remainder_completable()
    -> exact_expand_node()
```

`query-index.cpp` enters it through `find_completable_classes()` around line
424.

## Which improvements transfer

| Recent improvement | Transfer to concrete enumeration |
|---|---|
| Packed phase-1 class and member storage | Already shared by both programs |
| Packed phase-2 `FitClass` data | Already shared by both paths |
| Contiguous support-mask sidecar | Directly useful as a narrower scan stream |
| `next_support_fit()` AVX2 scan | Sparse existential design; direct concrete port rescans overlapping blocks |
| Enumerating AVX2 support-mask scan | Tried successfully: a 2–3% performance increase; implementation is on `support-mask-simd` |
| Skip score-key reads and updates with bounds off | Directly portable |
| Incremental exact mixed-radix key | Useful as a candidate-cache or reachability-cache key |
| Flat exact boolean memo | Only cached false results can prune enumeration directly |
| Successful witness reuse | Does not replace enumeration of all witnesses |
| Memo-aware exact lookahead | Existential-specific; it is designed to find one true child |
| Query result compaction and partial sorting | Does not remove DFS spelling expansion and global deduplication |

## 1. Adapt the support-mask SIMD scan

### Current exact-validation path

The exact path scans the standalone `class_supports` array through
`next_support_fit()`:

```cpp
uint64_t const* const supports = class_supports.get();
uint64_t const absent = ~worker->bag_mask;
auto const next_fit = [&](size_t from) {
  return next_support_fit(
      support_scan_vector, supports, from, end, absent);
};
for (size_t class_index = next_fit(begin); class_index < end;
     class_index = next_fit(class_index + 1)) {
  if (!hot_class_multiplicity_fits(
          uint32_t(class_index), *worker))
    continue;
  // ...
}
```

The AVX2 implementation examines sixteen contiguous 64-bit support masks per
outer iteration and returns the first fitting class. Returning the first fit
preserves the existing class order, but it also discards the other match bits
from the block.

On the 50-letter exact-validation benchmark documented in
`findings/exact-scan-simd.md`, the cumulative changes reduced exact validation
from 45.0 seconds to 7.2 seconds. The support-sidecar and AVX2 step itself
reduced the then-current 14.3-second version to 7.2 seconds.

### Initial concrete enumeration port

The initial port changed `walk()` from:

```cpp
for (size_t class_index = start; class_index < end; ++class_index) {
  uint32_t const id = uint32_t(class_index);
  if (!hot_class_fits(id, *worker)) continue;
  visit_fitting_class(
      worker, id, letters_left, representative_log_score, sink);
}
```

to the same `next_support_fit()` pattern as exact validation:

```cpp
uint64_t const* const supports = class_supports.get();
uint64_t const absent = ~worker->bag_mask;
auto const next_fit = [&](size_t from) {
  return next_support_fit(
      support_scan_vector, supports, from, end, absent);
};
for (size_t class_index = next_fit(start); class_index < end;
     class_index = next_fit(class_index + 1)) {
  uint32_t const id = uint32_t(class_index);
  if (!hot_class_multiplicity_fits(id, *worker)) continue;
  visit_fitting_class(
      worker, id, letters_left, representative_log_score, sink);
}
```

The corresponding change was made inside the scanned groups in
`walk_certified()`. Exact dense score-bound construction was deliberately left
unchanged; it is only selected with `-D` and was not exercised by the target
benchmark.

### Why the direct port does not transfer

The two paths have fundamentally different match density and stopping
behavior.

In the exact-validation benchmark, only about 0.02% of support masks fit. One
call to `next_support_fit_avx2()` normally scans a long run of absent-support
failures before returning. Its call prologue, AVX setup, and final
`vzeroupper` are amortized across thousands of candidates, and almost no
returned match is followed by another match in the same sixteen-mask block.

Concrete enumeration is much denser and must visit every fitting transition.
After one match is returned, the `for` loop calls `next_support_fit()` again
at the following class. The AVX2 routine reloads a sixteen-mask window, often
overlapping most of the window it just examined. It again computes all sixteen
match bits and again returns only the first.

Historical instrumentation in `findings/dfs-codex-perf.md` found that, among
concrete DFS candidate rejections:

- 79.75% first failed because a required symbol was absent; and
- 20.25% failed because a present symbol had insufficient multiplicity.

That rejection breakdown did not report the more important quantity for this
SIMD API: support-match density among all scanned candidates. On the current
50-letter concrete benchmark, 16.22 billion nodes were reached from 95.35
billion reported kept class scans, or roughly one multiplicity-fitting
transition per 5.9 scanned classes. Support matches are more frequent still
because some fail the repeated-count check.

At that density, a routine that reloads sixteen masks per returned support
match can examine the same masks several times. It also changes the memory
behavior:

- the old loop reads the support from the 16-byte `FitClass` stream;
- the direct port reads an 8-byte sidecar, but does so repeatedly for
  overlapping windows;
- every support match then loads its `FitClass` from a second stream for
  multiplicity and transition metadata; and
- the linked release binary contains an out-of-line AVX2 call from both
  `walk()` paths for every returned match.

With twenty search threads on the measured laptop, the extra load work,
function traffic, and possible AVX2 power/frequency effects can consume the
nominal four-lane advantage.

### Observed result

The initial before/after pair is:

- `results/dfs.s1.50.m4.1000.d15.stderr`;
- `results/dfs.s1.50.m4.1000.d15.o1-simd.stderr`.

Both runs selected exactly the same projected score bound:

- projection depth 15;
- 15,335,424 states;
- 61,341,696 bytes;
- 1,576,304 projected actions; and
- bottom-up AVX2 wildcard evaluation.

The baseline's explicit `-C 1024` and the SIMD run's default `-C 64` therefore
did not change the active table. The depth-15 table fits in 64 MiB; the larger
budget was unused.

| Measurement | Scalar `FitClass` scan | Direct SIMD port | Change |
|---|---:|---:|---:|
| Phase-2 setup | 32.1 s | 29.1 s | -9.3% |
| Phase-2 search | 137.9 s | 145.4 s | +5.4% |
| Process real time | 171.586 s | 177.274 s | +3.3% |
| Process user time | 3216.920 s | 3305.794 s | +2.8% |
| Kept class scans | 95.247 billion | 95.352 billion | +0.11% |
| Nodes | 16.21885 billion | 16.21884 billion | effectively unchanged |

The setup difference is run noise because the concrete scan change is not on
the projected-bound setup path. Parallel top-N scheduling also changed group,
solution, and spelling-expansion counters slightly, so one pair is not enough
to establish a precise regression. It is enough to reject the predicted
speedup, and the overlapping-window behavior explains why no gain appeared.

### Enumeration-oriented SIMD design

This design was implemented after the direct-port experiment. It avoids the
overlapping rescans described above and delivered a modest 2–3% performance
increase. The implementation is available on the `support-mask-simd` branch.

The concrete path should retain and drain every match bit from a loaded block:

```text
for each 16-class block in the current scan range:
    mask = support_fit_mask16(block, absent)
    while mask != 0:
        bit = ctz(mask)
        mask &= mask - 1
        class_index = block_begin + bit
        check repeated multiplicities
        visit the fitting class
scan the short tail scalar
```

This preserves:

- original class-index order;
- `entry_point`;
- length-certificate group boundaries;
- multiplicity validation; and
- deterministic tie-breaking.

It also reads each support mask once. A practical implementation should avoid
an AVX-target function call per match. Options include a scanner object with a
persistent pending mask and an out-of-line refill, or a helper that returns
match masks for a larger chunk. The scalar fallback can use the same
enumerating interface.

### Correctness

The direct port was correct because each call returned the first fitting class
at or after the requested position:

- class-index traversal order remains unchanged;
- `entry_point` remains enforced through the initial `start`;
- repeated-count requirements are still checked before visiting a class;
- length groups in `walk_certified()` remain intact; and
- output and deterministic tie-breaking remain unchanged.

The existing `NUTRIMATIC_SUPPORT_SIMD=0` switch supplies a useful scalar
sidecar A/B control, but it is not identical to the old scalar `FitClass`
loop: it scans the 8-byte sidecar and loads `FitClass` only for support
matches. Compare all three implementations when evaluating the redesign.

### Projected score bounds are orthogonal

Supplying `-d` or `-C` does not disable or defeat support scanning:

- `-C` is a memory budget and defaults to 64 MiB;
- `-d` chooses how many rarest letter types the projected score table keeps
  exact;
- the projected table prunes whole subtrees before candidate enumeration; and
- the support scanner filters individual class edges in nodes that survive.

The projected bound and the length certificate can greatly reduce total work,
which lowers the fraction of runtime any candidate scanner can improve. They
can also change the support-match distribution of the surviving ranges. That
is an Amdahl and density effect, not a reason to disable the projected cache.

`dfs-anagrams` can run without a projected score cache:

```text
dfs-anagrams ... -n 1000 -C 0 -F
```

This selects `SCORE_BOUND_OFF`; `-F` permits fallback instead of treating the
missing requested table as an error. A finite top-N run then has no remaining
score upper bound and must enumerate essentially every concrete completion,
although the independently prepared length certificate may still reject some
groups after the result heap fills. On large inputs such as the 50-letter
benchmark, removing the projected bound is likely to increase search time by
far more than a candidate scanner can recover.

`-n 0` already makes `DfsTopN::supports_score_pruning()` return false, so
score bounds are not requested regardless of the configured `-C` budget.
Count-only search with a null sink is also bounds-off. These exhaustive modes
are the natural first targets for a fitting-candidate cache.

## 2. Stop maintaining score keys when bounds are off

`DfsAnagramSearch::run()` requests score bounds only when its sink reports
that score pruning is supported:

```cpp
bool const score_bounds_requested =
    sink != NULL && sink->supports_score_pruning();
```

`DfsTopN::supports_score_pruning()` returns `result_limit != 0`. Therefore
`dfs-anagrams -n 0` runs with `SCORE_BOUND_OFF`.

Despite that, every non-terminal fitting transition in
`visit_fitting_class()` currently performs:

```cpp
worker->score_key -= score_key_deltas.get()[class_index];
// recurse or create a task
worker->score_key += score_key_deltas.get()[class_index];
```

The exact-validation path recently learned to skip score-key reads, updates,
and reachability probes when bounds are off. On its measured sequence, gating
score-key work reduced 15.6 seconds to 14.3 seconds.

The concrete path should similarly avoid score-key maintenance when
`score_bounds_active()` is false. This requires handling parallel task
creation coherently: an off-mode task does not need a meaningful score key,
but every on-mode task must retain the current behavior.

This optimization does not change the search tree. It removes random
`score_key_deltas` reads and integer updates whose result cannot be consumed.

## 3. Reconsider a fitting-candidate cache for bounds-off runs

### Why it matters

Concrete enumeration revisits the same remaining bag through many different
class paths. The remaining bag determines:

- the forced rarest symbol;
- the candidate bucket; and
- which classes fit.

It does not determine:

- `entry_point`;
- the current class path;
- accumulated score; or
- output state.

Historical measurements on the 19-letter `firestationteamused` workload found
70,608,083 concrete DFS nodes but only 18,379 distinct remaining bags. The
baseline performed 38.92 billion candidate tests.

A prototype cached the ordered fitting class IDs for each remaining bag, then
used `lower_bound(entry_point)` on every access. Its count-only search core
changed from:

| Mode | Time |
|---|---:|
| Scalar repeated scans | 80.40 s |
| Fitting-ID vector cache | 3.30 s |

That was a 24.4x speedup with about 39 MiB of additional peak RSS. A compact
bitset alternative reached 19.8x with about 10 MiB additional RSS.

Those are historical results. They predate the current packed class list,
projected score bounds, parallel concrete search, and the current `-n 0`
meaning of unlimited output. They establish the value of eliminating repeated
bag scans, not a current expected wall time.

### Why revisit it now

Candidate caching was removed when the score-bound cache became the sole
phase-2 cache. That tradeoff is sensible for ranked searches where the
projected score table consumes the budget and prunes most of the concrete
tree.

It is less compelling for `-n 0`:

- score pruning is disabled;
- the configured `-C` score-cache budget is consequently unused for bounds;
- concrete enumeration remains exhaustive; and
- repeated fitting-candidate scans remain in the hot path.

A modern cache could be enabled only when `bound_mode == SCORE_BOUND_OFF`,
avoiding competition with projected bounds and avoiding complexity in the
normal ranked path.

That is the lowest-risk first policy, not a fundamental restriction. The
50-letter depth-15 benchmark still reported about 95 billion kept class scans
after projected-bound and length-certificate pruning. A candidate cache may
therefore also help ranked searches, but it would compete with the score table
for memory and needs separate hit-rate and wall-time measurements.

### Machinery that can be reused

The current implementation already has:

- an exact collision-free mixed-radix signature on every class;
- `exact_root_key` for the full bag;
- an `exact_key` field in `SearchWorker`;
- a flat exact memo hashing scheme;
- packed repeated-count requirements;
- the contiguous support-mask sidecar; and
- the scalar and AVX2 kernels from which to build an enumerating scanner.

Concrete traversal does not currently update `worker->exact_key`, but it can
subtract and restore the selected class signature just as exact validation
does. Parallel `SearchTask` records would also need to preserve or reconstruct
the key.

On a candidate-cache miss:

1. Scan the complete forced-symbol bucket, independent of `entry_point`.
2. Use the enumeration-oriented support-mask scanner to skip support failures
   without reloading overlapping blocks.
3. Check repeated multiplicities.
4. Store fitting global class IDs in original order.

On a hit:

1. Find the first cached ID at or after `entry_point`.
2. Traverse the remaining IDs directly without fit tests.

### Required invariants

1. The key contains only the exact remaining bag.
2. Cache construction ignores `entry_point`.
3. Every use enforces `entry_point`.
4. Cached IDs retain original global class order.
5. Empty fitting lists are cacheable.
6. Concurrent publication exposes only complete lists.
7. Allocation, capacity, or arithmetic failures fall back to scanning.
8. A hard byte budget prevents unbounded storage on large state spaces.

## 4. Use exact memoization only as a negative pruning oracle

The flat exact memo, witness reuse, and memo-aware lookahead made
`query-index` much faster because existential search can return immediately
after proving that one completion exists.

Concrete enumeration cannot use a cached true result the same way. It still
must traverse every canonical completion below the state. In addition, the
exact validator ignores concrete traversal's `entry_point`, so an unrestricted
true result does not prove that a legal canonical suffix exists at a
particular concrete node.

A false result is different:

> If no completion exists even without an `entry_point` restriction, no
> canonical completion exists either.

Therefore a shared exact reachability memo could safely prune concrete nodes
whose remaining bag is known false. True entries would mean only "continue
enumerating."

Possible implementations include:

- a lazy two-bit dense state table when the exact mixed-radix state space fits;
- a bounded flat hash table using the current exact memo representation; or
- an existence prepass whose false results are retained for concrete search.

This should follow the support scan and candidate cache. Proving a state false
requires exploring its fitting transitions, so it benefits from both. It also
needs measurement: many concrete dead ends may be caused by `entry_point`,
while the unrestricted bag remains completable, and such states cannot be
pruned by a bag-only false memo.

## Improvements that do not transfer directly

### Successful witness reuse

One successful exact search can mark classes from its witness as completable,
allowing later top-level validations to be skipped. Full enumeration cannot
mark a subtree complete and skip it: the other witnesses are output.

### Memo-aware lookahead

Exact lookahead buffers unknown children while searching for a memoized true
child that can end the existential query. Concrete enumeration must eventually
visit every relevant child. Reordering concrete children can also change when
the top-N floor improves; prior score-based candidate ordering reduced nodes
but increased spelling expansion and wall time.

### Query output compaction and partial sorting

After exact validation, `query-index` retains each surviving index member once
and partially sorts a flat member array.

`dfs-anagrams` instead expands the Cartesian product of member spellings for
each class solution, deduplicates by word-set key, and retains either a global
top N or every result. The query output path cannot replace that work.

## The `-n 0` comparison is not like-for-like

`query-index -n 0 --require-completable` prints every individual index member
whose class has at least one completion. Its exact validation stops after one
witness per queried state.

`dfs-anagrams -n 0` means unlimited output. It:

1. enumerates every complete canonical class path;
2. expands every relevant class path into member spellings;
3. globally deduplicates word-set keys;
4. retains all surviving spellings in memory;
5. sorts all retained spellings; and
6. prints them.

Even a perfect candidate scanner cannot make those workloads equivalent. The
phase-2 timing diagnostic should be separated from total process wall time:

- if phase-2 search dominates, the support scan and candidate cache target the
  right work;
- if time after the phase-2 completion diagnostic dominates, unlimited
  spelling expansion, deduplication, sorting, or printing is the next problem.

Large unlimited runs may also be memory-bound because every retained result
stays resident until the final sort.

## Recommended implementation order

1. Replace the direct `next_support_fit()` concrete port with an
   enumeration-oriented scanner that drains every match bit from each loaded
   block.
2. Compare the old `FitClass` loop, scalar sidecar scan, and mask-draining
   AVX2 scan with identical command lines.
3. Skip score-key maintenance whenever score bounds are off.
4. Benchmark current `-n 0` with phase-2 time separated from output time.
5. Reintroduce a bounded fitting-candidate cache first for bounds-off runs.
6. Measure whether the candidate cache also repays its memory cost alongside
   projected bounds in ranked searches.
7. Prototype negative-only exact reachability pruning.
8. If output dominates, investigate worker-local/sharded unlimited retention,
   batched global merging, or an external/streaming sorting design.

## Benchmark requirements

Before every timing-sensitive run, verify from the host process table that no
other `query-index` or `dfs-anagrams` process is active.

For the SIMD scan:

- use the same binary, index, flags, and environment for every A/B run;
- alternate multiple runs of the mask-draining AVX2 mode and
  `NUTRIMATIC_SUPPORT_SIMD=0`;
- separately compare against the old scalar `FitClass` loop, because the
  scalar sidecar switch is not that baseline;
- require byte-identical stdout;
- use serial search when exact internal-counter equality is required;
- for parallel top-N, expect small scheduling-dependent differences in floor
  timing and internal counters, and compare repeated distributions rather
  than one pair;
- instrument support matches, SIMD blocks loaded, pending match bits consumed,
  scalar-tail candidates, and multiplicity fits;
- record phase-2 setup and search separately; and
- exercise both `-n 0` and a representative finite top N.

For candidate caching:

- compare cache off, a fitting dense configuration, and a deliberately
  exhausted bounded configuration;
- require identical ordered output and deterministic counters;
- test the same remaining bag reached with different `entry_point` values;
- record cache bytes and peak RSS; and
- measure a workload whose phase-2 search, rather than output printing,
  dominates.

## Conclusion

The recent query work is not merely adjacent to `dfs-anagrams`: the concrete
enumerator already allocates the new support-mask sidecar, and the narrower
stream remains promising. The exact validator's `next_support_fit()` loop,
however, is not directly portable. Its first-match interface is efficient
when matches are extremely sparse and the caller may stop at one witness; it
reloads overlapping blocks and incurs a call per match when concrete DFS must
enumerate a dense set of matches. The initial direct port produced no speedup
on the 50-letter depth-15 benchmark.

The existential memo improvements require adaptation rather than copying.
Their safe contribution to enumeration is negative reachability, not witness
short-circuiting. The support-sidecar equivalent is a mask-draining enumerator
that consumes all matches from each loaded block in original order.

Projected score bounds should remain enabled for large finite top-N searches:
they prune whole subtrees and are why those searches are tractable. For
exhaustive `-n 0` and count-only modes, the larger opportunity is likely a
bounded per-bag fitting-candidate cache using the exact signature and the
enumerating SIMD scanner together. Negative-only exact reachability can then
prune bags with no unrestricted completion. If phase-2 search ceases to
dominate, the next work belongs in unlimited spelling expansion, retention,
and sorting rather than in the candidate scanner.
