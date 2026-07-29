# Plan: use map-only retention for `dfs-anagrams -n 0`

## Goal

Remove unnecessary heap maintenance from unlimited output. With `-n 0` there
is no score floor or eviction, and `take_sorted_results()` fully sorts the
retained results afterward, so maintaining heap order during collection has no
value.

The bounded behavior does not change: for every `n > 0`, the heap remains
capped at exactly `n` slots. Only `n == 0` uses map-only retention and keeps
the heap empty.

## Implementation

### 1. Make the retained map the common owning store

In `source/dfs-output.h` and `source/dfs-output.cpp`:

- Keep `retained` as the single owning store in both bounded and unlimited
  modes.
- Change `DfsTopN::size()` to return `retained.size()`, since an unlimited
  sink will no longer have a heap entry for each retained result.
- Keep `HeapSlot`, heap positions, sifting, score-floor publication, and
  eviction for finite top-N operation only.
- Update comments to state that `heap_pos` is meaningful only when
  `result_limit > 0`.

### 2. Keep the unlimited path out of the heap

Keep branching localized inside `offer()` rather than introducing separate
collection implementations or duplicating deduplication logic:

- Perform the common lookup in `retained`.
- For an existing key, reject an equal or weaker spelling as today.
- For an improved duplicate, update the map entry in both modes, but update
  its `HeapSlot` and call `sift_down()` only when `result_limit > 0`.
- For a new unlimited result, insert it directly into `retained` and return
  without allocating a `HeapSlot` or calling `sift_up()`.
- For `result_limit > 0`, preserve the existing bounded insertion and eviction
  paths. The heap grows only until `heap.size() == result_limit`; subsequent
  unique improvements recycle the weakest retained entry instead of growing
  either container.

Make `publish_floor()` explicitly return when `result_limit == 0`. This guard
becomes necessary once a successful unlimited insertion leaves the heap empty,
and it documents that unlimited mode never publishes a floor.

### 3. Materialize results from the map

Simplify `take_sorted_results()` so both modes use the same drain:

- Reserve `retained.size()` elements in the result vector.
- Iterate over `retained`, copying each map key and moving each retained text
  into a `DfsSpelling`.
- Release both `heap` and `retained` afterward so sink reuse retains its
  existing behavior.
- Keep the existing final deterministic sort: descending score, then
  word-set key, then text.

Iterating the owning map for both modes avoids a separate unlimited drain
branch. In unlimited mode this eliminates the 16-byte `HeapSlot` allocation
for every retained spelling as well as all `sift_up()` and `sift_down()` work.

## Minimal tests

### Unit smoke test

Extend the unlimited case in `source/test-dfs-output.cpp`:

- Retain several distinct keys with `result_limit == 0`.
- Offer both a stronger and a weaker duplicate.
- Check `size()` before draining.
- Check that the stronger duplicate wins, all unique keys are returned, and
  the final order is correct.
- Confirm `score_floor()` remains unavailable.

Leave the existing bounded heap-churn and concurrent top-N tests in place.
They cover heap-position updates, duplicate improvement, eviction, the finite
result cap, floor monotonicity, and sink reuse.

### CLI smoke test

In `source/test-dfs-cli.sh`:

- Retain the comparison between `-n 0` and a finite limit large enough to
  contain every result from the synthetic index.
- Explicitly verify that the unlimited run returns more rows than the existing
  `-n 2` run, so the test distinguishes unlimited output from an accidentally
  fixed top-N limit.

## Verification

1. Build using the repository's Conan environment.
2. Run the focused `dfs-output` and `dfs-cli` smoke tests.
3. Run `/review` before committing.
4. For a performance comparison, export `IDX` as instructed and check for
   other `dfs-anagrams` processes immediately before each timing run. Use the
   same `-n 0` workload before and after, redirect stdout, and compare elapsed
   time and peak RSS so output rendering does not dominate the measurement.

## Acceptance criteria

- `-n 0` retains and returns the same fully sorted, globally deduplicated
  results as before.
- Unlimited collection never adds a `HeapSlot` and never calls either sift
  operation.
- For `n > 0`, `heap.size()` never exceeds `n`, and exact top-N behavior is
  unchanged.
- Concurrent unlimited emission remains protected by the existing mutex.
- Draining and reusing a `DfsTopN` instance remains correct.
