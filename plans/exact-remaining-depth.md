# Exact remaining-depth projected score bounds

## Summary

Add an opt-in `dfs-anagrams --exact` mode for exact-`-g N` searches. In this
mode the projected score table records the best completion score separately
for each exact number of segments still owed by the phase-2 walk:

```text
V[L][d] = best provisional score that exhausts projected letter state L
          using exactly d further segments
```

The current table remains the default. It stores one value per projected
letter state and admits a completion using any positive number of further
segments. `--exact` changes only that remaining-segment relaxation. It does
not add the BEST-count dimension proposed in
`findings/dfs-score-optimization-idea-2.md`, change phase-1 class scores, or
change `ProjectedAction`.

This is possible as a long-only option: `--exact` is not currently registered
by `dfs-anagrams` or by `DFS_COMMON_LONG_OPTIONS`.

## CLI contract

Add `bool exact_remaining_depth` to `dfs-anagrams.cpp`'s `Args`, defaulting to
false, and add one local option code and this long option:

```cpp
{ "exact", OPT_EXACT, OPTPARSE_NONE },
```

The detailed help entry is:

```text
--exact
    index projected score bounds by the exact number of segments remaining;
    requires -g N and uses N-1 values per projected state for N greater than
    1, instead of one
```

The usage synopsis remains `[-i INDEX] [options] letters`; long-only options
are not expanded into the synopsis.

After all options have been parsed, reject `--exact` when
`args.num_segments == 0`:

```text
error: --exact requires -g N
```

Accept `--exact -g 1`. There is no non-root remaining-state lookup in a
one-segment search, but accepting it keeps the option's domain equal to
`-g N`'s positive domain. Its root feasibility calculation is exact and its
table retains one value per state, so it consumes no more cache memory than
the current mode.

Pass the flag into `DfsAnagramSearch` after `exact_segments`:

```cpp
DfsAnagramSearch(
    DfsClassList const* classes, std::string const& letters,
    DfsScoreModel const& score_model,
    size_t score_cache_bytes = 0,
    size_t preprocess_threads = 1,
    size_t search_threads = 1,
    size_t exact_segments = 0,
    bool exact_remaining_depth = false);
```

Store it as a query-fixed `bool const` beside `exact_segments`. The CLI is the
only caller that passes true. Existing callers, including `query-index`, keep
the default and therefore keep the scalar relaxed table.

Add `bool exact_remaining_depth_bounds` to the private
`prepare_phase_two()` parameters. `run()` passes the query-fixed flag;
`find_completable_classes()` passes false. When the flag is true,
`prepare_phase_two()` passes both `exact_segments` and the flag into
`ScoreBounds::build()`:

```cpp
bool ScoreBounds::build(
    BoundStateView root, ScoreKeyLayout const& layout,
    ProjectedActions const& actions, size_t budget, size_t threads,
    size_t exact_segments, bool exact_remaining_depth,
    DfsSearchStats* stats);
```

Passing `exact_segments` as well as the derived plane count keeps `N==1`
distinguishable from `N==2`; both allocate one value per projected state, but
their exact root recurrences owe different numbers of segments.

`find_completable_classes()` always prepares relaxed bounds even if a caller
constructs a search with the flag true. That operation asks whether classes
have any completion and does not carry an exact number of segments owed;
depth-indexing it would change its contract. `run()` is the only entry point
that forwards the query-fixed flag into phase-2 preparation.

## Bound semantics

Let `A` be a fitting projected action. Its existing `partial_score` already
contains the class score and one segment-boundary score. Exact-depth bounds use
the recurrence:

```text
V[empty][0]       = 0
V[nonempty][0]    = -infinity
V[empty][d > 0]   = -infinity
V[L][d]           = max over fitting A:
                        A.partial_score + V[L - A][d - 1]
```

Every action consumes exactly one segment, so neither `ProjectedAction` nor
the projected-action quotient needs a depth field or another score. The same
action record is reused for every `d`.

The recurrence continues to be a projection over letters. When letter types
have been merged into the wildcard coordinate, a finite value is still only
an upper bound on the concrete remainder. `--exact` makes the segment count
exact; it does not undo letter projection.

The same upward-rounding rules remain in force:

- accumulate candidates and their error envelopes in `double`;
- call `get_score_bound()` for each `(L, d)` cell;
- store the result with `round_float_score_bound_up()`;
- preserve `-HUGE_VAL` for unreachable cells.

This makes each cached value an admissible bound for exactly `d` appended
segments. It is no greater than the corresponding relaxed value because it
maximizes over a subset of the relaxed completions.

## Dense layout

Do not allocate full `d=0` or root `d=N` planes.

- `d=0` is the virtual base case above and is needed only to distinguish the
  empty state from nonempty states.
- The root remains the separately stored `root_score_bound_`. For `N > 1`,
  calculate it from the `d=N-1` child plane. For `N == 1`, calculate it by
  accepting only root actions that consume the whole projected root state.
- Every non-root lookup made by an exact `-g N` walk has
  `1 <= d <= N-1`.

For `N > 1`, store `N-1` complete planes. For `N == 1`, retain one plane so
the existing allocation and active-mode invariants do not acquire a
zero-capacity special case; no non-root score lookup reads it.

```text
depth_values = max(1, N - 1)
slot(d, key) = (d - 1) * state_capacity + key
```

Each depth plane is contiguous in `key`. This preserves contiguous wildcard
child ranges for the AVX2 bottom-up kernel. A state-major layout would make
those children strided and is explicitly not used.

The relaxed layout remains exactly one plane with its current recurrence and
slot numbering. Do not route the default through the exact recurrence; the
default's output, counters, evaluator choice and diagnostics remain unchanged.

For the motivating 14,745,600-state `-g6` input:

```text
current relaxed table:  1 * 4 * 14,745,600 =  56.25 MiB
exact-depth table:      5 * 4 * 14,745,600 = 281.25 MiB
additional table:       4 * 4 * 14,745,600 = 225.00 MiB
```

## Projection selection and cache policy

Make projection selection aware of the bytes stored per projected letter
state. Change `ScoreKeyLayout::choose()` to take `value_bytes_per_state`
rather than assuming `sizeof(float)`. Its automatic loop uses that width when
testing each candidate exact-letter projection against `-C`.

Phase-2 preparation computes:

```text
relaxed: value_bytes_per_state = sizeof(float)
exact:   value_bytes_per_state = sizeof(float) * depth_values
```

Use checked multiplication before projection selection and again before
allocation. Continue to round the final allocation size with
`dfs_round_up_alignment()`.

This gives the existing cache controls these precise meanings:

- With automatic projection depth, `--exact` selects the largest number of
  exact letter types whose entire depth-indexed table fits `-C`.
- With explicit `-d`, preserve the requested letter projection. If its exact
  table exceeds `-C`, issue the existing required-MiB failure unless `-F` was
  supplied.
- With `-F`, a failed exact table falls back to score-bound mode off. It must
  not silently build the relaxed table, because that would make `--exact`
  claim a bound semantics it did not retain.
- `-C 0 -F` continues to disable score bounds.
- If the output sink does not support score pruning, including `-n 0`, do not
  allocate either table; accepting `--exact` does not override the existing
  sink policy.

The first preflight line that reports projected states and bytes uses the
same `value_bytes_per_state`, so it reports the actual exact-table allocation
rather than the scalar size.

## `ScoreBounds` state and statistics

Add these fields to `DfsSearchStats::Bounds`:

```cpp
size_t depth_values = 1;
bool exact_remaining_depth = false;
```

Keep the existing fields' meanings stable:

- `capacity` remains the number of projected letter states in one plane.
- `value_bytes` remains `sizeof(float)`.
- `bytes_charged` is the full aligned allocation across all planes.
- `entries` is the number of materialized `(state, depth)` values. It equals
  `capacity * depth_values` for bottom-up construction and the number of
  published atomic slots for top-down construction.
- `projected.states_computed` counts `(state, depth)` cells in exact mode and
  letter states in relaxed mode. Its diagnostic is changed only for exact
  mode to call these “bounded state-depth values.”

`ScoreBounds` retains the same one-allocation ownership. Add its selected
depth count and exact/relaxed flag to its private state, reset them in
`clear()`, and flatten both `plain_float_values_` and `float_values_` using the
plane-major slot formula.

Change `prepare()` to accept `state_capacity`, `depth_values`, and the exact
flag. Allocate `state_capacity * depth_values` float words after checked
multiplication. Do not add one allocation per plane.

The current relaxed mode diagnostic stays byte-for-byte unchanged. Exact mode
prints a distinct line:

```text
phase 2 preflight: score-bound mode projected dense exact remaining depth
    (4-byte values, D values/state, capacity S, complete effective coverage)
```

Keep that as one physical diagnostic line despite its wrapping above. This
allows a run log to establish whether `--exact` survived cache selection and
how many values were allocated per state.

## Lookup interface and phase-2 walk

Change the non-root score lookup to accept the exact number of segments owed:

```cpp
bool ScoreBounds::lookup(
    uint64_t key, size_t segments_owed, double* value) const;
```

The contract is:

- relaxed mode ignores `segments_owed` and reads plane zero;
- exact mode requires `1 <= segments_owed <= depth_values` and reads plane
  `segments_owed - 1`;
- an out-of-range key or depth returns false rather than reading storage.

Keep `root_lookup()` separate.

In `DfsAllSolutionsRunner::should_prune()`:

- keep using `root_lookup()` when `worker->path` is empty;
- in exact mode calculate
  `segments_owed = data.exact_depth - worker->path.size()` and pass it to
  `lookup()`;
- in relaxed mode pass zero, which `lookup()` ignores.

The existing exact-`-g` guards guarantee that a recursed non-root node has a
positive number of segments owed. Add assertions for that invariant and for
`segments_owed <= depth_values` in exact mode.

Add a query-fixed `bool exact_remaining_depth_bounds` to `DfsSearchData` so
the hot lookup path does not infer policy from allocation statistics. It is
true only when the exact table was successfully selected and built; cache
fallback leaves it false along with `score_bounds_active`.

`DfsSearchData::cached_reachability()` and `DfsAnySolutionRunner` remain on
the relaxed lookup path. `find_completable_classes()` never builds an exact
table, so pass zero to the new lookup parameter there. Do not make the exact
completion memo carry a segment depth as part of this change.

## Bottom-up evaluator

Retain the current bottom-up evaluator selection rule and threading model.
Add a separate exact-depth construction path inside `ScoreBounds`; do not put
an exact/relaxed branch in the AVX2 inner lane update.

Build exact planes in increasing `d`:

1. Treat `d=0` as a virtual child plane. A transition from a `d=1` cell is
   live only when subtracting the action produces the empty projected state;
   its child value is exactly `0.0`.
2. For each `d >= 2`, scan the same fitting projected actions as today but
   read children from the completed `d-1` plane and write parents to the `d`
   plane.
3. Keep planes contiguous, so the existing
   `projected_wild_update_avx2()` receives a contiguous child range from the
   preceding plane. Parent `best` and rounding-error scratch stays one
   wildcard span per worker and is cleared before each `(exact_key, d)` row.
4. Store `-HUGE_VALF` for every state that cannot be exhausted in exactly
   `d` actions.
5. After the last non-root plane is complete, calculate the separate root
   bound using exactly `N` actions: a root action plus a child from plane
   `N-1`. Handle `N==1` against the virtual `d=0` base.

Depth is the outer dependency order. Within one completed child plane, retain
the current exact-letter layers, dynamic worker queue, action buckets,
support filtering, repeated-count checks, and wildcard vector kernel.
Aggregate candidate, fitting-transition, successful-transition and rounding
counters across all depth planes.

The exact recurrence has no same-depth dependencies. Do not use that fact to
redesign the existing letter-layer scheduling in this change; keeping its
partitioning limits the implementation to the new dimension.

## Top-down evaluator

The atomic top-down evaluator must support the same semantics even though
ordinary workloads normally select bottom-up plain storage.

Add `segments_owed` to `TopDownWorker`. Use the plane-major slot when claiming,
waiting for, publishing, or reading an atomic value. The recursion is:

- if `segments_owed == 0`, return `0.0` only for the empty projected state and
  `-HUGE_VAL` otherwise, without claiming a table slot;
- if the projected state is empty while `segments_owed > 0`, publish and
  return `-HUGE_VAL` for that `(L, d)` cell;
- otherwise subtract one fitting action, decrement `segments_owed`, recurse,
  then restore both the bag and depth before trying the next action.

Root workers start with `N` segments owed, but the root itself remains outside
the atomic array. Root candidate threads subtract their assigned first action
before the recursive lookup, so every claimed slot is in `1..N-1`. `N==1`
terminates at the virtual base without claiming a slot.

Atomic unseen/computing sentinels, wait behavior, exception cleanup, and
upward rounding remain unchanged. A `(key, d)` slot synchronizes only with the
same `(key, d)`; equal letter states at different remaining depths must not
share an atomic word.

## Tests

Keep validation focused.

### `source/test-dfs-cli.sh`

Add three CLI cases:

1. `--exact` without `-g` exits 2 and prints exactly
   `error: --exact requires -g N`.
2. Run the existing `abcd` small index with `-m 1 -g 3 --exact`, compare
   stdout with the same `-m 1 -g 3` run using cache size zero plus `-F`, and
   require the exact-mode diagnostic to report two values per state. The
   explicit `-m 1` is required because the existing `-m 2` fixture permits at
   most two segments.
3. Run `-g 1 --exact`, compare its stdout with existing `-g 1`, and require a
   successful exit. This locks the deliberate one-segment acceptance.

Do not broaden the shell test into performance assertions.

### `source/test-dfs-search.cpp`

Extend the existing projected-bound smoke fixture with one exact-`-g3` search:

- disable the length certificate for this comparison so it cannot own the
  prune being tested;
- obtain expected spellings from the same exact-`-g3` search with cache size
  zero;
- build the exact table with a fixed projection and enough budget;
- assert identical retained spellings and scores;
- assert `exact_remaining_depth`, `depth_values == 2`, unchanged per-plane
  `capacity`, `value_bytes == sizeof(float)`, and
  `bytes_charged` equals the aligned result of
  `capacity * depth_values * sizeof(float)`;
- assert `entries == capacity * depth_values` for the bottom-up evaluator;
- use a fixed score floor on the `aabb` fixture to assert that the exact table
  visits no more nodes than the relaxed table. The correctness comparison,
  not a strict node-count improvement, is the test contract.

Retain the existing serial/threaded comparison. Add the exact `-g3` variant
only if it reuses the same fixture: serial and threaded construction must have
identical counters, results, depth count and byte charge.

No public test hook is added for the private top-down recurrence. Compile it
under the existing warnings-as-errors build and review its base cases and slot
arithmetic directly.

## Implementation order

1. Add the CLI flag, validation, help text, constructor field and the
   run-versus-completability policy plumbing. At this point the flag reaches
   preparation but does not alter storage.
2. Add `depth_values` and `exact_remaining_depth` statistics, make
   `ScoreKeyLayout::choose()` width-aware, and allocate one checked flattened
   array for all depth planes. Update preflight byte accounting and the exact
   diagnostic in the same step.
3. Add the bottom-up exact recurrence, including the virtual `d=0` base and
   separately calculated exact root.
4. Add the top-down exact recurrence and plane-qualified atomic slots.
5. Pass `segments_owed` from `DfsAllSolutionsRunner` into lookup; leave
   `DfsAnySolutionRunner` explicitly relaxed.
6. Add the focused C++ and CLI cases, then perform code review before any
   commit.

## Validation

Use the repository setup and keep the run to the affected smoke targets:

```bash
source ./setup.sh
source .env/bin/activate
source build/dep-info/conanbuild.sh
meson compile -C build dfs-anagrams test-dfs-search
meson test -C build dfs-search dfs-cli --print-errorlogs
git diff --check
```

Accurate timing is not required for correctness validation. Before any later
benchmark comparing relaxed and exact modes, check the host process table for
both `query-index` and `dfs-anagrams` as required by `AGENTS.md`.

Run `/review` on the implementation before committing. Report the focused
tests above as focused smoke validation, not as a full-suite result.

## Not in scope

- BEST-count-indexed values `V[L][k]` or combined `V[L][d][k]` values.
- Additional phase-1 class scores or `ProjectedAction` score alternatives.
- Sparse or variable-length depth storage.
- Making `length_tail_bounds` or the length certificate depth-indexed.
- Last-segment signature lookup, floor seeding, or score-order changes from
  `findings/g6_relief.md`.
- Changing result scoring, BEST corrections, segment penalties, or emitted
  result order.
- Enabling exact depth by default. The default remains the current scalar,
  remaining-segment-relaxed bound.
