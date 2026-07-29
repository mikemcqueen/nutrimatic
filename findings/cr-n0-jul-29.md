# Code review: repurposing `-n 0` as "return all results"

Reviewed 2026-07-29 against the uncommitted working tree (`git diff HEAD`) plus
the untracked `freq-find.sh`. No upstream was configured, so the review scope is
the working-tree diff rather than a branch range.

The change removes the `args.top == 0 ? NULL : &output` conditional in
`dfs-anagrams.cpp` and teaches `DfsTopN` to treat `result_limit == 0` as
unbounded retention, so `-n 0` now means "return everything" instead of "run the
search with no output sink".

## Verification performed

- Built (`ninja -C build`, already current) and ran the full suite with `$IDX`
  set: 7/7 pass.
- `-n 0` output is byte-identical to `-n 2000000` (718,670 rows, 10 letters),
  and identical between `-S 1` and `-S 4`.
- The `floor_log_score()` guard is a genuine latent-UB fix: previously, with
  `result_limit == 0` and an empty heap, the `heap.size() == result_limit`
  branch in `emit()` would have read `heap[0]` on an empty vector.

No correctness defects were found in the `DfsTopN` limit-0 logic itself. The
guards added at `dfs-output.cpp:147`, `:195`, and `:212` are consistent,
thread-safe, and produce output identical to a large finite `-n`.

## Findings

### 1. HIGH — `-n 0` is unbounded with no memory cap and no streaming

`source/dfs-anagrams.cpp:274`

With `result_limit == 0`, `supports_score_pruning()` returns false, so phase 2
runs with `SCORE_BOUND_OFF` *and* phase 3 has no floor to prune the per-solution
Cartesian expansion. Nothing bounds `retained`, `heap`, or the worker-local
`pending` priority queue, and results are only printed after
`take_sorted_results()`, so nothing is emitted incrementally either.

Measured on the real index, `dfs-anagrams $IDX somethingxyza -m 3 -n 0` reached
1.8 GB RSS at 10 s and 8.1 GB at 60 s, still climbing, and never terminated.
That is a 13-letter bag, well within normal use. The old `-n 0` returned in
seconds at roughly 200 MB.

At minimum this needs a documented warning. Better would be streaming output or
a hard cap on retained size.

### 2. MEDIUM — silently removes the count-only benchmark mode the docs depend on

`source/dfs-anagrams.cpp:274`

`-n 0` was the established "search core, no output collection" control, and is
referenced as such in:

- `findings/heap-contention.md:105` — "`-n 0`, which removes output collection
  and exposes scheduler scaling"
- `findings/dfs-codex-perf.md:67` — "Count-only (`-n 0`)", with a 3.30 s /
  223 MB baseline table
- `plans/archive/dfs-candidate-cache.md:242,378`

None of those are updated by this diff, and there is no replacement flag. The
`DfsAnagramSearch::run()` `sink == NULL` path (`dfs-search.cpp:1523,1536,2992,
3044,3129,3140,3174,3356`) is now unreachable from any CLI. Anyone re-running
the documented benchmark command gets an 8 GB+ hang instead of a 3 s run.

### 3. MEDIUM — usage text understates what `-n 0` does

`source/dfs-anagrams.cpp:49`

"`0 returns all results`" omits that it also disables phase-2 score-bound
pruning entirely (`dfs-search.cpp:1523` → `score_bounds_applicable` false →
`SCORE_BOUND_OFF`), which is what the new `test-dfs-cli.sh:129` assertion pins.

Side effects a user would not predict:

- `--dense-cache`, `--projection-depth`, and `--cache-size` become no-ops.
- `-C 0` without `--allow-cache-fallback` now *succeeds* under `-n 0` where it
  errors under `-n 10`, because the sizing check at `dfs-search.cpp:1415` is
  gated on `score_bounds_applicable`.

### 4. LOW — stale smoke-check documentation

`source/plans/heap-rewrite.md:188`

"a `--verbose` run with a small `-n` prints exactly one first-floor line, and
`-n 0` prints none" — the second clause now describes the wrong reason.
`publish_floor()` still never fires for limit 0, correctly, but the stated
rationale ("`publish_floor()` guards on `heap.size() != result_limit`") is only
accidentally true now, since `heap.size()` is always at least 1 by the time
`publish_floor()` is reached. Worth a comment at `dfs-output.cpp:83` making that
invariant explicit, since it is now the only thing preventing a `heap[0]` read
on an empty heap in the limit-0 case.

### 5. LOW — the min-heap is maintained but never used when `result_limit == 0`

`source/dfs-output.cpp:212`

Every insert does a `sift_up` and every dedup improvement a `sift_down`, all
inside the global `heap_mutex` critical section, yet with limit 0 the ordering
is never consulted: there is no floor and no eviction, and
`take_sorted_results()` re-sorts from scratch. At 700k+ retained spellings under
`-S 4` this is measurable wasted work in the contended section.
`heap.reserve(0)` / `retained.reserve(0)` (`dfs-output.cpp:78-79`) also means
roughly 20 rehashes of a 700k-entry map.

### 6. LOW — the new CLI assertion is weaker than its failure message claims

`source/test-dfs-cli.sh:90`

`cmp all.stdout unlimited.stdout` compares `-n 10` against `-n 0`, but the
synthetic index only ever produces 4 word sets, so `-n 10` was never saturated.
The test proves `-n 0` is not inert, but cannot distinguish "returns all" from
"returns top 10". Comparing `-n 0` against `-n 2` (which the script already runs
at line 157) and asserting the line counts differ would exercise the new
semantics.

### 7. LOW — `set -e` plus `pipefail` turns an empty freqsort result into a silent abort

`freq-find.sh:63`

If `freqsort` exits non-zero, for example on "no results" — the script
explicitly filters that string, so it anticipates the case — the pipeline into
`$candidate_file` kills the script with no diagnostic and a bare non-zero exit,
unlike every other failure path here, which prints a message. The script
otherwise runs correctly end-to-end against `$S3` / `$S3_IDX`.

## Suggested resolution

Finding 1 is the blocker. The options are to keep `-n 0` unbounded and document
the hazard prominently, add streaming output so memory stays flat, or pick a
different flag for "return all" and leave `-n 0` as the benchmark control.
