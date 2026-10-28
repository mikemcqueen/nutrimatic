# bag-stats-cuda: Candidates Review

From the `/code-review medium` pass on the working tree that implements
`plans/bag-stats-cuda-candidates.md`. No correctness bug was found; the
candidate path's counts match the removed `count_pairs`. Line numbers refer
to that tree.

* The candidate buffers shrink the pair-sum filter
  (`bag-stats-gpu.cu:1055`). `set_up` allocates the per-stream candidate
  buffers, about 90·C bytes per stream with 128-bit keys, before
  `build_filter` sizes the filter from free memory. `-C 24` takes about
  4.3 GB across two streams; on the 5.97G-pair bag (filter needs 8.5 GB,
  about 1.4 GB spare) the filter is clamped to `room`, more false-positive
  leftovers become candidates, and more batches split — what raising `-C`
  was meant to avoid. Nothing warns.

* An overflowing pass still does the full candidate work
  (`bag-stats-gpu.cu:987`). When the first pass has `entry_count > C`,
  `run()` still launches `insert_candidates`, a full `pair_sums`
  enumeration and `fold_candidates`, then copies `host_out` back, and
  `clear_pairs` discards it. Every split batch, and any rewalk share that
  overflows before doubling, pays one extra group-pair enumeration and a
  3 MB D2H copy. `pair_sums` already reads `entry_count`; it can return when
  the count is over capacity, and insert/fold can do nothing in that case.

* The whole candidate hash is cleared every pass (`bag-stats-gpu.cu:931`).
  `count_candidates` memsets all 2C+1 hash slots and the candidate filter on
  every batch and every split share, however few entries were appended —
  about 1.5 GB per pass at `-C 24` with 128-bit keys, P times for a split
  into P shares. `fold_candidates` could reset the state and pairs of the
  slots it reads through `entry_slots`, leaving the hash clean for the next
  pass.

* The plan doesn't match the code (`plans/bag-stats-cuda-candidates.md:153`).
  It describes a separate `count_candidate_pairs` kernel and a `pass_max`
  counter; the code runs that kernel as `pair_sums` in
  `PairSumMode::candidates` and keeps the pass maximum in `entries[1]`. The
  nsys step names `count_candidate_pairs` too.

* A line in `fold_candidates` is 81 columns (`bag-stats-gpu.cu:578`). The
  `atomicAdd(reinterpret_cast<unsigned long long*>(&out.with_min_pairs),`
  line should wrap its argument like the `total_pairs` atomicAdd above it.

## Timings

The 5.97G-pair bag run (`seed-pairs -s1 | bag-stats-cuda --threads 1 -s1 -n
5000000 --wbfrac-min --mp --wbwp-min --ss ppb,pb,wb,p --pp
../nutrimatic/results/s1/list -u ... --progress 10`), before the candidate
change (`tmp/s1/bs.ten.c-h-aut.3`, recorded in `tmp/junk`) and with it
(`tmp/s1/bs.ten.c-h-aut.5`). The outputs are byte-identical. No other
`query-index`, `dfs-anagrams` or `bag-stats-cuda` process was running during
the second run.

| | Before | After |
|---|---|---|
| GPU memory | 9045.6 MB | 8730.5 MB |
| Pair-sum filter | 8540.0 MB, built in 4.6s | 8540.0 MB, built in 4.6s |
| Pass 1/2 | 15s | 24s |
| Pass 2/2 | 15s | 22s |
| Elapsed | 38.48s | 53.17s |
| User CPU | 35.40s | 50.41s |
| Leftovers scanned | 2688328 of 37081447087 | 2688328 of 37081447087 |
| Candidates | | largest batch 68817 (-C 1 holds 1000000), 0 of 186 batches split |

The change is 38% slower on this bag, with no batch split.
A HEAD build (4 files from `git show HEAD:`, the build's own compile
commands) reran it: 15s and 15s passes, 38.23s elapsed, output identical, so
the before column is HEAD.

The standard profile run (`tmp/s1/seed.pairs`, `-n 1000`, `--progress 30`)
goes the other way. Outputs identical, nothing else running:

| | HEAD | Candidates |
|---|---|---|
| GPU memory | 3246.5 MB | 3085.8 MB |
| Pass 1/2 | 5s | 2s |
| Elapsed | 7.47s | 4.30s |
| Candidates | | largest batch 551661 (-C 1 holds 1000000), 0 of 20 batches split |

No kernel times were recorded for the candidate code. The only `nsys`
report in `tmp/s1` (`bsc.nsys-rep`, 00:28) predates the two-kernel change:
`count_rows` 16.42s over 20 launches, `pair_sums` (build) 2.24s, `pair_sums`
(count) 6.0 ms.
