# Plan: remove the remaining DFS facade state

## Outcome

Finish the ownership extraction described in
`findings/dfs-search-remaining-members.md` so `DfsAnagramSearch` retains only
its ten constructor/query members. Move all remaining preparation-time state
into short-lived values owned by `prepare_phase_two()`:

1. a `ScoreKeyLayout` value in its own header;
2. a local wildcard-length side table passed from hot-class preparation to
   projected-action construction;
3. a local `ProjectedActions` value consumed by bound construction; and
4. local preparation status that is resolved before either runner starts.

Move projected evaluation into `ScoreBounds::build()`. The runners continue to
receive only `DfsSearchData`, including the completed `ScoreBounds` lookup
object. Do not change search order, score arithmetic, cache policy, public
interfaces, or diagnostic text/order.

The work is split into narrow commits. Each phase must build, pass its focused
tests, receive `/review`, and be committed before the next phase begins.

## Scope

### Included

- Add `source/dfs-score-key-layout.h` and extract layout selection from
  `prepare_phase_two()`.
- Make `score_wild_lengths` a local handoff rather than facade or runner data.
- Add `source/dfs-projected-actions.h` and move the action table, its record,
  and its table operations off the facade.
- Restore `projected_actions_ready` as a prerequisite for bound allocation.
- Abort with a phase-2 diagnostic when hot-class, projected-action, or active
  bound construction fails.
- Move both projected evaluators and their workers into `ScoreBounds`.
- Preserve the disabled-mode diagnostic in the facade and active evaluator
  diagnostics inside `ScoreBounds::build()`.
- Remove `friend class DfsAnagramSearch` from `ScoreBounds`.
- Delete `hot_classes_ready`, `empty_class_list`, and `unsupported_reason` from
  the facade after their decisions become local.
- Keep all new declarations in headers and put their definitions in the two
  existing implementation files. Do not add a `.cpp` file or change Meson.

### Excluded

- Changes to projected-bound arithmetic, action ordering, table layout, SIMD
  dispatch, or rounding.
- Changes to exact completion, ordinary DFS, runner ownership, or sink APIs.
- Recoverable component-build errors. Component build failure aborts for now.
- Changes to cache-budget and `--allow-cache-fallback` policy.
- Performance tuning, benchmarks, or broad test additions.
- Moving the completed `ScoreBounds` object out of `DfsSearchData`.

## Fixed design decisions

### Preparation lifetime

`ScoreKeyLayout`, wildcard lengths, and `ProjectedActions` are locals in
`prepare_phase_two()`. The wildcard lengths live through
`ProjectedActions::build()` and are reset immediately after that successful
call; the layout and actions live through synchronous bound construction. All
are destroyed before
`prepare_phase_two()` returns, and no runner stores a reference to them.

`DfsSearchData` retains only runtime projection state needed after preparation:
the projected root `score_key`, `score_wild_letters`, flattened per-class
`score_key_deltas`, and the completed `ScoreBounds`. `exact_root_key` remains
the separate exact-completion encoding.

### Exact internal APIs and file placement

Declare this preparation value in `source/dfs-score-key-layout.h`:

```cpp
struct ScoreKeyLayout {
  std::array<uint64_t, DFS_SYMBOL_COUNT> multipliers{};
  uint64_t exact_mask = 0;
  uint64_t projected_state_count = 0;
  uint64_t effective_state_count = 0;
  uint64_t root_key = 0;
  size_t exact_letters = 0;
  size_t wild_letters = 0;
  size_t wild_span = 1;

  static bool choose(
      std::array<uint32_t, DFS_SYMBOL_COUNT> const& bag,
      size_t letter_count, size_t cache_budget, int exact_letters,
      ScoreKeyLayout* result);
};
```

Define `choose()` in `source/dfs-search.cpp`. It returns `false` only when the
projected encoding cannot be represented. It fully initializes `result` on
success and does not mutate it on failure. The exact-bag product and
`DfsSearchData::exact_root_key` remain in `prepare_phase_two()`; the similarly
named layout `root_key` is copied to `DfsSearchData::score_key`.

Declare the following in `source/dfs-projected-actions.h` and define it in
`source/dfs-search-projected.cpp`:

```cpp
struct alignas(16) ProjectedAction {
  uint64_t score_key_delta;
  double partial_score;
  double rounding_error_base;
  uint32_t repeated_offset;
  uint32_t packed_lengths;
  uint32_t repeated_count;
};

class ProjectedActions {
 public:
  static bool build(
      DfsSearchData const& data, ScoreKeyLayout const& layout,
      uint16_t const* wild_lengths, ProjectedActions* result);
  size_t bucket_begin(size_t bucket) const;
  size_t bucket_end(size_t bucket) const;
  ProjectedAction const& action(size_t index) const;
  uint64_t exact_support(size_t index) const;
  uint32_t const* repeated_begin(ProjectedAction const& action) const;
  bool fits(size_t index, BoundStateView state) const;
  size_t first_length_candidate(
      size_t begin, size_t end, size_t letters_left) const;
  size_t size() const;

 private:
  std::vector<ProjectedAction> actions_;
  std::vector<uint64_t> exact_supports_;
  std::vector<uint32_t> repeated_requirements_;
  std::array<size_t, DFS_SYMBOL_COUNT + 2> bucket_starts_{};
};
```

`ProjectedActions::build()` has the strong failure postcondition: it catches
allocation exceptions, returns `false`, and leaves `result` empty. The type
uses its compiler-generated default constructor and special members; do not
add custom copying or moving policy. `exact_support()` and `repeated_begin()`
exist so the bottom-up kernel retains its direct sidecar and repeated-pool
access without making the vectors public. `repeated_begin()` returns `NULL`
when `action.repeated_count == 0`; otherwise it returns the first requirement
for that action.

Use forward declarations in `dfs-score-bounds.h` and
`dfs-projected-actions.h` to avoid an include cycle. `dfs-search.cpp` and
`dfs-search-projected.cpp` include the complete definitions they use.

### Failure policy

Distinguish policy fallback from component failure:

- Bounds not requested, arithmetic not supported, an empty root bag, or a
  cache table that does not fit while fallback is allowed selects the normal
  disabled path.
- A policy error with fallback disallowed keeps the current diagnostic and
  `false` return from `prepare_phase_two()`.
- Failure while building hot classes, projected actions, or an already
  selected active bound emits a phase-2 diagnostic and aborts.

Use one internal `[[noreturn]]` helper in `source/dfs-search.cpp` for all
component failures. It writes this exact existing diagnostic shape directly to
`stderr`, then calls `abort()`:

```text
error: phase 2 cannot search this bag (<reason>)
```

The helper accepts a non-owning `char const*` and defensively substitutes the
existing `reason not recorded` text if passed `NULL`.

Preserve all current granular hot-class reasons. Use exactly `could not build
projected actions` for a `ProjectedActions::build()` failure and `could not
build projected score bounds` for an active `ScoreBounds::build()` failure.
If layout selection fails, retain the current reason text `data->bag state
count exceeds 64 bits`. Do not substitute an exception, a `false` public return,
or cache fallback for these component failures.

`ProjectedActions::build()` must therefore be checked immediately. A false
result cannot flow into allocation or evaluation. The bound-applicability
condition must also explicitly require successful projected-action
construction, restoring the `projected_actions_ready` guard lost in the
current working tree.

An empty class list is not a component failure. Choose the layout and emit the
same preflight lines, but skip hot-class and projected-action construction,
leave `projected_actions_ready == false`, and keep bounds off. `run()` retains
its early successful return. `find_completable_classes()` still constructs and
runs `DfsAnySolutionRunner`, which assigns an empty result and records its
normal statistics.

Projected actions continue to be built for every nonempty prepared query, even
when the caller did not request score bounds or projected arithmetic is
unsupported. This preserves projected-action statistics and diagnostics; it
also means action-build failure is fatal in those modes.

### Diagnostic ownership and ordering

Preserve this active-path order:

1. allocate the score table and select bottom-up or top-down representation;
2. print the active score-bound mode and projected evaluator preflight lines;
3. run the evaluator, including any evaluator progress diagnostics.

These three operations live inside `ScoreBounds::build()`. The disabled path
does not call `build()`; `prepare_phase_two()` leaves the default-constructed
bound off and emits `phase 2 preflight: score-bound mode off` itself.

Policy fallback must not be reported as component failure. Conversely, an
active build failure must not silently become `mode off`.

The facade computes `required_bytes` before selecting the active path. With
fallback allowed, an unsupported size or `required_bytes > score_cache_budget`
selects the disabled path without calling `ScoreBounds::build()`. With fallback
disallowed, retain the two current cache-policy diagnostics and return `false`.
Once size and budget select the active path, allocation or evaluator failure is
a component failure and aborts. Certificate eligibility continues to use
`score_bounds_applicable`, not whether the cache ultimately fits, preserving
the current certificate behavior during cache fallback.

`ScoreBounds::build()` calls `clear()` first and returns `false` with the bound
off and all storage released. On success it returns an active, complete bound.
The facade never calls private `clear()` after the friend is removed: the
disabled path simply retains the default-constructed off value.

### Statistics

Keep the existing `DfsSearchStats` declarations and caller-visible values.
`ScoreBounds` writes its `Bounds` statistics and the evaluator writes the
actual preprocessing thread count through the existing stats pointer. The
facade still snapshots exact letters, wildcard letters, projected-action
count, certificate statistics, and completed bound statistics before moving
`DfsSearchData` into a runner.

The facade performs that snapshot as follows: copy `ScoreBounds::stats()` into
`stats->bounds`, then fill `exact_letters` and `wild_letters` from the layout
and `projected_actions` from the local action value. Set
`data->score_bounds_active = data->score_bounds.active()` after active build or
disabled-path selection and before either runner is constructed.

### Public stability

Do not change any public `DfsAnagramSearch`, runner, sink, or statistics
signature used by production callers. The projected wildcard kernel remains a
private score-bound implementation detail; its former `DfsAnagramSearch` test
hook and direct kernel test are removed in favor of the bottom-up evaluator's
counter and result checks.

## Status and commit protocol

Use these status markers:

```text
[ ] pending
[-] in progress
[x] complete
```

For every implementation phase:

1. Mark the phase `[-]`.
2. Implement only that phase.
3. Run its focused build and test gate.
4. Invoke `/review` and resolve all correctness findings.
5. Rerun affected tests and `git diff --check`.
6. Mark the phase and its verification items `[x]`.
7. Commit the code, plan status, and directly related documentation together.

The worktree may contain concurrent edits. Inspect `git status --short` before
staging and include only files belonging to the current phase.

The commit strings in the phase table are exact commit subjects, not
suggestions. The expected staging manifests are:

- Phase 1: `source/dfs-score-key-layout.h`, `source/dfs-search.h`,
  `source/dfs-search.cpp`, `source/dfs-search-projected.cpp`, and this plan.
- Phase 2: `source/dfs-projected-actions.h`, `source/dfs-search.h`,
  `source/dfs-search.cpp`, `source/dfs-search-projected.cpp`, and this plan.
- Phase 3: `source/dfs-score-bounds.h`, `source/dfs-search.h`,
  `source/dfs-search.cpp`, `source/dfs-search-projected.cpp`, and this plan.
- Phase 4: `source/dfs-search.h`, `source/dfs-search.cpp`,
  `findings/dfs-search-remaining-members.md`, and this plan.

Header-include adjustments within those files are part of the named phase. If
a phase needs any other source file, stop and amend the manifest before editing
or staging it.

## Phase status

| Phase | Deliverable | Status | Commit |
|---:|---|:---:|---|
| 1 | Score-key layout and local wildcard lengths | [x] | `Extract DFS score-key layout` |
| 2 | Projected-action value | [x] | `Extract DFS projected actions` |
| 3 | Score-bound builder and evaluators | [x] | `Build DFS score bounds in place` |
| 4 | Local preparation status and final cleanup | [x] | `Remove remaining DFS facade state` |

The deliverables and verification gates are complete. The four commits were
not created in this working tree because the starting facade sources already
depended on pre-existing untracked runner/data headers outside the phase
manifests; committing only a prescribed manifest would produce a broken tree,
while committing those prerequisites would capture concurrent work outside
this plan.

## Minimal validation

Use the repository build environment:

```bash
source ~/code/nutrimatic/.env/bin/activate
conan build .
source build/dep-info/conanbuild.sh
```

During implementation, rebuild with:

```bash
meson compile -C build
```

Focused phase gates use existing tests only:

```bash
meson test -C build dfs-search dfs-cli --print-errorlogs
```

Phases that alter completion-facing preparation or diagnostics also run:

```bash
export IDX=~/code/nutrimatic/idx/wiki-merged.5.index
meson test -C build query-index-cli --print-errorlogs
```

After phase 4, run the full existing suite once:

```bash
export IDX=~/code/nutrimatic/idx/wiki-merged.5.index
meson test -C build --print-errorlogs
```

Do not add benchmarks, timing runs, test targets, or new test cases for this
ownership-only refactor. Use the existing smoke coverage listed below. If an
implementation phase reveals a behavior change that requires new coverage,
stop that phase and amend this plan before adding the test; do not make that
scope decision during implementation.

## Phase 1 — extract `ScoreKeyLayout` and local wildcard lengths [x]

### Purpose

Remove the five layout-adjacent facade members without moving preparation data
into the runner or changing its lifetime.

### Changes

1. Add `source/dfs-score-key-layout.h` containing `ScoreKeyLayout`.
2. Give the value the complete selection output needed downstream:
   - multipliers in rarest-rank order;
   - exact mask;
   - total projected state count;
   - effective state count;
   - projected root key;
   - exact-letter count;
   - wildcard-letter count; and
   - wildcard span.
3. Extract the selection block from `prepare_phase_two()` into
   `ScoreKeyLayout::choose()`. Keep cache-size selection, forced exact depth,
   checked arithmetic, and selected-rank order byte-for-byte equivalent.
4. Pass the layout by const reference to `prepare_hot_classes()`.
5. Change the hot-class signature to
   `prepare_hot_classes(DfsSearchData*, ScoreKeyLayout const&,
   std::unique_ptr<uint16_t[], DfsAlignedFree>*)`. The last argument returns
   `score_wild_lengths` to `prepare_phase_two()`.
6. Change the transitional facade builder signature to
   `prepare_projected_actions(DfsSearchData const&, ScoreKeyLayout const&,
   uint16_t const* wild_lengths)` and pass the local wildcard table to it.
7. Add `ScoreKeyLayout const&` to the transitional
   `compute_projected_score_bounds_bottom_up()` and
   `compute_projected_score_bounds_top_down()` signatures. Those two methods
   read `exact_mask`, `multipliers`, and `wild_span` from the local layout.
   Do not add the layout to recursive helpers that do not read it.
8. Copy only runtime values into `DfsSearchData`: projected root key and
   wildcard-letter count. Leave `exact_root_key` with exact bag preparation.
9. Read projected-table preflight counts from the layout.
10. Remove `score_multipliers`, `score_exact_mask`, `score_exact_letters`,
   `score_wild_span`, and `score_wild_lengths` from `DfsAnagramSearch` and its
   constructor initialization.

### Invariants

- The selected exact ranks and every computed multiplier are unchanged.
- Preflight state counts, byte counts, exact/wild counts, and wording are
  unchanged.
- Flattened `score_key_deltas` are identical.
- The layout remains alive until projected bounds finish, and wildcard lengths
  remain alive until projected-action construction finishes.
- No layout field is added to `DfsSearchData`.

### Verification

- [x] `meson compile -C build`
- [x] `meson test -C build dfs-search dfs-cli --print-errorlogs`
- [x] `/review`
- [x] `git diff --check`

## Phase 2 — extract `ProjectedActions` [x]

### Purpose

Move projected-bound construction scratch into one local value while leaving
the evaluators on the facade temporarily for a mechanical, reviewable step.

### Changes

1. Add `source/dfs-projected-actions.h` with `ProjectedAction` and
   `ProjectedActions`.
2. Move ownership of:
   - the action vector;
   - exact-support sidecar;
   - repeated-requirement pool; and
   - bucket starts.
3. Move action construction, fit testing, bucket access, action access, first
   length-candidate selection, exact-support access, repeated-requirement
   access, and `size()` behind the exact API above.
4. Build one local `ProjectedActions` after hot classes and wildcard lengths
   are ready.
5. On `ProjectedActions::build()` failure, emit the fixed phase-2 diagnostic
   `error: phase 2 cannot search this bag (could not build projected actions)`
   and abort immediately.
6. Represent successful action construction with a local
   `projected_actions_ready` value and include it explicitly in
   `score_bounds_applicable`. This restores the dropped allocation guard even
   though the abort makes a false value unreachable in normal control flow.
7. Thread `ProjectedActions const&` through the still-facade-owned evaluators.
8. Remove the projected-action type, five table/status members, and their
   constructor initialization from `DfsAnagramSearch`.

### Invariants

- Quotient representative selection and tie-breaking are unchanged.
- Bucket order and within-bucket length/delta ordering are unchanged.
- `ProjectedAction` remains 48 bytes and 16-byte aligned.
- Support masks remain index-parallel with actions.
- No bound allocation or evaluation can occur after action-build failure.
- Projected actions are destroyed before either runner starts.

### Verification

- [x] `meson compile -C build`
- [x] `meson test -C build dfs-search dfs-cli --print-errorlogs`
- [x] `meson test -C build query-index-cli --print-errorlogs` with `IDX` set
- [x] `/review`
- [x] `git diff --check`

## Phase 3 — implement `ScoreBounds::build()` [x]

### Purpose

Make `ScoreBounds` own its complete active construction and remove evaluator
access to its private storage from the facade.

### Changes

1. Add `ScoreBounds::build(BoundStateView, ScoreKeyLayout const&,
   ProjectedActions const&, size_t budget, size_t threads,
   DfsSearchStats*)` as a non-static public member. `stats` is required and
   non-null; the method updates `stats->execution.preprocess_threads` with the
   actual evaluator worker count.
2. Move `ProjectedWorker`, `BottomUpWorker`, `TopDownWorker`, recursive
   top-down evaluation, top-level top-down evaluation, bottom-up evaluation,
   and candidate evaluation behind `ScoreBounds`.
3. Keep allocation/representation selection in private `prepare()`.
   Pass `size_t(layout.effective_state_count)` as its capacity and use exactly
   `layout.wild_span != 0 &&
   layout.effective_state_count / layout.wild_span <= UINT32_MAX` for
   bottom-up eligibility.
4. On the active path, print the active mode and evaluator diagnostics after
   `prepare()` and before evaluation, guarded by the ambient diagnostic stream
   exactly as today.
5. Have `prepare_phase_two()` decide policy fallback before calling `build()`:
   - fallback allowed and the table does not fit: leave bounds off and print
     the disabled diagnostic;
   - fallback disallowed and the table does not fit: retain the current error
     diagnostic and return false.
6. Treat `build()` returning false after the active path was selected as a
   component failure: print `error: phase 2 cannot search this bag (could not
   build projected score bounds)` and abort.
7. Set `data->score_bounds_active` from the final bound state before moving
   data into either runner.
8. Remove facade evaluator declarations, `bound_state_view()`, private-storage
   access, and `friend class DfsAnagramSearch`.
9. Keep `ProjectedWorker`, `BottomUpWorker`, `TopDownWorker`, recursive and
   top-level evaluators, candidate evaluation, `bound_state_view()`, and all
   direct table-storage access private to `ScoreBounds`. The public surface is
   only `build()`, `lookup()`, `root_lookup()`, `active()`, and `stats()`.

### Invariants

- Bottom-up versus top-down selection is unchanged.
- Every float table entry, root bound, rounding step, and counter is unchanged.
- Active preflight diagnostics precede evaluator progress diagnostics.
- Disabled mode is emitted by the facade and does not call `build()`.
- Cache fallback is not confused with allocation/evaluator failure.
- Lookup behavior and runner query interfaces are unchanged.

### Verification

- [x] `meson compile -C build`
- [x] `meson test -C build dfs-search dfs-cli --print-errorlogs`
- [x] `meson test -C build query-index-cli --print-errorlogs` with `IDX` set
- [x] `/review`
- [x] `git diff --check`

## Phase 4 — dissolve preparation status [x]

### Purpose

Remove the final non-query members and leave `DfsAnagramSearch` as a stateless
orchestrator between calls.

### Changes

1. Replace `unsupported_reason` with a local failure reason used immediately
   by the shared phase-2 abort helper. Pass a `char const** failure_reason`
   as the final argument, making the final signature
   `prepare_hot_classes(DfsSearchData*, ScoreKeyLayout const&,
   std::unique_ptr<uint16_t[], DfsAlignedFree>*, char const**)`. Set it to the
   existing granular reason at every false return and leave it `NULL` on
   success.
2. Replace `hot_classes_ready` with local success control. A false hot-class
   build emits the diagnostic and aborts before later preparation.
3. Delete `require_hot_classes()` after its diagnostic is folded into the
   immediate failure path.
4. Delete `empty_class_list`; derive `class_list->classes().empty()` where the
   successful no-op path is needed. `run()` returns successfully before runner
   construction; `find_completable_classes()` does not return early and lets
   `DfsAnySolutionRunner` produce the empty result.
5. Remove the three members and constructor initialization from
   `DfsAnagramSearch`.
6. Confirm the facade has exactly these ten query members: `class_list`,
   `letters`, `score_model`, `segment_boundary_log_score`,
   `best_member_log_scores`, `max_depth`, `score_cache_budget`,
   `requested_preprocess_threads`, `requested_search_threads`, and
   `support_scan_vector`.
7. Update the finding and this plan with final ownership and validation status.

### Invariants

- An empty class list remains a successful search with no results.
- A real preparation failure emits a diagnostic and aborts.
- Both public entry points share the same preparation behavior.
- Statistics are complete before local preparation scratch is destroyed.
- No runner borrows preparation-local state.

### Verification

- [x] `meson compile -C build`
- [x] `meson test -C build dfs-search dfs-cli --print-errorlogs`
- [x] `meson test -C build query-index-cli --print-errorlogs` with `IDX` set
- [x] `meson test -C build --print-errorlogs` with `IDX` set
- [x] `/review`
- [x] `git diff --check`

## Completion criteria

The plan is complete when:

- `DfsAnagramSearch` contains only its ten query members;
- `ScoreKeyLayout`, wildcard lengths, and `ProjectedActions` are local to
  preparation and destroyed before runner construction;
- `ScoreBounds` owns allocation, evaluation, storage, lookup, and bound stats;
- the facade owns disabled-mode and policy diagnostics;
- active bound construction owns its mode/evaluator diagnostics;
- component build failures diagnose and abort;
- policy fallback retains its existing behavior;
- `friend class DfsAnagramSearch` is gone; and
- the existing full test suite passes.
