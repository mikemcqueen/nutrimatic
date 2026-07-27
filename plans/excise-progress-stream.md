# Plan: excise `progress_stream` in favor of an ambient diagnostic sink

## Goal

Stop threading a `FILE*` through constructors, `run()` signatures, and member
variables just so a class can emit a diagnostic line. Replace it with a single
process-wide sink that `dfs_diagnostic()` writes to, defaulting to off (as
today) so tests stay silent unless they opt in. Any class — `DfsTopN`
included, which today has no way to log at all short of adding a stream
member nobody else needs — gets to call `dfs_diagnostic(...)` with zero
plumbing.

## Why this is safe here (and might not be elsewhere)

A process-wide mutable sink is normally a smell: two independent callers can
stomp on each other's redirection. Two things about this codebase make it a
non-issue:

- `dfs-search.cpp` joins every worker thread (`background[i].join()` at
  `:2319`, `:2490`, `:2609`; `pool[i].join()` at `:2994`) before `run()`
  returns. The sink is set once before a run starts and never mutated while
  workers are reading it, so no synchronization is needed around the global
  itself.
- Every test's `check()` (`test-dfs-search.cpp:17`, `test-dfs-output.cpp:16`)
  calls `exit(1)` on failure rather than throwing. There is no unwind path
  where a test that redirected the sink leaves it pointed at a `FILE*` that's
  about to go out of scope — the process is already dead in that case.

This would need revisiting if the code ever became a library linked into
something that runs independent callers concurrently. It isn't, and nothing
in this plan makes that harder to fix later than it already is.

## Settled decisions

- **The sink defaults to off (`NULL`), not to `stderr`.** This preserves
  today's behavior exactly: `DfsAnagramSearch::run()`'s `progress` parameter
  already defaults to `NULL`, and the ~25 test call sites that call
  `.run(&sink)` with no second argument rely on that default to stay silent.
  Flipping the default to `stderr` would make every one of those tests start
  spraying phase-2 progress lines at the real terminal.
- **`dfs-anagrams.cpp` opts in once, at startup**, with
  `dfs_set_diagnostic_stream(stderr)`, instead of passing `stderr` down through
  `search.run(..., stderr, ...)` (`dfs-anagrams.cpp:371`) and every function
  that forwards it.
- **The setter returns the previous stream**, mirroring `signal()`. Tests that
  capture output via `tmpfile()` do:
  ```cpp
  FILE* const previous = dfs_set_diagnostic_stream(tmpfile());
  ...
  dfs_set_diagnostic_stream(previous);
  ```
  No new RAII guard class — two call sites don't earn one, and per the
  exit(1)-on-failure point above there's no exception-safety need for it.
- **`dfs_diagnostic()` keeps its no-forced-newline behavior.** It writes a
  `[HH:MM:SS] ` prefix and exactly what the format string produces — no more.
  `run()`'s preflight block (`dfs-search.cpp:1328-1447`) exploits this today,
  composing one line across a `dfs_diagnostic()` call and several raw
  `fprintf`/`fputc`/`fputs` calls against the same stream before terminating
  it with `fputc('\n', progress)`. That pattern is preserved verbatim; see
  below.
- **Hot-path checks keep reading a local `bool`, not the global.** `walk()`
  (`dfs-search.cpp:2766`) tests `progress_stream != NULL` on every node
  visited — the single hottest branch in the program. Replacing that with a
  function call through the global on every node would be a real regression.
  Instead, `run()` samples `dfs_diagnostic_stream() != NULL` once into a new
  member, `progress_enabled`, exactly where it currently seeds
  `progress_stream`. `start_search_worker()`'s
  `next_progress = progress_stream == NULL ? INT64_MAX : progress_interval`
  (`dfs-search.cpp:2854` area) becomes
  `next_progress = progress_enabled ? progress_interval : INT64_MAX`.
- **The three `FILE* progress` parameters on `compute_score_bound_parallel`,
  `compute_projected_score_bound_parallel`, and
  `compute_projected_score_bound_bottom_up`** (`dfs-search.h:272,278,280`)
  are pure pass-through plumbing from `run()` — they exist only so those
  functions can call `dfs_diagnostic(progress, ...)`. Drop the parameter;
  each fetches `FILE* const progress = dfs_diagnostic_stream();` as a local
  at its own top, preserving every existing `if (progress != NULL) { ... }`
  gate (some of those blocks compute values, like `minimum_mib`, that are
  only needed for the message, so the gate stays worth keeping — this is a
  routing change, not a "always compute, sometimes print" change).
- **Drop the manual `fflush()` calls that follow `dfs_diagnostic()`.**
  `dfs_diagnostic()` already flushes internally (`dfs-diagnostic.cpp:63`), so
  every `dfs_diagnostic(...); fflush(progress_stream);` pair still in
  `dfs-search.cpp` (`:2877`, `:3050`, and inside the three bound-computation
  functions) is redundant today, not just after this change. Fold that
  cleanup in since the same lines are being touched anyway. The raw
  `fflush(progress)` calls that terminate a *composed* line (e.g.
  `dfs-search.cpp:1382`, `:1447`) stay — those flush after raw `fprintf`/
  `fputc` writes that bypass `dfs_diagnostic()` and so aren't covered by its
  internal flush.
- **`dfs-anagrams.cpp`'s two `fflush(stderr)` calls** (`:363`, `:417`) are
  out of scope. They order stderr diagnostics ahead of subsequent stdout
  output, not diagnostic-flushing — a different concern from what this plan
  touches.

## Target API

```cpp
// dfs-diagnostic.h

// Returns the previous stream, so callers that want to restore it can do so
// with no separate save step. NULL disables dfs_diagnostic() entirely.
FILE* dfs_set_diagnostic_stream(FILE* stream);

// The stream dfs_diagnostic() currently writes to (possibly NULL). For the
// rare caller that composes a line across multiple raw fprintf/fputc calls
// instead of a single dfs_diagnostic() call.
FILE* dfs_diagnostic_stream();

void dfs_diagnostic(char const* format, ...)
    __attribute__((format(printf, 1, 2)));
```

`dfs-diagnostic.cpp` gains a file-scope `FILE* g_diagnostic_stream = NULL;`
that both functions read/write; `dfs_diagnostic()`'s body is unchanged except
for reading `g_diagnostic_stream` instead of a parameter.

## Phases

Each phase keeps the tree buildable and gets a `/code-review` before commit.

### Phase 1 — add the ambient sink

Add `dfs_set_diagnostic_stream()` / `dfs_diagnostic_stream()` to
`dfs-diagnostic.{h,cpp}` and drop the `FILE* stream` parameter from
`dfs_diagnostic()`. This alone doesn't compile until every call site is
updated, so phase 1 and 2 land as one commit in practice — listed separately
here because they're different kinds of edit.

Smoke check: none yet: nothing calls the new signature until phase 2.

### Phase 2 — migrate `dfs-search.{h,cpp}`

- `DfsAnagramSearch::run()` (`dfs-search.h:61`, `dfs-search.cpp:1122`): drop
  the `FILE* progress = NULL` parameter. At the top, add
  `FILE* const progress = dfs_diagnostic_stream();` (for the composed-line
  preflight block) and set the new `progress_enabled` member from
  `progress != NULL`.
- `dfs-search.h:365`: replace `FILE* progress_stream;` with
  `bool progress_enabled;`; update the constructor initializer
  (`dfs-search.cpp:509`) from `progress_stream(NULL)` to
  `progress_enabled(false)`.
- `walk()` (`:2766`), `start_search_worker()` (`:2854` area),
  `walk_unoptimized()` (`:3047`): swap `progress_stream != NULL` /
  `progress_stream == NULL` for `progress_enabled` / `!progress_enabled`.
- `report_search_progress()` (`:2858`) and `walk_unoptimized()`'s inline
  progress block (`:3048`): call `dfs_diagnostic("phase 2: %lld nodes, ...")`
  with no stream argument; delete the trailing `fflush(progress_stream)`.
- `run_parallel_search()` (`:2892` onward): drop the `progress_stream`
  argument from each `dfs_diagnostic(progress_stream, ...)` call — the
  `if (verbose)` gates already present from the last commit are unchanged.
- The three bound-computation functions (`dfs-search.h:272,278,280`;
  bodies at `dfs-search.cpp:1990`, `2408`, `2530`): drop the `FILE* progress`
  parameter, add the same local-fetch line, update their three call sites
  inside `run()` (`:1454`, `:1456`, `:1462` area) to drop the trailing
  argument.
- Preflight block (`:1328-1447`): every `dfs_diagnostic(progress, ...)`
  becomes `dfs_diagnostic(...)`; the raw `fprintf(progress, ...)`, `fputc('\n',
  progress)`, `fputs(..., progress)`, and the two structural
  `fflush(progress)` calls that terminate composed lines are unchanged
  (they already use the local `progress` this phase introduces).

Smoke check: `-v` run against a small letter set still prints the same
preflight block, task-splitting messages, and periodic node counts to stderr,
byte-for-byte, as a pre-change build (diff the two outputs on a fixed seed).

### Phase 3 — migrate `dfs-anagrams.cpp`

- Call `dfs_set_diagnostic_stream(stderr)` once, before the first
  `dfs_diagnostic(...)` call (near `:328`).
- Drop the `stderr,` first argument from all eleven `dfs_diagnostic(stderr,
  ...)` call sites (`:328, 347, 352, 359, 375, 384, 390, 394, 403, 408, 411`).
- Drop the `stderr,` argument from `search.run(...)` at `:371`.
- Leave the two `fflush(stderr)` calls (`:363`, `:417`) alone (see Settled
  decisions).

Smoke check: `dfs-anagrams` invoked normally still prints every diagnostic
line it did before, in the same order, to stderr.

### Phase 4 — migrate the two tests that capture diagnostics

`test-dfs-search.cpp:389-395` (`projected_diagnostics`) and `:608-614`
(`exhausted_diagnostics`) currently pass their `tmpfile()` as `run()`'s second
argument. Change both to:

```cpp
FILE* const previous = dfs_set_diagnostic_stream(projected_diagnostics);
projected.run(&projected_output);
... read_stream(projected_diagnostics) as today ...
dfs_set_diagnostic_stream(previous);
```

No other test call site changes — every other `.run(sink)` call already omits
the progress argument, and the ambient sink still defaults to `NULL`.

Smoke check: both tests' existing assertions on captured diagnostic text pass
unchanged.

### Phase 5 (follow-up, not this plan) — actually log from `DfsTopN`

Once phases 1-4 land, adding a diagnostic line inside `DfsTopN` (e.g. the
fill-phase "published first floor" line discussed for the heap-contention
work) is a one-line `dfs_diagnostic(...)` call with no constructor or member
change required. Not included here — this plan only removes the structural
blocker.

## Non-goals

- No change to what gets logged or when (task-splitting gating via `verbose`,
  progress-interval gating, preflight content) — this is a routing change,
  not a behavior change.
- No new logging abstraction (no `Logger` class, no severity levels). The
  ask was to remove plumbing, not to add a bigger one.
- No thread-safety mechanism (mutex/atomic) around the global — see "Why this
  is safe here."

## Verification

- `diff` stderr output of `dfs-anagrams` before/after on a fixed small
  letter set, both with and without `-v`.
- Full test suite (`test-dfs-search`, `test-dfs-output`) passes, in
  particular the two diagnostic-capture tests.
- `grep -n progress_stream source/*.h source/*.cpp` returns nothing.
