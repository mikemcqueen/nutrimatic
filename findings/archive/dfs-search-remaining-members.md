# What is left on the facade, and where it goes

`findings/dfs-refactor-the-refactor.md:376-386` ends with thirteen non-query
members still on `DfsAnagramSearch` and calls moving them "the `HotClassIndex`
extraction". That label is wrong, and correcting it is the first step: the
thirteen are not one component but four groups. Two become preparation-time
components, one is a temporary value passed between them, and one is status
that dissolves into orchestration.

## The name has drifted

The plan's `HotClassIndex` (`findings/dfs-search-refactor.md:485-546`) is
specifically the five hot per-class arrays: `fit_classes`, `class_supports`,
`score_key_deltas`, `score_wild_lengths`, `packed_letters`.

Four of those five already landed. They are `DfsSearchData` members
(`dfs-search-data.h:56-59`). Only `score_wild_lengths` is still on the facade,
and only because `prepare_projected_actions()` is the one thing that reads it.

So the `HotClassIndex` step is substantially done under another name. What
remains is the plan's `ScoreKeyLayout`
(`findings/dfs-search-refactor.md:448-459`), one temporary output from hot-class
preparation, preparation status, and one component the plan mentions only in
passing, in the projected-bound internals section
(`findings/dfs-search-refactor.md:1058` region): *"Projected actions should not
be exposed through the general bound interface. They are an implementation
detail of constructing a projected table."*

## The four groups

**A — score-key projection layout** (`dfs-search.h:119-122`).
`score_multipliers`, `score_exact_mask`, `score_exact_letters`,
`score_wild_span`.

`DfsSearchData` already holds the runtime projection state,
`score_wild_letters` and `score_key` (`dfs-search-data.h:50-52`). Its adjacent
`exact_root_key` belongs to the separate exact bag encoding used by completion
search; it is not part of `ScoreKeyLayout`.

**B — the projected-action table** (`dfs-search.h:124-132`).
`projected_actions`, `projected_action_support`,
`projected_repeated_requirements`, `projected_bucket_starts`,
`projected_actions_ready`, the `ProjectedAction` type
(`dfs-search.h:63-70`), and seven methods: `prepare_projected_actions()`,
`projected_action_fits()`, `first_projected_length_candidate()`,
`consider_projected_top_down_candidate()`,
`compute_projected_score_bound_top_down()`, and the two
`compute_projected_score_bounds_*()`.

**C — temporary projected wildcard lengths** (`dfs-search.h:123`).
`score_wild_lengths` is produced with the hot per-class arrays but consumed
only while constructing Group B.

**D — preparation status** (`dfs-search.h:133-139`). `hot_classes_ready`,
`empty_class_list`, `unsupported_reason`. Not data: this is
`prepare_phase_two()`'s return value wearing member clothes.
`empty_class_list` is `class_list->classes().empty()`, derivable at any point.

## What the code says about each group

**Group B is entirely projected-bound construction.** Both evaluators read the
whole of it: `projected_bucket_starts` at `dfs-search-projected.cpp:563`
(top-down) and `:614,803` (bottom-up); the action table, the support sidecar,
and the repeated-requirement pool through `projected_action_fits()` and
`first_projected_length_candidate()`, which both evaluators call.

**Group A is the encoding rule for the whole search, but only preparation reads
it.** `score_wild_span` flattens `score_key_deltas` in `prepare_hot_classes()`
(`dfs-search.cpp:253`), and both runners read those flattened deltas — so the
layout is upstream of the exact search, not just the bound. But checked against
the code: **no runner reads a layout field.** `score_multipliers`,
`score_exact_mask`, and `score_wild_span` occur only in `dfs-search.cpp` and
`dfs-search-projected.cpp`. The runners see the flattened results and nothing
else. Both evaluators do read the layout directly:
`dfs-search-projected.cpp:853,913` top-down, `:654-673,785-787` bottom-up.

**Neither group is score-bound *lookup*.** Both are score-bound
*construction*, and that distinction decides the placement:

> Every member in groups A and B is dead the moment bound computation returns.
> No runner touches an action, a bucket start, or a multiplier.

Group C dies earlier, as soon as `ProjectedActions::build()` returns.

## Where it goes

### Not into `DfsSearchData`

`DfsSearchData` is moved into the runner and lives for the whole search. Group B
is build scratch: 48 bytes per action plus an 8-byte support sidecar and the
repeated-requirement pool, one action per distinct score-key delta, up to one
per class. Putting it in
`DfsSearchData` keeps all of that alive through the search for nothing, and
drops cold data into the struct whose layout was tuned against a measured 2%
(`findings/dfs-search-refactor.md:695-704`).

### A new header, `dfs-projected-actions.h`

```cpp
struct alignas(16) ProjectedAction { ... };   // moves off the facade

class ProjectedActions {
 public:
  static bool build(DfsSearchData const&, ScoreKeyLayout const&,
                    uint16_t const* wild_lengths, ProjectedActions*);
  size_t bucket_begin(size_t bucket) const;
  size_t bucket_end(size_t bucket) const;
  ProjectedAction const& action(size_t i) const;
  bool fits(size_t action, BoundStateView) const;
  size_t first_length_candidate(size_t begin, size_t end, size_t left) const;
  size_t size() const;
};
```

Instantiated as a local in `prepare_phase_two()` and destroyed when it returns,
so the lifetime is stated rather than accidental.

For this extraction, `ProjectedActions::build()` failure is not a cache-policy
fallback. The facade emits the phase-2 failure diagnostic and aborts. In
particular, a failed action build must never be followed by bound-table
allocation or evaluation. The current `score_bounds_applicable` condition must
also regain its `projected_actions_ready` requirement; dropping that check from
the old allocation path is a working-tree regression. The check remains useful
as a boundary invariant even though the immediate abort makes it defensive.

### The evaluators move into `ScoreBounds::build()`

`findings/dfs-refactor-the-refactor.md:190-194` records `ScoreBounds::build()`
as blocked on having something to build from. It unblocks more cleanly than that
note suggests, because the four evaluator methods need very little of
`DfsSearchData`: `data->bag`, `data->bag_mask`, `data->score_key`,
`data->score_wild_letters`, and `letters.size()`.

That is exactly `BoundStateView` (`dfs-score-bounds.h:25-30`), which already
exists and which `bound_state_view()` (`dfs-search.h:72-76`) already builds:

```cpp
bool ScoreBounds::build(BoundStateView root, ScoreKeyLayout const&,
                        ProjectedActions const&, size_t budget,
                        size_t threads, DfsSearchStats*);
```

No `DfsSearchData` parameter. That avoids passing `ScoreBounds` the struct that
contains it — `DfsSearchData` holds a `ScoreBounds` by value
(`dfs-search-data.h:42`) — and it honours dependency rule 2
(`findings/dfs-search-refactor.md:1058` region) in the direction that matters.
It also removes `friend class DfsAnagramSearch` (`dfs-score-bounds.h:86`): the
evaluators become members and stop reaching into `float_values_` and
`plain_float_values_` from outside.

### `ScoreKeyLayout` gets its own header

The tempting move is to collapse the split by putting the whole layout into
`DfsSearchData`. Against it: `score_multipliers` is a 512-byte
`array<uint64_t, DFS_SYMBOL_COUNT>` that no runner reads, riding into the hot
struct.

Instead it is a preparation-time value, produced by extracting the selection
block at `dfs-search.cpp:445-547` into `ScoreKeyLayout::choose()`, passed by
const reference to `prepare_hot_classes()`, `ProjectedActions::build()`, and
`ScoreBounds::build()`. It includes the complete selection result from the
original plan: multipliers, exact mask, state count, effective state count,
projected root key, exact-letter count, wildcard-letter count, and wildcard
span. `DfsSearchData` keeps only the runtime values its runners read.

This is not the duplication that
`findings/dfs-refactor-the-refactor.md:288-290` set out to remove.
`exact_mask` and `multipliers` are the encoding *rule*; `score_key` and
`exact_root_key` are a *state* in that encoding. Different declarations of
different things.

Declare it in a new `dfs-score-key-layout.h`. Both `dfs-search-data.h` and
`dfs-score-bounds.h` need to name concepts on opposite sides of this boundary;
a dedicated header keeps the shared preparation type independent and avoids
making either header include the other merely to obtain the layout.

### `score_wild_lengths` stays local

It is produced by `prepare_hot_classes()` and consumed only by
`ProjectedActions::build()`. It should be an out-parameter of hot-class
building, held by `prepare_phase_two()` — not a facade member, and not a
`DfsSearchData` member. That is why it did not move with its four siblings.

### Group D dissolves

`unsupported_reason` and `hot_classes_ready` become local build status in
`prepare_phase_two()`. For now, any component build failure, including
hot-class construction, produces its phase-2 diagnostic and aborts;
`require_hot_classes()` folds into that immediate failure path. This is not yet
a recoverable `PreparationResult`. `empty_class_list` is a valid successful
no-op and is deleted in favour of the one-line derivation.

That leaves the facade with the ten query members and nothing else, which is the
target shape in `findings/dfs-refactor-the-refactor.md:204-210`.

## Order

1. **`ScoreKeyLayout` and the temporary wildcard lengths.** Extract
   `dfs-search.cpp:445-547` into `choose()`, thread the layout by const
   reference, and make `score_wild_lengths` an out-parameter held by
   `prepare_phase_two()`. Facade loses five members. Mechanical; no value lives
   longer than it does now.
2. **`ProjectedActions` and `ScoreBounds::build()`.** Move the table and the
   seven methods. Facade loses five more, the `friend` goes away, and
   `dfs-search-projected.cpp` stops declaring `DfsAnagramSearch::` on every
   function.
3. **Group D.** Fold into step 2 or take separately.

### Diagnostic ordering in step 2

There are currently three operations, in this order:

1. `ScoreBounds::prepare()` allocates the table and thereby chooses the
   bottom-up or top-down representation.
2. `prepare_phase_two()` reports the chosen mode and evaluator
   (`dfs-search.cpp:634-651`).
3. The chosen evaluator fills the table and may emit its own progress lines.

A naive `ScoreBounds::build()` that silently combines operations 1 and 3 would
leave the facade able to print the preflight lines only after the evaluator has
finished. That would reorder them after evaluator progress, changing verbose
CLI output. This is only a diagnostic-ordering issue; it does not constrain the
bound algorithm or its ownership.

On the active path, keep the same sequence inside `ScoreBounds::build()`:
allocate and select the representation, emit the existing active-mode and
evaluator preflight diagnostics, then invoke the selected evaluator. The
projected evaluators already emit component-owned progress diagnostics, so no
diagnostics hook is needed and `prepare()` can remain private.

The disabled path never enters `ScoreBounds::build()`. The facade retains the
default-constructed off bound, emits the existing `score-bound mode off`
diagnostic, and continues.
Policy-driven cache fallback belongs to this disabled path; it is not a
component build failure. If an active component build actually fails after the
facade has selected it, the facade emits a diagnostic and aborts rather than
turning that failure into `mode off`. These two paths preserve the text and
ordering of `dfs-anagrams -v` output without exposing a half-built object.

## Implemented ownership

The extraction is complete in the working tree. `DfsAnagramSearch` now keeps
only its ten constructor/query members. `ScoreKeyLayout`, wildcard lengths,
and `ProjectedActions` are preparation locals, and no runner borrows them.
The wildcard-length table is released immediately after projected-action
construction.

`ScoreBounds::build()` now owns active allocation, representation selection,
active-mode diagnostics, bottom-up and top-down evaluation, storage, and bound
statistics. The facade owns policy selection and the disabled-mode diagnostic,
sets `DfsSearchData::score_bounds_active` from the completed value, and
snapshots statistics before runner construction. Component failures diagnose
and abort; cache-policy fallback still selects the normal disabled path.

The focused `dfs-search`, `dfs-cli`, and `query-index-cli` tests pass with
`IDX` set, and the full existing Meson suite passes 7/7. `git diff --check`
also passes.
