# Plan: optimize the projected bottom-up score-bound kernel

## Outcome

Implement the three recommendations in
`findings/projected-score-optimization.md` against the bottom-up projected
evaluator in `source/dfs-search.cpp:1782`:

1. a runtime-dispatched SIMD implementation of the inner wildcard-update loop;
2. a split exact-support-mask sidecar, measured against the SIMD baseline; and
3. worker-local diagnostic counters in the hot loops.

Each change must preserve the upward-rounded score-bound invariant, produce a
bit-identical `bound_plain_float_values` table, keep the deterministic
preprocessing and final-search counters identical, and produce byte-identical
stdout. The accepted metric is phase-2 setup **plus** final search wall time;
a preprocessing win that weakens bounds is not a win.

Every phase leaves the tree buildable with a working scalar path, receives a
`/code-review` before commit, and states its own smoke checks. Tests stay
minimal per `CLAUDE.md`.

## Deviation from the findings' stated order

The findings recommend SIMD, then sidecar, then counters. This plan does
counters **first** (as phase 2), for one reason: recommendation 1 step 6
requires the SIMD kernel to "count finite lanes locally", so the kernel's
contract already presumes the local-accumulator refactor. Doing it first makes
the extracted kernel signature final, gives one clean independent measurement
of the counter change, and avoids re-touching the same lines twice. The
findings' relative-importance claim is unchanged: SIMD is still expected to
dominate, and counters are still measured on their own.

## Scope

### Included

- Worker-local `uint64_t` accumulators for candidate tests, fitting
  transitions, successful transitions, and `nextafter` calls inside one worker
  invocation of the bottom-up evaluator, aggregated once at the end.
- Extraction of the inner wildcard update into a standalone kernel with an
  explicit contract, keeping the scalar implementation as the portable
  baseline.
- An AVX2 kernel behind `__attribute__((target("avx2")))` with runtime
  `__builtin_cpu_supports()` dispatch, an env override, and a `verify` shadow
  mode that cross-checks SIMD against scalar bitwise.
- A contiguous `uint64_t` exact-support-mask sidecar parallel to
  `projected_actions`, used by the bottom-up action scan; and, if it measures
  well, removal of the now-duplicated `exact_support_mask` field from
  `ProjectedAction`. `alignas(16)` on that record is retained, so the record
  stays 48 bytes — see phase 4b for what that costs.
- One additional smoke test that exercises a projection with `d > 0` (the
  existing projected test at `source/test-dfs-search.cpp:384` runs `d=0`,
  which never enters the layered worker loop the SIMD kernel lives in).
- Baseline and A/B measurement records under `results/`, and a findings
  update recording what was measured.

### Excluded

- Anything in the "Non-targets" list of the findings: atomic table words, a
  viability mask, final per-state rounding, persistent workers, and
  wildcard-length support groups.
- Support-group indexing / submask enumeration (explicitly deferred by the
  findings), packed 16-bit score storage, and layer-list streaming. The last
  is a deep-memory viability change, not a `d=15` throughput change.
- Profiling runs. No hardware counters are available on this machine and the
  existing Callgrind profile in the findings is sufficient evidence of where
  the instructions go; decisions here are made on wall time.
- Raising the binary-wide ISA above `-march=x86-64-v2` (`source/meson.build`).
  GCC 14 does not vectorize this loop even at `-march=native`, so the ISA bump
  buys nothing and costs portability.
- Changing the recursive evaluator
  (`consider_projected_bound_candidate()`, `source/dfs-search.cpp:1662`) or the
  root scan. They are not leading costs; the root scan runs once.
- Vectorizing the exact-empty base vector loop
  (`source/dfs-search.cpp:1830`). It is loop-carried across `wild` and runs
  `score_wild_span` times total.
- Any `-ffast-math`, reassociation, or FMA-contraction change.

## Invariants every phase must preserve

1. **Bound direction.** `round_score_bound_up()` and
   `round_float_score_bound_up()` stay scalar, unchanged, and applied at the
   same points, so the established upward-bound proof carries over verbatim.
2. **Bit-identical arithmetic.** The kernel evaluates the same expressions in
   the same association order: `partial_score + child` for the score, and
   `((rounding_error_base + fabs(child)) + 1.0) * (DBL_EPSILON * 4.0)` for the
   error. IEEE double lanes with identical op order are bit-identical to the
   scalar sequence; there is no reduction reassociation because `max` is per
   `wild` element, not across lanes. No FMA fusion is possible in these
   expressions, and intrinsics prevent contraction regardless.
3. **Dead children are inert.** A `-HUGE_VAL` child must not affect `best`,
   must not affect `max_rounding_error` (it would poison it with `inf` through
   `fabs`), and must not increment the successful-transition counter.
4. **Counters.** `bound_candidate_tests`, `bound_fitting_transitions`,
   `bound_transitions`, and `bound_nextafter_calls` keep their exact final
   values and their unsigned overflow behavior. The test at
   `source/test-dfs-search.cpp:443` asserts
   `candidate_tests >= fitting_transitions >= transitions`.
5. **Determinism.** Results must not depend on worker count, layer scheduling,
   or which ISA path ran. One owner per exact bag/vector; only completed
   earlier layers are read.
6. **Portability.** The `x86-64-v2` binary must still run on a machine without
   AVX2, and non-x86 builds must compile the scalar path only. `werror=true`
   with `warning_level=3` (`source/meson.build`) means no unused-function or
   unused-parameter warnings on any target.

## Phase 1: baseline and A/B harness

No source change.

1. Confirm the machine is uncontended: `pgrep -a dfs-anagrams` must be empty
   (`CLAUDE.md` warns about concurrent sessions).
2. Build the optimized binary (`conan build .`). Record the build flags and
   the machine's ISA support (`/proc/cpuinfo` AVX2) alongside the numbers, so
   later A/Bs are comparable.
3. Record the reference run at the findings' workload — 40 letters, `-d 15`,
   `-n 1000`, `-C 256 -F -p 10000 -T 1`, `$IDX`, letters from `source ./s.sh`
   (`${S6:0:40}` matches the recorded stderr) — capturing:
   - phase-2 setup and search seconds and total wall time;
   - candidate / fitting / successful transition and `nextafter` counters;
   - final node, solution, spelling-expansion, and retained counts;
   - `sha256sum` of stdout.
4. Write a small A/B script in the scratchpad that runs a named binary,
   extracts those fields from stderr, and prints them next to the stdout hash.
   Use `-T 1` for kernel A/Bs (isolates the kernel from scheduling noise) and
   one multi-thread confirmation run per phase.
5. There is no hardware-counter tooling on this machine, so every decision
   rests on wall time plus the deterministic counters. That makes the
   uncontended-machine check the only defence against noise. One run per side
   is enough; the workload is long and the check for a competing
   `dfs-anagrams` process is what keeps it honest. Re-run only when a result
   lands close enough to the baseline that the sign is in doubt.
6. Also record a fast iteration workload (smaller letter count / `-n`) whose
   setup phase is long enough to rank changes but short enough to run per
   commit. Anything promising is re-confirmed on the full workload.

Deliverable: `results/projected-opt-baseline.{stdout,stderr}` and the
recorded field table. No commit needed beyond the results files.

## Phase 2: worker-local diagnostic counters (recommendation 3)

Target: the worker lambda at `source/dfs-search.cpp:1918`, specifically the
`++worker->candidate_tests` (line 1955), `++worker->fitting_transitions`
(1978), `++worker->transitions` (1985), and the `nextafter_calls` argument at
line 2008.

1. Declare four local `uint64_t` accumulators at the top of the `work()`
   lambda — one invocation, spanning every bag that worker drains from this
   layer's queue, not one per bag. Increment those in the loops.
2. Replace the per-`wild` `++worker->fitting_transitions` with one
   `local_fitting += score_wild_span - wild_length` per surviving action —
   the loop increments it unconditionally on every iteration, so the closed
   form is exact and removes a store from the hot loop.
3. Pass a pointer to the local `nextafter` accumulator into
   `round_score_bound_up()` in the publication loop.
4. Add all four into the `VectorWorker` fields once, immediately before the
   lambda returns. The fields stay cumulative across layers because the same
   `VectorWorker` is reused, so totals survive re-invocation; aggregation into
   `bound_*` at `source/dfs-search.cpp:2041` is unchanged.
5. Apply the same treatment to the exact-empty base loop
   (`source/dfs-search.cpp:1830`) only if it is free to do so; it is not hot.

Verification:
- `test-dfs-search` and `test-dfs-cli.sh` pass.
- On the fast workload, all four printed counters are identical to phase 1,
  and stdout hash is unchanged.
- Record the setup-time delta. The findings expect this to be small; report
  whatever it is, including a regression.

Commit: `Localize projected bottom-up counters`, after `/code-review`.

## Phase 3: masked SIMD wildcard updates (recommendation 1)

### 3a. Extract the scalar kernel (no behavior change)

Lift `source/dfs-search.cpp:1976-1996` into a file-static function in the
anonymous namespace:

```cpp
struct ProjectedWildUpdate {
  double partial_score;         // action.partial_score
  double rounding_error_base;   // action.rounding_error_base
  float const* children;        // values + base_key + wild_length - delta
  double* best;                 // worker->best + wild_length
  double* max_rounding_error;   // worker->max_rounding_error + wild_length
  size_t count;                 // score_wild_span - wild_length
};

// Returns the number of finite children, i.e. successful transitions.
static uint64_t projected_wild_update_scalar(ProjectedWildUpdate const& u);
```

The child index is `base_key + wild - score_key_delta` with `wild` running
contiguously, so `children` is a contiguous ascending float range — this is
what makes lane loads possible, and it is worth an explicit comment. Keep the
existing `assert(action.score_key_delta <= parent_key)` as a range assert on
the whole span before the call.

Verify: counters and stdout hash unchanged; setup time within noise. Commit:
`Extract the projected wildcard update kernel`.

### 3b. AVX2 kernel and runtime dispatch

Add, guarded by `#if defined(__x86_64__)` (`<immintrin.h>` is already included
at `source/dfs-search.cpp:21`):

```cpp
__attribute__((target("avx2")))
static uint64_t projected_wild_update_avx2(ProjectedWildUpdate const& u);
```

Per group of four:

1. `_mm_loadu_ps(children + i)` then `_mm256_cvtps_pd` to widen to four
   doubles. `float` -> `double` is exact.
2. `finite = _mm256_cmp_pd(child, neg_inf, _CMP_NEQ_OQ)`.
3. `candidate = _mm256_add_pd(partial_bcast, child)`;
   `err = _mm256_mul_pd(_mm256_add_pd(_mm256_add_pd(base_bcast, abs(child)),
   one), eps4)`, with `abs` as `_mm256_andnot_pd(sign_mask, child)` and
   `eps4 = DBL_EPSILON * 4.0` broadcast — same association order as the scalar
   code (invariant 2).
4. Blend both results against the current values under `finite`
   (`_mm256_blendv_pd`) before `_mm256_max_pd` with the loaded
   `best`/`max_rounding_error` lanes. `best` would in fact be safe without the
   blend because `partial_score` is finite and `-inf` loses the max, but the
   error term is not (`fabs(-inf) = inf`), so blend both and keep the code
   uniform.
5. `_mm256_storeu_pd` back (the `std::vector<double>` data is not guaranteed
   32-byte aligned and the range starts at `wild_length`, so unaligned
   load/store throughout).
6. `local_finite += __builtin_popcount(_mm256_movemask_pd(finite))`.

Handle the remainder (and any `count < 4`) with the scalar kernel — note
`score_wild_span` is 17 on the reference workload, so the tail is always
exercised.

Dispatch:

```cpp
typedef uint64_t (*ProjectedWildUpdateFn)(ProjectedWildUpdate const&);
static ProjectedWildUpdateFn projected_wild_update_impl();
```

resolved **once** in `compute_projected_score_bound_bottom_up()` before the
layer loop and passed to the workers as a plain function pointer — not
re-resolved per bag, and not a function-local static in the hot path.
Selection rules, following the existing env conventions
(`source/dfs-search.cpp:235-256`):

- `NUTRIMATIC_PROJECTED_SIMD` unset or `1`: auto — AVX2 if
  `__builtin_cpu_supports("avx2")`, else scalar. Land this phase with the
  default **off** (opt-in `1`) and flip it in 3d, mirroring how the bottom-up
  evaluator and projected cache were promoted (`f7cb1e6`).
- `0`: force scalar.
- `verify`: shadow mode, mirroring `NUTRIMATIC_LENGTH_CERTIFICATE=shadow`. Copy
  the `best` / `max_rounding_error` spans, run the SIMD kernel, re-run the
  scalar kernel on the copies, and `memcmp` both spans plus the returned
  counts. Report a diagnostic and fail preprocessing on mismatch. This mode is
  for tests and debugging, not for timing.

Auditable intrinsics are the default. GCC vector extensions
(`__attribute__((vector_size(32)))`) under the same target attribute are an
acceptable alternative if the intrinsic version turns out unreadable — but
only if the generated code is checked to be genuinely AVX2.

Do **not** add an SSE2 kernel now. The findings make it conditional on sharing
test coverage without obscuring the scalar path; revisit only if a target
machine without AVX2 matters.

Verify: `verify` mode clean on the fast workload and on the new `d > 0` test;
counters and stdout hash identical between `0` and `1`.

Commit: `Add an AVX2 projected wildcard update kernel`.

### 3c. Tests

Keep it to smoke coverage, but cover the kernel's edges directly rather than
hoping an end-to-end run happens to hit them. A single table-driven test that
calls the scalar and dispatched kernels on the same inputs and compares
`best`, `max_rounding_error`, and the returned finite count bitwise, over
these cases:

- all-finite, all-dead, and mixed children;
- `count` shorter than one lane group, exactly one group, and a group plus a
  tail (`score_wild_span` is 17 on the reference workload, so the tail path
  always runs in production);
- a non-zero starting `wild_length`, so the offset destination range is
  exercised;
- pre-seeded `best` / `max_rounding_error` values close enough to the
  candidates to change the max in one lane but not its neighbours.

This needs the kernel reachable from the test. Expose it through a small
internal test hook rather than widening the public `DfsAnagramSearch` API; if
that proves awkward, drop the direct test and rely on `verify` mode over the
end-to-end cases below, and say so in the commit message.

Then the end-to-end coverage:

- Extend the projected block in `source/test-dfs-search.cpp:384` with one run
  under `NUTRIMATIC_PROJECTED_SCORE_D` set to a small non-zero value so the
  layered exact-bag path (and therefore the kernel) actually runs, asserting
  identical retained spellings, nodes, solutions, and the existing counter
  ordering against the `d=0` projected run. Confirm during implementation that
  the synthetic test index (`make-dfs-test-index`) produces a non-trivial
  exact projection at that `d`; if it cannot, fall back to asserting the same
  equivalence through `source/test-dfs-cli-differential.sh`.
- One run of that same case with `NUTRIMATIC_PROJECTED_SIMD=verify`, plus one
  with `=0`, checking identical results. On a non-AVX2 machine both paths are
  scalar and the test degenerates harmlessly.

Commit: `Cover the projected SIMD kernel`.

### 3d. Measure and decide the default

Run the full A/B from phase 1 with `NUTRIMATIC_PROJECTED_SIMD` `0` vs `1`, on
an uncontended machine, single-thread for the kernel comparison plus one
multi-thread confirmation. Required evidence: setup seconds, search seconds,
**setup + search**, all counters, and stdout hash equality.

If setup + search improves and the hash matches, flip the default to auto in a
separate commit (`Enable the projected SIMD kernel by default`) with the
measurement recorded in the message. If it does not, keep it opt-in, record
the negative result in the findings, and continue to phase 4 anyway — the
sidecar is then measured against the scalar baseline instead.

## Phase 4: split exact-support-mask sidecar (recommendation 2)

Target: `source/dfs-search.cpp:1956`,
`if ((action.exact_support_mask & ~exact_mask) != 0) continue;`, which today
touches a 48-byte cold record to reject the large majority of scanned actions
at `d=15`.

### 4a. Add the sidecar, keep the field

1. Add `std::vector<uint64_t> projected_action_support` beside
   `projected_actions` (`source/dfs-search.h:311`).
2. Populate it in `prepare_projected_actions()` **after** the per-bucket sort
   at `source/dfs-search.cpp:713`, by copying `exact_support_mask` out of the
   sorted actions, so the two arrays stay index-parallel by construction. Clear
   it on every early-return path alongside `projected_actions`.
3. Keep `prepare_projected_actions()`'s existing `catch (...)` path clearing
   the sidecar together with `projected_actions` and the bucket starts, so a
   failed build can never leave a half-populated pair visible. Assert equal
   sizes once at the end of a successful build.
4. In the bottom-up scan, test `projected_action_support[action_index]` first
   and only bind `ProjectedAction const& action` after the test passes.
5. Gate the scan on a `NUTRIMATIC_PROJECTED_SUPPORT_SIDECAR` switch so 4a is a
   same-binary A/B (both layouts exist while the field is still present). The
   switch disappears in 4b, when the field does.
6. Measure. This step is pure layout: counters and hash must be identical.

### 4b. Drop the duplicated field, if 4a measures well

**`alignas(16)` on `ProjectedAction` stays.** That is a deliberate decision,
and it changes what 4b is worth: with the alignment retained, removing the
8-byte mask leaves 40 bytes of members that pad straight back to 48, so the
record does not shrink and the per-action footprint goes from 48 bytes to 56
(48-byte record plus the 8-byte sidecar) rather than staying flat as the
findings assumed. The cold-record load for a *surviving* action is therefore
unchanged, and every measurable density win comes from 4a. What is left in 4b
is a correctness/maintenance benefit — one source of truth for the mask — not
a throughput one.

So treat 4b as optional cleanup, sequenced after the 4a measurement:

1. Remove `exact_support_mask` from `ProjectedAction`
   (`source/dfs-search.h:157`), leaving `score_key_delta`, `partial_score`,
   `rounding_error_base`, `repeated_offset`, `packed_lengths`,
   `repeated_count`. Keep `alignas(16)`; keep the `static_assert` at
   `source/dfs-search.cpp:605` at `48` and reword its message to say the
   record is three 16-byte blocks with the last one partly free — a later
   field can land there without changing the footprint.
2. Change `projected_action_fits()` (`source/dfs-search.cpp:1400`) to take the
   action index, or an explicit mask argument, since it reads the mask at line
   1405. Both callers already have the index:
   `compute_projected_score_bound()` (line 1762) and the root scan (line 2072).
   Every remaining exact-support read — bottom-up, recursive, root — must go
   through the sidecar; there is no second source of truth left.
3. Check the reported action/table byte accounting still matches the real
   footprint, including the added sidecar.
4. Re-measure. Expect neutral; the purpose is to confirm no regression from
   the extra 8 bytes per action, not to find a win.

Keep 4a and 4b as separate commits so a regression can be bisected to the
layout change rather than the scan change. If 4a is neutral-to-negative, stop
there and skip 4b — without a scan win, paying 8 more bytes per action to
delete a field is not worth it.

If 4a does win and the extra footprint later looks like it is costing
something, revisiting `alignas(16)` is the lever that recovers the findings'
original 48-byte-flat outcome. That is a separate, separately measured
decision, out of scope here.

Verification for both: `test-dfs-search`, `test-dfs-cli.sh`, the bottom-up
versus recursive and quotient-on versus -off differentials in the new layout,
identical counters, identical stdout hash, and an uncontended long-input
timing — which the findings note has never been collected for this change.
Compare against the completed SIMD baseline at equal depth and thread count,
and include action-construction time; do not carry over the earlier top-down
8–15% figure as evidence.

Commits: `Split the projected exact-support mask` and `Shrink the projected
action record`.

## Phase 5: combined confirmation and write-up

1. Full-workload run with everything enabled, `-T 1` and the production thread
   count, against the phase-1 baseline: setup, search, setup + search,
   counters, stdout hash, and the final DFS node / solution /
   spelling-expansion / retained counts.
2. Update `findings/projected-score-optimization.md` (or add a companion
   findings file) with the measured outcome of each of the three changes,
   including negatives, and the final default for each env switch. Record the
   commands, build flags, thread counts, stdout hashes, deterministic
   counters, and a timing table — enough for a later session to reproduce the
   comparison without re-deriving the setup.

## Risks and mitigations

- **The kernel is memory-bound, not instruction-bound.** The findings' `Ir`
  profile cannot rule this out, and with no hardware counters available here
  we cannot settle it directly either. Wall time is the arbiter: if SIMD does
  not improve it, that is the answer; record it, keep the kernel opt-in, and
  prioritize phase 4 (which is a locality change) instead.
- **Target-attribute functions under LTO.** The build uses `b_lto=true`.
  GCC will not inline an AVX2 function into a non-AVX2 caller, which is the
  behavior we want, but confirm the AVX2 body actually gets AVX2 codegen
  (disassemble, or check that `vcvtps2pd` appears) rather than silently
  compiling to baseline SSE.
- **Illegal instruction on old hardware.** The dispatch must be reached before
  any AVX2 code executes and must never be constant-folded to the AVX2 path at
  the `x86-64-v2` baseline. Test the `0` path explicitly.
- **`werror=true` with `warning_level=3`.** Unused-function warnings for the
  AVX2 symbol on non-x86 builds must be prevented with the `#if` guard, not a
  pragma.
- **The sidecar is now additive, not free.** With `alignas(16)` retained the
  record cannot shrink, so the sidecar adds 8 bytes per action to the
  preprocessing footprint (about 1.2 MB at the reference workload's 151,440
  actions — negligible against the 28 MB score table, but no longer the
  break-even the findings described). If 4a does not win on wall time, the
  sidecar should not land at all.
- **Sidecar drift.** Two index-parallel arrays can desynchronize if a future
  change re-sorts actions. Building the sidecar strictly after the sort, in
  one place, plus an assert on equal sizes, is the mitigation.
- **Test index too small to exercise the layered path.** Covered by the
  fallback in 3c; without a `d > 0` case the SIMD kernel is untested by the
  unit tests.

## Measurement checklist (applied at every A/B)

- [ ] `pgrep -a dfs-anagrams` empty before starting
- [ ] Compare against the immediately preceding same-binary baseline
- [ ] phase-2 setup seconds, search seconds, and their sum
- [ ] candidate tests, fitting transitions, successful transitions,
      `nextafter` calls — all expected identical
- [ ] nodes, solutions, spellings expanded, retained
- [ ] `sha256sum` of stdout matches the baseline
- [ ] command, build flags, depth, and thread count recorded next to the
      numbers, so the row can be reproduced later
