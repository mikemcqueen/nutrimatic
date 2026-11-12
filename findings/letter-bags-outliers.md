# letter-bags --outliers: staged candidate index

`--outliers --by N` used to list every sub-bag (up to N letters removed) of
every bag as a 16-byte (hash, bag) entry and sort the lot: about 11, 66 and
280 entries per bag for N = 1, 2, 3 on pair-length bags.

## What it does now

A bag with no other bag within N has none within N - 1 either, so the search
runs in stages k = 1..N and only the bags still without a neighbour after
stage k - 1 (the open bags) are indexed at stage k:

1. Put the sub-bags of the open bags (up to k letters removed) in an
   open-addressing table of 8-byte slots: 32 hash bits and the bag id.
2. Stream every bag: generate its sub-bags, prefetch their slots, probe. A hit
   on an open bag is confirmed with the real distance, then both are marked.
3. Drop the marked bags from the open list.

Two bags within k always share a sub-bag, so a stage misses nothing, and
every mark is confirmed by `bag_distance`, so the result is exact.

The table holds at most 2^22 entries (64 MB). When the open bags have more,
they are indexed in several chunks, each followed by a stream of all bags;
bags marked by an earlier chunk are skipped. Memory beyond the bag map is
therefore fixed, whatever the input size or N. The stream is read-only on the
table and runs on all hardware threads.

A chunk's table is sized before it is filled. The size comes from each bag's
letter runs, not from generating its sub-bags: the number of sub-bags with up
to k letters removed is the number of ways to remove r_i ≤ c_i from each run
of c_i equal letters with Σ r_i ≤ k, a small polynomial product truncated at
degree k. Only the fill pass generates hashes.

Only `--all` keeps every input line, so only that mode's bag type carries the
lines; the other modes' bags are 24 bytes smaller.

## Why it works on this data

The open list collapses quickly:

| | bags | open after 1 | after 2 | after 3 | after 4 |
|---|---|---|---|---|---|
| seed.pairs | 654582 | 209234 | 20422 | 1175 | 59 |
| all.s1.m3.dict.remain | 819818 | 103136 | 7727 | 528 | 39 |

## Measurements

20 threads, a bag-stats-cuda run active on the GPU at the time.

| | seed.pairs before | after | dict.remain before | after |
|---|---|---|---|---|
| count mode | 109 MB, 0.7 s | | 125 MB, 1.0 s | |
| --by 1 | 250 MB, 1.3 s | 194 MB, 1.1 s | 270 MB, 2.3 s | 214 MB, 1.7 s |
| --by 2 | 1.2 GB, 5.0 s | 194 MB, 1.8 s | 1.2 GB, 5.9 s | 214 MB, 1.9 s |
| --by 3 | 4.3 GB, 19.1 s | 194 MB, 2.8 s | 2.2 GB, 17.5 s | 213 MB, 2.3 s |
| --by 4 | not run | 194 MB, 3.8 s | not run | 213 MB, 2.9 s |

Output is byte-identical to the previous version for --by 1, 2 and 3 on both
files, with and without --all.

Sizing from run counts and the smaller bag, best of 5, old → new:

| | seed.pairs | dict.remain |
|---|---|---|
| count mode | 109 → 99 MB, 0.52 → 0.51 s | 125 → 112 MB, 0.81 → 0.80 s |
| --by 1 | 194 → 183 MB, 1.01 → 0.96 s | 213 → 200 MB, 1.43 → 1.37 s |
| --by 2 | 194 → 184 MB, 1.68 → 1.59 s | 213 → 201 MB, 1.77 → 1.68 s |
| --by 3 | 194 → 184 MB, 2.70 → 2.61 s | 213 → 201 MB, 2.15 → 2.07 s |
| --by 4 | 194 → 184 MB, 3.49 → 3.45 s | 213 → 201 MB, 2.84 → 2.73 s |
| --by 3 --all | 233 → 233 MB, 2.97 → 2.97 s | 267 → 267 MB, 2.56 → 2.44 s |

Table cap trade-off on seed.pairs --by 4: 2^24 entries 521 MB, 2.8 s; 2^22
194 MB, 3.8 s; 2^20 145 MB, 7.3 s.
