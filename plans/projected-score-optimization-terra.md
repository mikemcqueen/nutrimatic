# Plan: optimize projected score-bound preprocessing

## Outcome

Reduce total phase-2 wall time for complete projected score tables while
preserving exact search behavior. The work targets the bottom-up evaluator in
`source/dfs-search.cpp`, specifically
`DfsAnagramSearch::compute_projected_score_bound_bottom_up()`.

Implement these changes in order:

1. worker-local diagnostic counters;
2. a scalar helper boundary followed by runtime-dispatched masked SIMD
   wildcard updates; and
3. a split exact-support-mask/cold-action layout.

The first change is a small cleanup and measurement baseline. SIMD is the
primary throughput experiment. The sidecar is a separate action-layout A/B
that must be evaluated against the SIMD result rather than the old scalar
layout.

## Constraints and invariants

- Preserve the projected recurrence and action quotient.
- Keep `float` score-table storage for complete bottom-up tables.
- Do not enable `__FAST_MATH__`, reassociation, or a reduced rounding model.
- A stored score bound must remain an upward bound on the exact recurrence.
  Preserve the final `round_score_bound_up()` followed by
  `round_float_score_bound_up()` path.
- A dead child (`-HUGE_VAL`) must make no contribution to either maximum.
- Preserve candidate-test, fitting-transition, successful-transition, and
  `nextafter` counters exactly.
- Retain scalar execution as the portable and differential baseline.
- Keep tests focused smoke coverage; performance claims require uncontended
  A/B measurements rather than unit-test timing.

## Phase 0: baseline and instrumentation

1. Record the exact command, build configuration, input, depth, thread
   count, output hash, phase-2 counters, and elapsed time used for each A/B.
2. Check for an existing `dfs-anagrams` process before a timing run.
3. Retain the Callgrind instruction profile as work evidence. If PMU counters
   are available, collect cycles, instructions, branch misses, and cache
   misses with `perf stat`.
4. Once SIMD exists, provide an internal scalar opt-out for the same-binary
   differential, consistent with current projected-evaluator validation
   switches.

Acceptance: baseline output/counters are captured; this phase changes no
runtime behavior.

## Phase 1: worker-local diagnostic counters

### Design

`VectorWorker` owns cumulative counters across layers, but the hot loops do
not need to increment its fields directly. At the start of each
`work(worker_index)` call, use local counters:

```cpp
uint64_t candidate_tests = 0;
uint64_t fitting_transitions = 0;
uint64_t transitions = 0;
uint64_t nextafter_calls = 0;
```

Increment these locals in the loops, pass `&nextafter_calls` to rounding, and
add each local once to the worker field when the worker drains the layer.
This preserves cumulative totals when the same worker is reused on later
layers.

### Steps

1. Add local counters at the top of the work lambda.
2. Replace per-event `worker->` increments with local increments.
3. Add local totals to the worker immediately before returning from the
   lambda.
4. Leave wildcard-only and root counters in their existing outer scopes.
5. Build and run the narrow DFS-search smoke test.
6. Run an output/counter differential before and after the change.

Acceptance: stdout hash and all deterministic counters match; no data race is
introduced; this remains independent of SIMD and sidecar work.

## Phase 2: factor the scalar wildcard-update operation

### Design

Extract the inner wildcard-update range into a scalar helper with explicit:

- child `float` vector pointer;
- wildcard range;
- action partial score and fixed rounding-error term;
- `best` and `max_rounding_error` vectors; and
- local successful-transition counter.

The helper must retain the current operation order. It must not move the
`fabs(child) + 1` component out of the lane, and a dead child must leave both
destination vectors unchanged. Use pointer bases for the action-specific child
range and destination ranges where that makes contiguity clear.

### Steps

1. Add a private/static helper in `dfs-search.cpp`; do not widen the public
   API.
2. Route the bottom-up action loop through it.
3. Preserve the fitting counter’s position before the dead-child check.
4. Add a focused test with finite and dead children, including an offset
   wildcard range.
5. Run the existing bottom-up-versus-recursive differential.

Acceptance: scalar behavior matches Phase 1; the normal baseline target has
no new ISA requirement.

## Phase 3: runtime-dispatched masked SIMD updater

### Design

Keep the scalar helper as the reference path. Add an AVX2 target clone on
supporting CPUs without globally raising the program’s `x86-64-v2` baseline.
For four adjacent wildcard counts, the SIMD helper should:

1. load contiguous child floats and widen them to doubles;
2. build a finite-child mask;
3. form score candidates from `partial_score + child`;
4. form rounding-error candidates using the existing base, `fabs(child)`,
   one, and `DBL_EPSILON * 4`;
5. masked-max `best` and `max_rounding_error`, retaining old values for dead
   lanes; and
6. accumulate the finite-lane count locally.

Handle short prefixes and tails scalarly. Use unaligned loads unless an
alignment proof is explicit. Do not use fast-math or instructions/flags that
alter infinity, NaN, comparison, or reassociation semantics.

### Steps

1. Establish compiler-guarded target attributes and runtime CPU-feature
   detection for supported GCC/Clang builds.
2. Implement the AVX2 helper using auditable intrinsics or vector extensions.
3. Initialize the dispatcher once rather than checking CPU features per
   action.
4. Route the Phase-2 scalar helper boundary through that dispatcher.
5. Test all-finite, all-dead, mixed, short, exact-lane-width, and tail
   vectors, with values close enough to change either maximum.
6. Compare scalar and SIMD table payloads where bit equality is expected;
   otherwise require equal output and all counters and document why the bound
   remains upward-safe.
7. Run projected bottom-up versus recursive differentials with SIMD enabled
   and forced off.

### Measurement and decision

Run same-binary scalar-versus-SIMD A/Bs on the established uncontended
long-input workload. Record phase-2 setup, total elapsed time, hash, and all
counters. Choose by setup plus final search, not instruction count alone.

Acceptance: scalar fallback is correct everywhere; SIMD has identical output
and counters. If it regresses total time, keep only the scalar refactor and
record the result rather than enabling SIMD by default.

## Phase 4: split exact-support-mask sidecar

### Design

Move `ProjectedAction::exact_support_mask` into a contiguous `uint64_t`
sidecar ordered exactly like `projected_actions`. Remove it from the cold
record and target a 40-byte, eight-byte-aligned action record; including the
8-byte sidecar should retain the current 48-byte-per-action footprint.

The scan becomes:

```cpp
if ((projected_exact_supports[action_index] & ~exact_mask) != 0)
  continue;
ProjectedAction const& action = projected_actions[action_index];
```

This prevents rejected actions from pulling score, delta, and repeated-
requirement fields into the hot scan. The recursive projected fallback, root
calculation, and all diagnostics must use the same sidecar or an unambiguous
compatibility accessor.

### Steps

1. Add sidecar ownership to `DfsAnagramSearch`, clear it alongside projected
   action state, and populate it in `prepare_projected_actions()`.
2. Update size/alignment assertions and allocation accounting; verify the
   cold record’s actual layout.
3. Replace every exact-support read in bottom-up, recursive, and root paths.
4. Ensure exception paths cannot expose a partially built action/sidecar pair.
5. Keep an internal plain-versus-sidecar layout switch for same-binary A/B.
6. Run quotient-on/off and bottom-up/recursive differentials in both layouts.

### Measurement and decision

Compare the completed SIMD baseline with the sidecar at equal depth and thread
count, including action construction. Do not infer a result from earlier
top-down measurements. If it wins on total wall time, make it default;
otherwise do not advance directly to support grouping.

## Phase 5: review and documentation

Before each commit, run `/review` as required by the repository. Keep phases
separately buildable and reviewable. Record commands, hardware/build flags,
hashes, deterministic counters, and timing tables in the findings document.

Defer full support groups, wildcard-length grouping, viability masks,
persistent workers, packed binary16, and layer-list streaming unless later
profiles change the evidence. The latter is a deep-memory viability change,
not a leading `d=15` throughput optimization.

## Focused validation matrix

For each phase:

1. build with the repository’s Conan/Meson environment;
2. run the narrow existing DFS test target;
3. run one projected bottom-up smoke case and the recursive evaluator opt-out;
4. run the scalar/SIMD or plain/sidecar differential relevant to the phase;
5. only for performance A/Bs, compare the representative long-workload stdout
   hash and all deterministic counters.

Do not add broad coverage solely for these internal representation changes.
