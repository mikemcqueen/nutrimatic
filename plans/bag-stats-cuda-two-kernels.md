# Plan: bag-stats-cuda counts rows in two kernels

## Outcome

`count_rows` becomes two kernels per batch:

- **`count_leftovers`** runs on every row. It walks the row's word bag
  combinations, checks each leftover against the pair-sum filter, and writes
  the row's `word_bags` and `solo`. It also records which rows have a
  leftover that passed the filter.
- **`count_pairs`** runs only on those rows. It finds the passing leftovers
  again and counts their pairs against a group hash built once at startup.
  Every warp in the block shares the scans.

The per-row hash, the per-row shared filter, the stamps and the 64-bit
rest-key packing all go. Output is byte-identical, and `bag-stats-gpu.h` and
`bag-stats-cuda.cpp` stay as they are. Of the progress lines, only the GPU
memory total changes.

On the standard profiling run, `count_rows` takes 8.3s today. The two
kernels should take about 1–2s together, which brings the whole run from
11.1s to about 4–5s.

## Why

The numbers below come from a phase-timed copy of today's kernel (thread 0's
`clock64()` at each row barrier) on the standard run: `--ss ppb,pb,wb,p`,
s1 with the `-u` seed, 96,381 groups and 9,304 word bags, 1,299,023 rows
averaging 32 letters.

| Phase | Block wall time | Per row |
|---|---|---|
| Hash build | 38.5% | fit-tests 96,380 groups; ~9,100 global `atomicCAS` inserts |
| List and word list | 17.1% | fit-tests 95,500 groups again, plus 9,304 word bags |
| Word walk | 43.8% | ~1,300 leftovers, each checked against the pair-sum filter |
| Row setup | 0.5% | |

Three facts drive the redesign:

1. **Only 26% of rows use the hash or the list.** Of 1.69G leftovers,
   5.84M (0.3%) pass the pair-sum filter, and only 339,639 rows have one.
   The other 74% build a hash and a list they never read.
2. **The per-row hash is equivalent to a hash of all groups.** The scan
   looks up `L − a` for a leftover L and a list group a. A group equal to
   `L − a` lies inside L, so it fits the row and is at most `|row| −
   min_group_length` letters long. That puts it in the row's hash whenever
   it is a group. Indexes match too: both use `groups_by_length` order.
3. **The walk waits on single-warp scans.** Rows with a passing leftover
   take 75% of the walk time, about 1.1M cycles each, against 131k for the
   rest. Each passing leftover is scanned by one warp over a ~9k-entry
   list while the block's other 31 warps sit at the end-of-row barrier.
   This is the 14.5% barrier stall in the `ncu` profile. One block per SM
   leaves nothing to fill the gap. Meanwhile every filter check uses a
   whole warp to read eight words.

## Keys

Everything uses the bag's packing (`BagKey`, 64 or 128 bits, chosen per
bag as today) with `p.bag_guards`. `fits(pool | bag_guards, key,
bag_guards, &rest)` is valid for any rest inside the bag, so fit tests work
on rows, leftovers and depth rests alike. This is the test `pair_sums`
already uses.

`RestKey`, `pack_rest`, `repack`, `Shared<RestKey>` and the narrow/wide
split in `count()` go. Bags with 128-bit keys pay 128-bit fit tests
throughout. The list scans those tests mostly served are now 0.3% of
today's.

## The group hash

`GpuRowCounter` builds it once, on the host, inside `upload_keys` from the
group keys it packs there, and uploads it. It stays read-only after that.

```cpp
template <typename Key>
struct GroupSlot {
    Key key;
    uint32_t index;
};
```

- It has `group_slots = 2 * group_count + 1` slots, the same as today's
  per-row `hash_slots`. `key == 0` marks an empty slot, since no group is
  empty.
- Group `i` is inserted at `home_slot(mix_key(key_i), group_slots)` with
  linear probing. The host computes `home_slot` with `unsigned __int128`;
  the device keeps `__umul64hi`.
- The device lookup, `find_group(hash, slots, key)`, walks from the home
  slot until the key matches (returns `index`) or the slot is empty
  (returns −1).

On the standard run that is 192,763 slots of 16 bytes, about 3 MB, which
stays resident in the 48 MB L2.

## The word walk

Both kernels enumerate a row's leftovers the same way, through one device
helper, `LeftoverWalk<BagKey>`, which each warp runs. The combinations are
today's: word bags `j1 ≤ j2 ≤ … ≤ jk` from the row's word list (`k =
p.words`), with every depth's rest required to fit. The leftover is `y`
minus their keys.

### The row's word list

Each block compacts the word bags that fit `y` into a shared list. Keys and
lengths are kept in separate arrays, with `WORD_CAPACITY<Key>` entries:
2048 for 64-bit keys and 1024 for 128-bit keys. A row with more fitting
word bags writes them to the block's slice of a per-stream global buffer
(`word_count` entries per block) instead, as today's `big` list does.
Compaction order doesn't matter. Both kernels compact the same way, but
each kernel only uses its own list.

### Chunks

`step()` gives the warp one chunk: up to 32 leftovers, one per lane, each
with a `valid` flag, its bag key and its length. It returns false once the
row has no work left for that warp.

- **`k == 1`:** lane 0 takes `base = atomicAdd(&next_word, 32)` and
  broadcasts it. Lane `l` takes word `base + l`, which is valid when it is
  below the list's count.
- **`k ≥ 2`:** lane 0 takes `first = atomicAdd(&next_word, 1)` and
  broadcasts it. Depths are numbered as in today's loop: `picks[1]` starts
  at `first`, and each deeper depth starts at the pick above it. Depths 1
  to `k − 2` advance warp-uniformly, as today's loop does. The last depth,
  `k − 1`, walks `[start, count)` in chunks of 32, where `start` is `first`
  when `k = 2` and `picks[k − 2]` otherwise. Lane `l` takes `j = start +
  32c + l`, which is valid when `j < count` and word `j` fits that depth's
  rest. The DFS state (`picks`, `rests`, `lengths`, and the chunk position)
  persists across calls in the warp's locals, so `step()` resumes where it
  stopped. When it finishes a `first`, it takes the next one.

Either way, each lane gets one leftover per chunk. Today a whole warp
handles each leftover.

### The filter check

`passes(key)` runs per lane. It computes `h = mix_key(key)`, reads the
32-byte block at `home_slot(h, filter_blocks)` as two `uint4` loads, and
tests the eight `filter_bit(h, k)` bits. It returns true when all eight
are set. The filter, its build and its sizing are unchanged.

## `count_leftovers`

- **Launch:** `LEFTOVER_THREADS = 256`, with the grid set to the SM count
  times `cudaOccupancyMaxActiveBlocksPerMultiprocessor`.
- **Row loop:** blocks take rows with `atomicAdd(p.next, 1)` as today, in
  `count()`'s longest-first order.

Per row:

1. Load the row's letters, build `y` and its length, and compact the word
   list. Zero the row's shared counts (`word_bags`, `solo`, `passed`).
2. Each warp loops on `step()`. For each valid lane:
   - add 1 to `word_bags`;
   - if the length is 0, add 1 to `solo`;
   - otherwise, if the length is at least `2 * min_group_length`, add 1 to
     the warp's `leftovers` filter count, and if `passes()`, add 1 to
     `scanned` and to `passed`.

   Each warp sums these over its lanes, and lane 0 adds the sums to the
   row's shared counts.
3. After `__syncthreads()`, thread 0 writes `out[row] = {word_bags, 0, 0,
   0, solo, 0}`. If `passed != 0`, it also appends the row to the stream's
   `flagged` array with `atomicAdd(&flagged_count, 1)`.

When the block runs out of rows, lane 0 of each warp adds its `leftovers`
and `scanned` counts to `filter_counts`, as today. `flagged` holds up to
`GPU_BATCH_ROWS` entries, the most rows a batch can have, so it can't
overflow.

## `count_pairs`

- **Launch:** `PAIR_THREADS = 512` with `__launch_bounds__(PAIR_THREADS,
  2)`, and a grid of `2 * multiProcessorCount` blocks.
- **Row loop:** blocks take entries of `flagged` with `atomicAdd(&next_flagged,
  1)` until they reach `flagged_count`, which they read on the device, so
  the host never reads it back between the two kernels.

Per flagged row:

1. Load the letters, build `y`, compact the word list, and zero the row's
   shared pair counts (`with_pairs`, `total_pairs`, `max_pairs`,
   `with_min_pairs`). Set `list_built = false` and `candidate_count = 0`.
2. **Collect.** Each round, every warp calls `step()` at most once, inside
   `while (__syncthreads_or(more))`. Every valid lane whose length is at
   least `2 * min_group_length` and that `passes()` appends its key and
   length to the shared candidate buffer through
   `atomicAdd(&candidate_count, 1)`. The buffer holds
   `CANDIDATE_CAPACITY = 1024` entries. After the round's barrier, if
   `candidate_count > CANDIDATE_CAPACITY − PAIR_THREADS`, the block
   flushes. A round adds at most `PAIR_THREADS` candidates, so the buffer
   never overflows. After the loop, the block flushes whatever is left.
3. **Write.** Thread 0 writes the four pair fields of `out[row]`. The row
   belongs to this block alone, and `count_leftovers` ran earlier on the
   same stream.

### Flush

1. **Build the row's list, once per row.** This is an order-preserving
   compaction of the groups `i < lenstart[length / 2 + 1]` that fit `y`.
   It runs in tiles of `PAIR_THREADS` groups. Each warp counts its fitting
   groups with `__ballot_sync` and `__popc`, the block scans the warp
   counts, and each fitting group's key and index go into the block's
   slice of the per-stream list buffers (`list_capacity` entries per
   block). Groups come in length order, so the list stays sorted by
   length. Then threads `l = 0 … length / 2 + 1` each binary-search the
   list by `group_lengths[index]` to get `pos[l]`, the first entry of at
   least `l` letters. Set `list_built`.
2. **Split the scans into chunks.** Candidate `c` needs
   `chunks_c = ceil(pos[len_c / 2 + 1] / 32)` chunks. The block takes an
   exclusive scan of those counts into `chunk_start[c]`, which also gives
   `total_chunks`.
3. **Scan.** Warp `w` takes chunks `t = w, w + 16, w + 32, …` below
   `total_chunks`. It finds chunk `t`'s candidate `c` by binary search over
   `chunk_start`, and lane `l` takes list entry
   `k = (t − chunk_start[c]) * 32 + l`, if it is below
   `pos[len_c / 2 + 1]`. For entry `(a, i)` that fits `L_c` with rest `r`:
   - if `r == a`, add `size_i * (size_i − 1) / 2`;
   - otherwise, with `b = find_group(group_hash, group_slots, r)`, add
     `size_i * size_b` when `b ≥ 0 && (2 * len_i < len_c || b > i)`.

   These are today's rules, with today's indexes. The warp sums over its
   lanes, and lane 0 adds the sum to `candidate_pairs[c]` with
   `atomicAdd`.
4. **Fold.** After `__syncthreads()`, the threads stride over the
   candidates. Each nonzero `candidate_pairs[c]` adds 1 to `with_pairs`,
   adds the pairs to `total_pairs`, updates `max_pairs` with `atomicMax`,
   and adds 1 to `with_min_pairs` when the pairs reach `p.min_pairs`.
   Then zero `candidate_count` and `candidate_pairs`, and sync.

### Shared memory

| Width | Word list | Candidates (key, length, pairs, chunk start) | Total |
|---|---|---|---|
| 64-bit | 2048 × 9 B = 18 KB | 1024 × 21 B = 21 KB | ~40 KB |
| 128-bit | 1024 × 17 B = 17 KB | 1024 × 29 B = 29 KB | ~47 KB |

A `static_assert` checks `2 * sizeof(PairShared<Key>) <=
SHARED_BYTES_MAX`, so two blocks fit per SM.

## `count()` per batch

1. Sort the rows longest first. The sort key is `GPU_MAX_LETTERS −
   length` alone, now that the wide/narrow split is gone.
2. Copy the rests up. Zero `next[0]` (rows), `next[1]` (flagged rows),
   `flagged_count` and `filter_counts`.
3. Launch `count_leftovers`, then `count_pairs`, on the stream.
4. Copy `out` and `filter_counts` back, then synchronize, as today.

## Device memory

**Per stream:**
- `rests` and `out` (unchanged);
- `next[2]`;
- `flagged`: `GPU_BATCH_ROWS` × 4 B;
- `flagged_count`;
- `filter_counts`;
- word-list overflow for both kernels: `(leftover_blocks + pair_blocks) ×
  word_count × (sizeof(Key) + 1)`;
- row lists: `pair_blocks × list_capacity × (sizeof(Key) + 4)`, about
  130 MB per stream on the standard run.

**Shared by both streams:** the group hash.

**Removed:** the per-row hash (`blocks × hash_slots × 16 B` per stream,
178 MB in all), today's per-block list and word buffers, and `next_stamp`.

The filter is still sized from the memory left after the streams' buffers.

## Code that goes

From `bag-stats-gpu.cu`:
- `count_rows`, `count_leftover`, `Shared`, `Slot`, `pack_rest`, `repack`,
  and `filter_has`;
- `FILTER_BITS`, `FILTER_WORDS`, `LIST_CAPACITY` and `THREADS`;
- the stamp logic in `count()`, `rest_bits()`'s use there, and
  `allow_shared<BagKey, RestKey>`. It is replaced by one `allow_shared<Key>`
  per kernel.

## What it should gain

Today's counts give these estimates:

- **Fit tests:** 250G (1.3M rows × 192k groups) become about 45G: 340k
  flagged rows × 96k for the lists, plus 1.3M rows × 9.3k word bags, done
  in both kernels on flagged rows.
- **`count_leftovers`:** limited by 1.69G random 32-byte filter reads, about
  54 GB of DRAM traffic. That should take roughly 0.3–0.5s.
- **`count_pairs`:** 5.84M candidate scans of up to ~9k entries each,
  spread over all warps, with lookups that hit L2. That should take roughly
  0.3–0.8s, plus the 340k list builds. It also repeats the word walk and
  its filter checks on the flagged rows. Those tend to be the long rows,
  which hold most of the leftovers, so at worst it rereads all 1.69G
  filter blocks: another 0.3–0.5s, or 0.6–1.3s for the kernel.

The rest of the run doesn't change: about 2.1s of startup, of which 1.5s is
the filter build, and about 1s of host prep between batches under
`--threads 1`. Neither is part of this plan.

## Other plans

This plan replaces items 2 (two 512-thread blocks) and 4 (check the filter
only on long lists) of `plans/bag-stats-cuda-other-optimizations.md`.
`count_leftovers` reads the filter one lane per leftover, and the list scans
it would have skipped no longer exist for unflagged rows. Item 1 (RestTable
lookups in pass 1) still applies. Item 3 (per-unit prefilter) would now
apply to `count_leftovers`' word scan and `count_pairs`' list build.

## Verification

- `meson test bag-stats-cuda` passes. It compares against CPU `bag-stats`
  and includes a wide-key (128-bit) case.
- The standard profiling command gives output byte-identical to the
  pre-change binary, saved as `tmp/s1/bag-stats-cuda.before`. The filter
  lines and the `scanned N of M leftovers` line match too: the filter
  already gets its full 12.0 bits per pair, so the freed memory doesn't
  resize it. Only the GPU memory total changes.

  ```
  bag-stats-cuda --threads 1 -s1 -n 1000 --wbfrac-min --mp --wbwp-min \
    --ss ppb,pb,wb,p --pp ../nutrimatic/results/s1/list \
    -u "$U_PROFILE" $S1 tmp/s1/seed.pairs $DICT --progress 30
  ```
- On the same inputs, `--ss pb,wb,wb,p` is byte-identical to the pre-change
  binary. It exercises the `k ≥ 2` path of `step()`.
- `nsys`: `count_leftovers` plus `count_pairs` per batch compared with
  today's 8.3s of `count_rows`, timed with no other `query-index`,
  `dfs-anagrams` or `bag-stats-cuda` process running.
