# Plan: integrate the `dfs-codex-1` certificate and parallel search

## Outcome

Integrate the two useful phase-2 changes from independent branch
`dfs-codex-1` into this branch:

1. an exact consumed-length-group certificate for concrete DFS; and
2. parallel concrete DFS over shallow independent subtrees.

Retain this branch's projected-action quotient and bottom-up wildcard-vector
evaluator. Recalibrate projected depth only after the certificate and parallel
search have been verified together.

The implementation is deliberately split into small commits. Every commit
must leave the tree buildable, preserve a usable serial path, receive a code
review, and pass its stated smoke checks before its phase is marked complete.

The primary production performance target is top-1,000,000. Smaller top-N
runs remain useful for quick smoke tests, but they are not sufficient evidence
that the parallel output lock or projected-depth policy suits the expected
workload.

The architectural rationale and measurements live in
`findings/dfs-codex-1-integration.md`. The reviewed source for the independent
parallel implementation is `../nutrimatic` commit `30561e0`.

## Scope

### Included

- Refactor concrete DFS mutation into a per-worker context without changing
  serial behavior.
- Build a length-only completion bound directly from the bag-restricted
  `DfsClassList`.
- Skip whole contiguous equal-length groups when their score envelope cannot
  reach the output floor.
- Give sinks an explicit concurrent-call contract.
- Make `DfsTopN` safe for concurrent `emit()` and `score_floor()` calls.
- Generate shallow concrete-search tasks and distribute them with an atomic
  cursor.
- Expose explicit search-thread control and diagnostics.
- Re-run the projected-depth calibration with the new search costs.

### Excluded

- The independent branch's recursive projected evaluator, support-group
  builder, modular fallback experiments, and projected certificate
  experiments.
- Score-descending certificate suffix traversal. It changes class order and
  sometimes regresses despite skipping more scans.
- Block-max certificate metadata. That is a follow-up after the group-only
  certificate establishes a new baseline.
- Persistent projected tables, corpus-static tables, background rich-bound
  construction, static wildcard splitting, and support/multiplicity indexes.
- Parallel `walk_unoptimized()`.
- Parallel lazy construction for `SCORE_BOUND_PREFIX`.

## Branch-specific decisions

These decisions adapt `30561e0` to the code that actually exists here.

1. **Keep the current projected representation.**
   `ProjectedAction`, `prepare_projected_actions()`, and
   `compute_projected_score_bound_bottom_up()` remain the projected-score
   implementation. Do not replace them with structures from the independent
   branch.

2. **Build the length bound from concrete classes.**
   For each consumed length, retain the largest
   `best_member_log_scores[class_index]` from this query's extracted class
   list. This is independent of projected depth and is equivalent to using
   the quotient for a length-only maximum.

3. **Preserve original class order.**
   `DfsClassList` already sorts every rarest-rank bucket by decreasing key
   length and then key. Equal-length groups are therefore contiguous. The
   first implementation may skip a group but must not permute its surviving
   classes.

4. **Keep parallel search opt-in initially.**
   Add `--search-threads N` with a default of `1`. Do not silently change
   default CPU use until combined benchmarks pass. An automatic policy is a
   final calibration decision, not part of the scheduler commit.

5. **Use an explicit library parameter.**
   Add a `search_threads` constructor argument after
   `preprocess_threads`, defaulted to one. The CLI resolves and passes it.
   Do not make the library's behavior depend solely on an environment
   variable.

6. **Retain one internal task-count override while tuning.**
   The default target is `64 * search_threads` independent subtrees, capped
   safely against overflow. `NUTRIMATIC_SEARCH_TASKS` may override it for
   smoke tests and benchmarks. It is not a supported CLI interface.

7. **Fix partial thread-creation accounting during the port.**
   `actual_search_threads` must be the caller plus successfully launched
   workers, not the requested count. Only started worker contexts are merged.

8. **Do not invent modular worker state.**
   This branch has no concrete-search modular signature. `SearchWorker` needs
   the current bag, bag mask, score key, path, counters, progress state, and
   task-generation state. If modular bounds are added later, their residual
   signature must be worker-local as in `30561e0`.

9. **Parallel-safe means only the documented calls.**
   The sink capability covers concurrent `emit()` and `score_floor()`.
   `DfsTopN::size()`, `spellings_expanded()`, and
   `take_sorted_results()` are called only after search workers have joined.

10. **Cutoff spelling ties are explicitly out of scope.**
    The expected production use retains roughly one million results, so which
    equally scoring spelling occupies the final slot is not a requirement.
    Keep the current numeric `<= floor` pruning and score-only cutoff
    admission. Do not add work or synchronization to stabilize fringe ties
    for tests. Parallel and serial validation compares the exact prefix above
    the cutoff-score bucket and treats differences inside that final bucket as
    acceptable.

11. **Retention depends on the million-result workload.**
    The shared top-N mutex was inexpensive in the independent top-1,000
    measurement, but top-1,000,000 can make output expansion and heap updates
    a much larger fraction of runtime. Do not select default threads or claim
    the integrated performance result until a million-result run shows the
    scheduler still provides useful wall-time improvement.

## Status and commit protocol

Use these status values in the phase headings:

```text
[ ] pending
[-] in progress
[x] complete
```

For each implementation phase:

1. Change the phase status to `[-]`.
2. Implement only that phase's scope.
3. Run the phase's build and smoke verification.
4. Invoke `/review` and review the complete diff before committing.
5. Resolve every correctness finding and rerun affected checks.
6. Run `git diff --check`.
7. Mark the phase `[x]`, including its verification checkboxes, in the same
   diff.
8. Commit the implementation, tests, and status update together using the
   proposed subject.

Do not mark a phase complete merely because the code was written. A completed
phase means its review and verification gates passed and its commit was
created. If a phase needs multiple commits, split it in this plan before
continuing rather than hiding an unreviewed intermediate state.

The worktree may contain unrelated or concurrent changes. Before every commit,
inspect `git status --short`, stage only the phase's files, and preserve all
unrelated edits.

## Phase status

| Phase | Deliverable | Status | Proposed commit |
|---:|---|:---:|---|
| 0 | Baseline contract | [x] | `Record DFS integration baseline` |
| 1 | Serial worker refactor | [x] | `Isolate concrete DFS state` |
| 2 | Group-only length certificate | [x] | `Certify concrete DFS length groups` |
| 3 | Concurrent top-N sink | [x] | `Make DFS top-N thread-safe` |
| 4 | Parallel concrete DFS scheduler | [ ] | `Parallelize concrete DFS search` |
| 5 | Integrated validation | [ ] | `Record DFS integration results` |
| 6 | Depth and thread policy | [ ] | `Recalibrate projected DFS` |

## Common setup

Use the repository instructions for every build:

```bash
source ~/code/nutrimatic/.env/bin/activate
conan build .
```

Use the configured index for corpus-backed validation:

```bash
export IDX=~/code/nutrimatic/idx/wiki-merged.5.index
```

Load the S6 benchmark letters with:

```bash
source ./s.sh
```

Before every timed `dfs-anagrams` run, verify that another instance is not
competing for the machine:

```bash
pgrep -af '(^|/)dfs-anagrams( |$)'
```

An empty result is the timing precondition. Do not kill another session.
Wait, or run only non-timing checks.

Unless a phase says otherwise, the smoke suite is:

```bash
./build/test-dfs-search
./build/test-dfs-output
./build/test-dfs-class-list
IDX="$IDX" ./build/test-dfs-search --validate-14
```

Keep tests focused on invariants and small synthetic indexes. Long S6 runs are
benchmark gates, not unit tests.

## Phase 0 — freeze the baseline and integration contract [x]

### Purpose

Record enough baseline information to distinguish a functional regression
from expected changes in traversal order or pruning. Commit the findings and
this plan before source work begins.

### Files

- `findings/dfs-codex-1-integration.md`
- `plans/dfs-codex-1-integration.md`

### Tasks

- [x] Confirm the findings refer to independent commit `30561e0`.
- [x] Record the current branch head and `git status --short`.
- [x] Build the untouched source.
- [x] Run the common smoke suite.
- [x] Capture one serial 40-letter, top-10,000 reference at the established
      projected depth. Preserve complete stdout in the ignored
      `results/` directory so Phase 5 can inspect the cutoff bucket and compare
      rows above it:

  ```bash
  mkdir -p results
  BASELINE_OUTPUT=results/dfs-codex-1-baseline.stdout
  BASELINE_LOG=results/dfs-codex-1-baseline.stderr
  NUTRIMATIC_PROJECTED_SCORE_D=15 \
    ./build/dfs-anagrams "$IDX" "${S6:0:40}" \
      -n 10000 -C 1024 -F -p 10000 \
      >"$BASELINE_OUTPUT" 2>"$BASELINE_LOG"
  test "$(wc -l <"$BASELINE_OUTPUT")" -eq 10000
  sha256sum "$BASELINE_OUTPUT"
  head -n 9900 "$BASELINE_OUTPUT" | sha256sum
  tail -n 1 "$BASELINE_OUTPUT"
  ```

- [x] Confirm the `dfs-anagrams` command itself exits successfully before
      accepting either hash. Do not use a live `dfs-anagrams | head` pipeline,
      because early pipe closure can hide an upstream failure.
- [x] Record the artifact path and row count, full-output SHA-256, SHA-256 of
      the first 9,900 output rows, printed cutoff score, setup time, search
      time, DFS nodes, solutions, spellings expanded, retained results, and
      projected work counters in the implementation log section at the end of
      this file. Retain the ignored output artifact through Phase 5.
- [x] Review both documents for agreement with the current source.
- [x] Run `git diff --check`.

### Completion gate

The baseline build and smoke checks pass, the reference output hash is
recorded, and the baseline/status update is committed with:

```text
Record DFS integration baseline
```

Do not change production source in this commit.

## Phase 1 — isolate serial concrete-search worker state [x]

### Purpose

Make the existing optimized DFS operate through an explicit per-worker state
without adding threads or changing traversal. This separates mechanical state
movement from synchronization and makes later certificate counters
worker-local from their first introduction.

### Files

- `source/dfs-search.h`
- `source/dfs-search.cpp`
- `source/test-dfs-search.cpp`
- this plan's phase status

### Data structure

Add a private `SearchWorker` containing:

```text
bag[DFS_SYMBOL_COUNT]
bag_mask
score_key
path
nodes
solutions
bound_prunes
next_progress
reported_solutions
```

Keep the query's root bag, root mask, and root score key in
`DfsAnagramSearch`; bound preprocessing still uses the existing root/shared
fields. Do not force `BoundWorker` and `SearchWorker` into one type: their
lifetimes and counters differ.

### Refactor

- [x] Add `start_search_worker()` to copy the root bag, mask, and score key,
      reserve the path, and zero counters.
- [x] Add `merge_search_worker()` to transfer final counters into the public
      search aggregates.
- [x] Change optimized-search helpers to accept `SearchWorker*`:
      `walk()`, `visit_fitting_class()`, `should_prune()`,
      `hot_class_fits()`, and `hot_class_multiplicity_fits()`.
- [x] Use `worker->score_key` for bound lookup and lazy-prefix decisions.
- [x] Keep all subtraction/restoration paired on the worker's bag, mask, and
      score key.
- [x] Move optimized path access to `worker->path`.
- [x] Keep `walk_unoptimized()` serial and on its existing object state for
      now. Its behavior is the fallback when hot-class preparation fails.
- [x] Run exactly one `SearchWorker` from `run()` and merge it afterward.
- [x] Preserve current progress text and cadence in serial mode.

### Invariants to review

- Every successful subtraction restores the same bag counts, mask, score key,
  path length, and entry point before the next candidate.
- The first class still omits `restart_log_rate`; every later class includes
  it exactly once.
- `SCORE_BOUND_PREFIX` still computes a missing prefix synchronously using the
  same residual state.
- Public node, solution, and bound-prune totals are unchanged.
- No shared field is accidentally read in an optimized recursive call after
  its worker-local replacement exists.

### Verification

- [x] Build successfully.
- [x] Run `test-dfs-search`.
- [x] Run `test-dfs-output`.
- [x] Run `test-dfs-search --validate-14` with `IDX`.
- [x] Diff a small CLI run before and after byte-for-byte.
- [x] Confirm identical score-bound mode, state, transition, node, solution,
      and output counters on the synthetic bounded search.
- [x] `/review` reports no unresolved findings.
- [x] `git diff --check` passes.

### Completion gate

Serial outputs and counters are unchanged and the reviewed refactor is
committed as:

```text
Isolate concrete DFS state
```

## Phase 2 — add the group-only length certificate [x]

### Purpose

Skip a contiguous equal-length class group before multiplicity checks when no
class in the group, followed by any length-feasible completion, can reach the
sink's numeric score floor.

The certificate is exact and query-specific. It does not depend on projected
depth or on whether a rich projected table was selected.

### Files

- `source/dfs-search.h`
- `source/dfs-search.cpp`
- `source/dfs-anagrams.cpp`
- `source/test-dfs-search.cpp`
- `source/test-dfs-class-list.cpp` only if an ordering assertion is needed
- this plan's phase status

### Length-only tail bound

Add:

```text
best_score[length] = maximum representative class score consuming length
U[0] = 0
U[left] = max(best_score[length] + restart_log_rate + U[left - length])
```

Build `best_score` from `DfsClassList::classes()` and
`best_member_log_scores`, not from `ProjectedAction`. Inflate each finite
`U[left]` conservatively using the existing rounding-error helpers. A missing
completion remains `-HUGE_VAL`.

The table has `letters.size() + 1` entries and is prepared after hot classes
exist but independently of projected table selection.

### Group metadata

For every `(rarest rank, consumed length)`, store:

```text
maximum representative class score
exclusive end index of the contiguous class group
```

Use a stride of `maximum class key length + 1`. A group start need not be
stored because the ordinary traversal already supplies its current index.
Do not allocate `certificate_order` or reorder classes.

Add setup time and byte-count accessors plus worker-local counters for:

```text
group tests
groups rejected
candidate scans skipped
candidate scans kept
```

### Rejection rule

At a non-root DFS node with a published sink floor, a group consuming `L`
letters is rejectable when the conservatively padded value

```text
representative_log_score
  + restart_log_rate
  + group_max_score[L]
  + U[letters_left - L]
```

cannot exceed the floor.

Use long-double accumulation and the same depth-scaled `DBL_EPSILON` padding
contract as `should_prune()`. Retain the existing `<= floor` cutoff semantics:
equal-score spellings at the Nth position are interchangeable for this
integration.

Do not apply this formula at the root: the first selected class does not pay a
restart. Root scanning is small and leaving it unchanged avoids a second
formula.

### Traversal

- [x] Add `walk_certified()` for the existing rarest-rank range.
- [x] Enter it only when the certificate is ready, the path is nonempty, and
      `score_floor()` returns a floor.
- [x] Advance rejected groups directly to their stored exclusive end.
- [x] Traverse surviving groups in their original class-index order.
- [x] Retain the entry-point tie-break by starting at
      `max(first_length_candidate(...), entry_point)`.
- [x] Keep certificate tables immutable once concrete search starts.

### Activation and A/B control

Enable group skipping by default whenever:

- hot-class preparation succeeded;
- the sink supports score pruning;
- score-bound arithmetic is supported; and
- certificate allocation succeeded.

The certificate may remain active even when the rich score cache is off or
does not fit. Add a temporary integration override:

```text
NUTRIMATIC_LENGTH_CERTIFICATE=0       disable
NUTRIMATIC_LENGTH_CERTIFICATE=shadow  count decisions, do not skip
unset                                   enable skipping
```

Invalid values should fail safe by disabling the experiment or selecting the
documented default; they must not partially initialize tables.

### Diagnostics

After phase 2, print one compact diagnostic when the certificate was prepared:

```text
mode, table bytes, preparation seconds,
group tests/rejects, scans kept/skipped
```

Do not add per-node output.

### Focused tests

- [x] A synthetic top-N search produces identical spellings with the
      certificate disabled, shadowed, and active.
- [x] Disabled and shadow modes have identical node, solution, and traversal
      counters.
- [x] Exercise the group-rejection predicate at floors below, equal to, and
      above its conservatively padded envelope.
- [x] Do not require equal aggregate group-decision counters from shadow and
      active modes: active skipping removes downstream nodes that shadow mode
      still visits.
- [x] Active mode skips at least one whole group on the synthetic fixture.
- [x] A sink without a floor never claims certificate skips.
- [x] Score cache disabled (`-C 0 -F` equivalent in the library) does not
      disable the certificate.
- [x] A non-nearest floating-point rounding mode disables the certificate,
      matching the rich-bound guard.
- [x] Original class order is unchanged.

### Verification

- [x] Build successfully.
- [x] Run the common smoke suite.
- [x] Compare certificate disabled, shadow, and active on a short S6 prefix;
      stdout hashes must match.
- [x] Confirm the active run reduces candidate scans without increasing DFS
      nodes.
- [x] `/review` checks the recurrence, restart placement, rounding padding,
      group boundaries, entry point, and failure cleanup.
- [x] `git diff --check` passes.

### Completion gate

The certificate is exact in synthetic and corpus-backed differentials,
produces useful skips, retains original class order, and is committed as:

```text
Certify concrete DFS length groups
```

## Phase 3 — make `DfsTopN` concurrency-safe [x]

### Purpose

Establish and test the shared sink contract before any search worker calls the
sink concurrently.

### Files

- `source/dfs-search.h`
- `source/dfs-output.h`
- `source/dfs-output.cpp`
- `source/test-dfs-output.cpp`
- `source/test-dfs-search.cpp`
- this plan's phase status

### Sink contract

Add:

```cpp
virtual bool supports_parallel_search() const { return false; }
```

Its comment must say that opting in guarantees concurrent `emit()` and
`score_floor()` calls. Existing sinks remain serial by default. `DfsTopN`
opts in only after the synchronization below exists.

### `DfsTopN` synchronization

- [x] Protect the heap, positions map, and `expanded` mutation with one mutex.
- [x] Hold that mutex for the full spelling expansion in `emit()`. This keeps
      the pending expansion's repeated floor checks coherent and avoids
      exposing internal heap operations.
- [x] Publish the numeric floor separately as atomic bits plus an atomic
      full flag.
- [x] Store floor bits before a release-store of the full flag; readers
      acquire-load the flag before loading bits.
- [x] Republish after every operation that may strengthen a full heap.
- [x] Preserve the invariant that the published floor never decreases while
      a populated search is active. A stale lower value may cause extra work
      but cannot over-prune.
- [x] In `take_sorted_results()`, acquire the heap mutex, clear the published
      full flag, and swap out both heap and positions before sorting.
- [x] State in comments that draining is not concurrent with an active search.
- [x] Leave `size()` and `spellings_expanded()` as post-join observers; do not
      pay synchronization for calls the contract does not permit.

Use bit conversion through `memcpy`, matching the existing atomic score-bound
style. Add static assertions if needed to document the atomic word size.

### Focused tests

- [x] Existing serial `DfsTopN` tests remain byte-identical.
- [x] Several threads can call `emit()` on one `DfsTopN`; using unique scores
      or a limit that does not cut a tie group, the final retained set matches
      a serial reference.
- [x] Concurrent `score_floor()` calls never observe a floor greater than the
      final floor or a decreasing sequence after publication.
- [x] A generic collecting sink reports no parallel capability.
- [x] Drain a full `DfsTopN`, refill it with lower scores, and verify it
      accepts results until the new heap becomes full.
- [x] A zero-limit sink remains inert.

### Review focus

- All heap and positions accesses during permitted concurrent calls are under
  the mutex.
- The release/acquire pair publishes initialized floor bits.
- A relaxed floor-bit load can only be stale in the safe direction.
- Reset happens while holding the same mutex as `emit()`.
- No fast path reads `full()` or `floor_log_score()` without either the mutex
  or the atomic publication.

### Verification

- [x] Build successfully.
- [x] Run `test-dfs-output`.
- [x] Run `test-dfs-search`.
- [ ] Optionally run the focused tests under ThreadSanitizer if a compatible
      build is already available; this is not a substitute for review.
- [x] `/review` reports no unresolved race or publication finding.
- [x] `git diff --check` passes.

### Completion gate

The concurrency contract is explicit, `DfsTopN` satisfies it, sink reuse is
safe, and the change is committed as:

```text
Make DFS top-N thread-safe
```

## Phase 4 — parallelize concrete DFS [ ]

### Purpose

Split optimized concrete traversal into shallow independent subtrees, process
them with a bounded set of workers, and merge worker-local counters after all
threads join.

### Files

- `source/dfs-search.h`
- `source/dfs-search.cpp`
- `source/dfs-anagrams.cpp`
- `source/test-dfs-search.cpp`
- possibly `source/meson.build` only if the existing thread dependency is not
  already attached to every affected target
- this plan's phase status

### Public interface and diagnostics

- [ ] Extend `DfsAnagramSearch` with a `search_threads` constructor argument
      defaulted to one.
- [ ] Add `--search-threads N` and short form `-S` to `dfs-anagrams`.
- [ ] Require `N >= 1`; default to one in this commit.
- [ ] Add `search_threads_used()` and `search_tasks_generated()` accessors.
- [ ] Report the actual worker count and task count when more than one worker
      was launched.
- [ ] Keep preprocessing and search thread controls independent.

The Meson thread dependency already exists for DFS preprocessing; verify it
rather than adding duplicate linkage.

### Task representation

Add an inline `SearchTask` containing the complete residual state needed to
resume:

```text
bag[DFS_SYMBOL_COUNT]
bag_mask
score_key
path indexes through the split depth
path_size
entry_point
letters_left
representative_log_score
```

Use a small fixed maximum split depth, initially six as in `30561e0`. Assert
that the inline path is large enough before publishing a task.

### Task generation

- [ ] Start from an ordinary initialized `SearchWorker`.
- [ ] At the selected shallow depth, publish a materialized task instead of
      descending.
- [ ] Begin at depth one, then expand queued frontier tasks breadth-first
      until at least `64 * requested threads` live tasks exist, the frontier
      is exhausted, or maximum split depth is reached.
- [ ] Preserve `entry_point` exactly so canonical class-permutation collapse
      remains valid after resumption.
- [ ] Count any shallow solutions found during task generation normally.
- [ ] Merge the serial seed worker's counters once.

Task generation is intentionally serial. Its job is to create enough coarse
work to amortize scheduling while retaining the ordinary traversal logic.

### Worker scheduler

- [ ] Cap the requested count at the number of live tasks.
- [ ] Allocate one `SearchWorker` per possible active worker.
- [ ] Use one relaxed atomic next-task cursor.
- [ ] Launch at most `worker_count - 1` background threads and run worker zero
      on the caller.
- [ ] Catch thread-construction failure, retain successfully launched
      workers, and let the caller/shared cursor finish all remaining tasks.
- [ ] Join every successfully created thread before reading worker counters or
      returning.
- [ ] Set `actual_search_threads` to `1 + launched background threads`.
- [ ] Merge only worker zero and successfully launched worker contexts.
- [ ] If no live task remains after frontier generation, return success
      without running the root search again.

No joinable thread may be abandoned on a failure path.

### Parallel eligibility

Use parallel concrete search only when all are true:

- requested search threads exceed one;
- hot-class preparation succeeded;
- the sink is null or `supports_parallel_search()` is true;
- score-bound mode is not `SCORE_BOUND_PREFIX`; and
- all active diagnostics and certificates use worker-local counters and
  immutable tables.

Otherwise run the one-worker serial path. In particular:

- a generic collecting sink falls back to serial;
- the group-only certificate is parallel-safe after Phase 2 because its
  tables are immutable and counters are worker-local;
- dense and completed projected bounds are read-only during concrete search;
  and
- lazy prefix construction remains serial.

### Progress synchronization

- [ ] Keep worker node and solution counters local.
- [ ] Use atomics only to aggregate progress intervals.
- [ ] Protect `fprintf()` and `fflush()` on the shared progress stream with one
      mutex.
- [ ] Merge exact final totals after joining; progress lines may be approximate
      snapshots, final public counters may not.
- [ ] Do not put a mutex or atomic increment in the per-node hot path when
      progress is disabled.

### Focused tests

- [ ] A non-opted-in collecting sink remains serial even when four threads are
      requested.
- [ ] `DfsTopN` uses more than one worker on a small fixture with the task
      target overridden low.
- [ ] On a fixture with no cutoff-score tie, serial and parallel searches
      retain identical spellings and scores.
- [ ] Serial and parallel searches report the same result for certificate
      disabled and active modes, using the same tie-aware comparison.
- [ ] Parallel search works with score bounds off.
- [ ] `SCORE_BOUND_PREFIX` falls back to one search worker.
- [ ] `search_tasks_generated()` is nonzero when parallel work is used.
- [ ] Repeated parallel runs preserve the same cutoff score; spellings tied
      exactly at that cutoff may differ.
- [ ] Progress output remains well-formed rather than interleaved.

Node and solution counters may differ between serial and parallel top-N runs
because the shared floor strengthens at a different time. Output and score
correctness are the gate. For exhaustive runs without a floor, counters should
match exactly.

### Verification

- [ ] Build successfully.
- [ ] Run the common smoke suite.
- [ ] Run serial/parallel corpus differentials at top-1,000 and on the chosen
      top-1,000,000 reference workload.
- [ ] On a fixture with no cutoff-score tie, compare all retained output
      byte-for-byte.
- [ ] On corpus runs, compare the exact prefix above the cutoff-score bucket
      and verify the same cutoff score and retained count.
- [ ] Confirm the requested, actual, and task diagnostics are truthful.
- [ ] Repeat the parallel smoke enough times to expose scheduling-sensitive
      failures.
- [ ] `/review` checks sink gating, task completeness, entry-point
      preservation, restoration, thread-creation failure, joins, progress
      locking, and counter merging.
- [ ] `git diff --check` passes.

### Completion gate

All tasks are searched exactly once, unsupported modes fall back safely, and
serial and parallel output agrees except for accepted spellings tied exactly
at the final score. The reviewed scheduler is committed as:

```text
Parallelize concrete DFS search
```

## Phase 5 — validate the integrated certificate and scheduler [ ]

### Purpose

Establish correctness and the combined wall-time effect before changing
automatic policies.

### Correctness matrix

Run at least these modes on one medium corpus workload:

| Certificate | Search threads | Expected use |
|---|---:|---|
| disabled | 1 | original serial baseline |
| active | 1 | certificate-only |
| disabled | chosen multicore count | parallel-only |
| active | chosen multicore count | integrated path |

Hold projected depth, preprocessing threads, cache size, minimum length, and
top-N fixed across the matrix.

For each mode capture:

- full stdout SHA-256 as a diagnostic, not an equality requirement;
- a SHA-256 of the prefix above the final cutoff-score bucket;
- retained spelling count and scores;
- setup and search seconds;
- DFS nodes and solutions;
- certificate group tests/rejects and scans kept/skipped;
- requested/actual search threads and tasks;
- user time, wall time, and peak RSS.

The required correctness result is:

- the same retained count;
- the same numeric cutoff score;
- identical sorted rows whose score is strictly above the cutoff; and
- differences, if any, confined to spellings tied exactly at the cutoff.

Parallel node counts may differ slightly because floor publication order
changes.

For the 10,000-row CLI baseline comparison, hashing the first 99% of output
is a useful quick signal. `dfs-anagrams` already prints retained results in
descending score order with deterministic tie ordering:

```bash
head -n 9900 output.txt | sha256sum
```

It is not the correctness definition and must not influence production code.
The stronger comparison removes the complete final cutoff-score bucket. Since
CLI scores are printed at limited precision, a CLI-only comparison may
conservatively remove every row sharing the last printed score. Focused C++
tests should compare exact `DfsSpelling::log_score` values.

### Reference workload

First rerun the exact Phase 0 workload after integration. Use one search thread
with `NUTRIMATIC_LENGTH_CERTIFICATE=0` for the unchanged serial path, then run
the other correctness-matrix cells with the same letters, depth, cache,
progress factor, and top-N:

```bash
NUTRIMATIC_PROJECTED_SCORE_D=15 \
  ./build/dfs-anagrams "$IDX" "${S6:0:40}" \
    -n 10000 -C 1024 -F -p 10000 --search-threads N \
    >results/dfs-codex-1-integrated-MODE.stdout \
    2>results/dfs-codex-1-integrated-MODE.stderr
```

Prefix the disabled cells with `NUTRIMATIC_LENGTH_CERTIFICATE=0`; leave the
variable unset for active cells. Replace `MODE` with a distinct matrix-cell
name. The disabled one-thread output must match the complete Phase 0 artifact
byte-for-byte. For the other cells, apply the cutoff-tie comparison above and
compare their first-9,900-row smoke hashes with the Phase 0 prefix hash.

The following two commands are additional performance gates, not alternate
baseline parameters. Their explicit depths reproduce previously measured
workloads: `d=13` is the established 38-letter quick differential, while
`d=12` is only the starting point for finding a practical million-result
reference. Neither depth is an automatic policy decision.

First run the established 38-letter quick differential:

```bash
NUTRIMATIC_PROJECTED_SCORE_D=13 \
  ./build/dfs-anagrams "$IDX" "${S6:0:38}" \
    -m 4 -n 1000 -C 32 -F -T 20 --search-threads N
```

Use `N=1` and the chosen explicit multicore count. The independent branch used
20 search threads and previously retained output hash:

```text
398abaeaeb5245dfe071f1f11d933742591230b685a6be2ac724abc35a4ffec4
```

The old hash is a useful serial reference, but a parallel run may select
different spellings tied at its final score. Compare the strict-above-cutoff
prefix and the numeric cutoff instead of changing production tie handling to
recover the hash.

Then select a practical million-result reference, starting with:

```bash
NUTRIMATIC_PROJECTED_SCORE_D=12 \
  ./build/dfs-anagrams "$IDX" "${S6:0:28}" \
    -m 4 -n 1000000 -C 32 -F -T 20 --search-threads N
```

If that bag exhausts before retaining one million spellings, increase the S6
prefix length and recalibrate its explicit depth before using it as the gate.
Use the same bag and depth for all serial/parallel/certificate matrix cells.
Capture output separately from timing diagnostics so writing one million rows
is included consistently or excluded consistently in paired comparisons.

### Thread and task sweep

Sweep search threads:

```text
1, 2, 4, 8, 10, 20
```

At the best two thread counts, sweep tasks per thread around:

```text
16, 32, 64, 128
```

Prefer the smaller count when median wall times are within 5%. Repeat close
winners three times and use the median. Record aggregate CPU so a latency win
with disproportionate work remains visible.

### Retention gates

- [ ] Every matrix output matches above the cutoff-score bucket.
- [ ] Every matrix run has the same retained count and cutoff score.
- [ ] The certificate skips a material fraction of class scans.
- [ ] Parallel search improves concrete-search wall time by at least 2x on the
      38-letter reference.
- [ ] Parallel search provides a useful wall-time improvement on the
      top-1,000,000 reference; a top-1,000-only win is insufficient.
- [ ] On the exact 40-letter, top-10,000 command, the combined phase-2 time
      improves over the Phase 0 serial baseline.
- [ ] Peak RSS remains within the expected certificate tables plus shallow
      task/worker storage.
- [ ] Aggregate CPU growth is recorded and judged acceptable.
- [ ] No concurrent `dfs-anagrams` process contaminated timings.
- [ ] Results are added to `findings/dfs-codex-1-integration.md`.
- [ ] `/review` verifies the recorded conclusions match the raw counters.
- [ ] `git diff --check` passes.

### Completion gate

The integrated path is correct and materially faster, the findings contain
the measured result, and the documentation/status update is committed as:

```text
Record DFS integration results
```

If the performance gates fail, keep the correct implementation explicitly
opt-in, record the failed gate, and do not proceed to automatic policy. If the
million-result profile identifies the shared output lock as the ceiling,
create and review a separate batching or local-heap plan before changing the
sink design; do not add speculative synchronization complexity to this port.

## Phase 6 — recalibrate projection depth and default threading [ ]

### Purpose

Certificates reduce candidate scans and parallel search reduces the wall cost
of remaining nodes. Both make projected setup relatively more expensive.
Replace the old setup-versus-search assumptions with measurements of the
integrated implementation.

### Depth matrix

Use process-gated runs with fixed evaluator, cache, and preprocessing policy.
At minimum measure:

| Workload | Top-N | Depth neighborhood |
|---|---:|---|
| `${S6:0:40}` | 1 | 11, 12, 13, 14 |
| `${S6:0:40}` | 10 | 12, 13, 14 |
| `${S6:0:38}` | 1,000 | 12, 13, 14, 15 |
| million-result reference | 1,000,000 | neighbors of its serial winner |

The million-result reference is the largest practical S6 prefix selected in
Phase 5 that actually retains 1,000,000 spellings.

For each point capture both one search thread and the chosen multicore count
unless a point's setup already exceeds the best complete phase-2 time.

Record:

- projected state and action counts;
- setup, search, phase-2, and whole-program time;
- DFS nodes;
- certificate scan counters;
- output hash;
- user/system time and RSS.

Stop a deeper sweep when its target-independent setup already exceeds the
best complete shallower phase-2 result.

### Selector change

Do not implement an elaborate predictive model from three bags. Choose the
smallest defensible production change:

1. retain `NUTRIMATIC_PROJECTED_SCORE_D` as the exact override;
2. update the automatic starting depth only where the measurements establish
   a clear neighboring winner;
3. include top-N in the selector input, including a million-result calibration
   bucket, when the measured winners differ; and
4. prefer a shallower depth when phase-2 medians are within 5%.

Put selector logic in a named helper with small table-driven tests. Do not
bury new thresholds inside the allocation loop.

### Default search-thread decision

Parallel search remains explicit unless all are true:

- the million-result reference shows a clear latency win;
- short-input overhead is negligible or guarded by a threshold;
- `hardware_concurrency()` is capped to the measured useful range; and
- generic sinks and prefix-bound mode still fall back to serial.

If those gates pass, define `--search-threads 0` as automatic and consider it
the CLI default. Keep the `DfsAnagramSearch` constructor default at one so
library callers do not acquire hidden concurrency.

If the gates do not pass, retain CLI default one and document the recommended
explicit count for long top-N searches.

### Verification

- [ ] Build successfully.
- [ ] Run the common smoke suite.
- [ ] Run selector unit tests at threshold boundaries and top-N boundaries.
- [ ] Verify explicit depth overrides are unchanged.
- [ ] Verify explicit `--search-threads 1` remains serial.
- [ ] Verify automatic mode, if enabled, reports the actual count.
- [ ] Re-run the winning calibration points after the policy change.
- [ ] Update the findings with the new depth/thread recommendations.
- [ ] `/review` checks that policy claims are supported by the recorded
      measurements.
- [ ] `git diff --check` passes.

### Completion gate

The selector and thread policy reflect measured integrated costs, preserve
explicit overrides and serial library behavior, and are committed as:

```text
Recalibrate projected DFS
```

## Final acceptance checklist

- [ ] Every phase is `[x]` and has a corresponding reviewed commit.
- [ ] `source ~/code/nutrimatic/.env/bin/activate && conan build .` passes.
- [ ] The common smoke suite passes.
- [ ] Serial and parallel output match above the final cutoff-score bucket
      and have the same retained count and cutoff score.
- [ ] The length certificate remains independent of projected depth.
- [ ] The bottom-up projected evaluator and action quotient remain in place.
- [ ] Unsupported sinks and lazy prefix bounds remain serial.
- [ ] `DfsTopN` reuse after draining is covered.
- [ ] Cutoff spelling ties remain an accepted, documented fringe difference;
      no implementation complexity was added to stabilize them.
- [ ] Actual worker diagnostics survive partial thread-creation failure by
      construction.
- [ ] Timing runs were process-gated.
- [ ] Findings contain final measurements rather than projected speedups.
- [ ] A final `/review` of the complete integration reports no unresolved
      correctness or concurrency findings.
- [ ] `git log` shows the intended small commit sequence.

## Implementation log

Fill this section as work proceeds. Do not replace raw measurements with only
speedup ratios.

### Baseline

```text
branch head:
  a4f05268885d28eb2a726915653b786cbbe686ae
initial git status --short:
  M plans/dfs-codex-1-integration.md
build:
  PASS — source /home/mike/code/nutrimatic/.env/bin/activate && conan build .
smoke tests:
  PASS — test-dfs-search, test-dfs-output, test-dfs-class-list,
  and IDX="$IDX" test-dfs-search --validate-14
40-letter, top-10,000 command:
  PASS (exit 0) — NUTRIMATIC_PROJECTED_SCORE_D=15,
  -n 10000 -C 1024 -F -p 10000
baseline output artifact:
  results/dfs-codex-1-baseline.stdout
retained output rows:
  10000
full stdout SHA-256:
  f8aae06669bcb31561134a2661ca8af71062ad378aae401ca53cb786161b4d5f
9,900-row stdout SHA-256:
  313c91e1f933abd8ff2d698e0453223c4e7d834f5c9b27dbfe1deddf59ab89e2
printed cutoff score:
  2.984e-34
setup:
  4.513798 s
search:
  251.849998 s
DFS nodes:
  3987952548
solutions:
  320681
spellings expanded/retained:
  149148 / 10000
projected work:
  7050240 bound entries, 12184378227 successful bound transitions,
  17491291 nextafter calls, 11154835508 candidate tests,
  12580372385 fitting transitions
```

### Phase commits

| Phase | Commit | Review | Verification notes |
|---:|---|---|---|
| 0 | `b3b9535` | complete | Build, common smoke suite, and 40-letter reference passed. |
| 1 | `3bcaa50` | complete | Build and focused smoke checks passed; full 40-letter output and counters matched Phase 0. |
| 2 | `96189fd` | complete | Common smoke suite passed; short S6 output matched in all modes and active skipped 319,698,260 scans. |
| 3 | `Make DFS top-N thread-safe` | complete | Focused serial, concurrent emit/floor, zero-limit, and drain/refill tests passed. |
| 4 | pending | pending | |
| 5 | pending | pending | |
| 6 | pending | pending | |

### Combined benchmark

| Certificate | Search threads | Setup | Search | Nodes | Scans skipped | Hash |
|---|---:|---:|---:|---:|---:|---|
| disabled | 1 | | | | | |
| active | 1 | | | | | |
| disabled | | | | | | |
| active | | | | | | |

### Calibration decision

```text
winning depths:
search-thread count:
task target:
automatic-policy decision:
evidence location:
```
