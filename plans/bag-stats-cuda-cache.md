# Plan: bag-stats-cuda serves pass 2 from a RestTable

## Outcome

When ranking needs two passes, `bag-stats-cuda` keeps the `RestTable`
`bag-stats` uses. Pass 1 counts every row on the GPU, as today, and puts
each rest's counts into the table. Pass 2 looks each row's rest up on the
CPU and sends only the misses to the GPU. With the table holding every
distinct rest, pass 2 takes about as long as `bag-stats`' pass 2, seconds,
in place of about as long as pass 1. Output is unchanged.

```
bag-stats-cuda ... [--cache-size N] ... LETTERS PAIRS DICT
```

`--cache-size N` means what it means in `bag-stats`: hold at most N MB of
cached remainder counts (default 128, about 1.9M remainders); 0 turns
the cache off. `--pair-cache-size` stays `bag-stats` only.

This builds finding #4 of `findings/archive/bag-stats-cuda-review.md` as
decided there: no lookups in pass 1, no deduplication within a batch.

## Shared table

`RemainderIndex`, `RestTable`, `REST_CACHE_ENTRY_BYTES`,
`rest_cache_entries` and `CacheCounts` move out of `bag-stats-cpu.cpp`'s
anonymous namespace into new `source/bag-stats-cache.{h,cpp}`, built into
`bag_stats_run_lib`. `RestTable` itself doesn't change, except that it
takes the program name for its `cache full` notice:

```cpp
RestTable(char const* program, PairsCounts const& bag, bool enabled,
          size_t megabytes, bool report);
```

`CacheHooks::SharedCacheCounts` moves with them as `SharedCacheCounts`,
still with `add` and `text(name)`. `CacheHooks` stays in
`bag-stats-cpu.cpp` and holds two of them as now.

The startup line moves too:

```cpp
// Prints "PROGRAM: cache holds N entries (M MB) of R remainders" when
// `table` is enabled.
void report_rest_table(char const* program, RestTable const& table);
```

`with_walk` in `bag-stats-cpu.cpp` calls it in place of its own
`fprintf`.

## Options

`--cache-size` becomes a common option:

- `BAG_STATS_DEFAULT_CACHE_SIZE_MB` moves from `bag-stats-cpu.h` to
  `bag-stats-run.h`.
- `BagStatsArgs` gains `int cache_size_mb =
  BAG_STATS_DEFAULT_CACHE_SIZE_MB`; `BagStatsOptions::cache_size_mb` goes
  away, and `with_walk` reads `common.cache_size_mb`.
- `OPT_COMMON_CACHE_SIZE` joins `BAG_STATS_COMMON_LONG_OPTIONS`, parsed in
  `parse_common_option` with `parse_count`. `bag-stats.cpp` drops
  `OPT_CACHE_SIZE` and its case, and its remaining options renumber from
  `OPT_COMMON_END`.
- `--cache-size N` goes in both usage synopses next to `--threads N`, and
  its `dfs_help_option` line moves from `bag-stats.cpp` into
  `help_common_options`.

## The walker knows its pass

`Walk::walker()` becomes `Walk::walker(int pass)` in both tools. The shared
passes in `bag-stats-run.h` pass theirs:

- `print_rows`, `count_sort_values`, `find_row`: 1.
- `top_rows`, `score_rank`: `walk.passes`.

`bag-stats`' `walker(int)` ignores it. `list_bag_stats` calls
`walker(1)`.

## GpuRowWalker

`GpuRowWalker` takes the `RestTable const&` and a `bool fill`, true when
`pass < walk.passes`. `Walk::walker(pass)` passes both.

**Pass 1 (`fill`).** `walk` buffers rows as now. `flush` counts them on the
GPU, then puts each row's rest and counts into the table before visiting
the rows. It does no lookups.

**Pass 2.** With the table enabled, `walk` looks each row's rest up
first. Every row is buffered, with its unit, picks and pair length as now,
plus its counts. Only misses append their rest to `rests_`, and a miss's
index in the buffer goes in `misses_`. With the table off, `walk` does no
lookups and every row is a miss. The buffer still flushes at
`GPU_BATCH_ROWS` rows and at the end of a walk. `flush` counts the misses
on the GPU, if there are any, writes their counts into their rows, then
visits every row in walk order. Pass 2 doesn't put: a rest that missed
was left out of a full table.

Rests go to the table as `PairsCounts` copied from their
`GPU_LETTERS_BYTES` (`static_assert`ed equal), and counts as
`BagStatsCounts` converted from `GpuCounts` (already `static_assert`ed the
same size).

## Wiring in bag-stats-cuda.cpp

`main` builds the table after the rankers:

```cpp
RestTable const rest_table(PROGRAM, inputs.bag, rankers.passes && !one_row,
                           size_t(args.cache_size_mb), args.progress != 0);
```

and calls `report_rest_table` after the device line. `Walk` holds it.

`NoHooks` becomes `GpuCacheHooks`: `finished(walker)` adds the walker's
`take_rest_counts()` to a `SharedCacheCounts`, and `text()` gives
`, cache hits N/M (P%)` once there have been lookups. When the table is
off in a two-pass run, `text()` gives `, cache off` on both passes' lines,
as `bag-stats` does. Otherwise `text()` is empty, so pass 1 lines with the
table on and single-pass runs print as they do now.

## Test

`test-bag-stats-cuda.sh`'s two-pass `run_both` (`--ss pb,pb,wb,wb,p -n 5
--wb --mp`, two sort keys) already covers pass 2 served from the table at
the default `--cache-size`. A second `run_both` of the same search with
`--cache-size 0` covers it with the table off.

## Review doc

Finding #4 in `findings/archive/bag-stats-cuda-review.md` goes from
`decided … not yet built` to `fixed`.
