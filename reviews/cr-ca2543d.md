# Code review ca2543d

Parallel search can invoke non-thread-safe sinks concurrently, and projected
fallback can query incorrect modular states and prune valid results. The patch
also introduces stale floor state when a top-N sink is reused.

## Findings

### 1. [P1] Require sink concurrency support before dispatching workers

Location: `source/dfs-search.cpp:3502-3503`

When `NUTRIMATIC_SEARCH_THREADS` is greater than one, multiple workers call the
same sink's `emit()` and `score_floor()` concurrently, but `DfsSolutionSink`
provides no thread-safety contract or capability flag. Existing sinks such as
`CollectSolutions` mutate ordinary vectors and sets, so enabling this mode
causes data races, heap corruption, and observed crashes; parallel execution
must be gated on explicit sink support or sink calls must be synchronized.

Status: completed.

Completion: Added `DfsSolutionSink::supports_parallel_search()` as an explicit
opt-in contract covering concurrent `emit()` and `score_floor()` calls. Search
now checks that capability before entering `run_parallel_search()`;
`DfsTopN` opts in because its heap mutation is locked and its published floor
is atomic, while existing sinks retain the safe serial default.

### 2. [P1] Initialize workers with the root modular signatures

Location: `source/dfs-search.cpp:3388`

When projected certificate fallback is enabled and the root modular signature
is nonzero, initializing this state to zero makes subsequent subtraction track
`-consumed` instead of the residual bag's `root-consumed` signature. Fallback
queries then index an unrelated modular bound, which can be `-HUGE_VAL` or
otherwise too low and incorrectly prune valid top-N solutions; the worker must
start from `current_modular_signatures`.

Status: completed.

Completion: `start_search_worker()` now copies
`current_modular_signatures`, preserving the root bag's modular state before
any class deltas are subtracted. This initializes both serial workers and the
parallel task-generation seed correctly.

### 3. [P2] Use the worker-local modular signature for diagnostics

Location: `source/dfs-search.cpp:3129-3131`

With projected query diagnostics enabled, descending now updates
`worker->modular_signatures`, while `current_modular_signatures` remains at the
root state. Every non-root rich-bound query therefore evaluates the modular
shadow bound for the wrong signature, making the reported prefix, rich-only,
and modular-only prune counts inaccurate.

Status: completed.

Completion: The rich-versus-modular diagnostic comparison in
`should_prune()` now indexes each modular table with
`worker->modular_signatures[p]`, so non-root counters describe the same
residual bag as the rich-bound query.

### 4. [P2] Clear the published floor when draining results

Location: `source/dfs-output.cpp:81-82`

Once the heap has been full, `published_full` remains true permanently even
though `take_sorted_results()` empties the heap. Reusing the `DfsTopN` after
draining therefore exposes the old floor and causes both search pruning and the
early check in `emit()` to reject candidates before the new heap is full, a
regression from the previous heap-derived `score_floor()` behavior.

Status: completed.

Completion: `take_sorted_results()` now locks the heap and clears
`published_full` before swapping out the heap and deduplication index. A reused
`DfsTopN` therefore reports no score floor until its new heap becomes full and
`publish_floor()` publishes a fresh value.

## Verification

Completed:

- `source /home/mike/code/nutrimatic/.env/bin/activate && conan build .`
- `./build/test-dfs-search`
- `./build/test-dfs-output`
- `git diff --check`
- Final Codex diff review: no additional findings.

Regression coverage now verifies that a sink without the concurrency opt-in
stays serial, `DfsTopN` still uses parallel search and matches serial results,
projected fallback and modular diagnostic invariants retain the expected
spellings, and a drained `DfsTopN` accepts a lower-scoring refill.
