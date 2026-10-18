# Plan: `bag-stats-cuda`

## Outcome

A separate tool, `bag-stats-cuda`, that prints exactly what `bag-stats`
prints for the same arguments, with every row's counts computed on the GPU:

```
bag-stats-cuda [-u LETTERS] [-s N] [--min-wb-len N] [--ss STEPS] [--pp FILE]
               [--threads N] [--progress N]
               [-n N SORT... [--by-length | --by-pick]] [--score ...] ...
               LETTERS PAIRS DICT
```

The options are `bag-stats`' options without `--cache-size` and
`--pair-cache-size`. The tool has no remainder cache, no pair cache and no
pair count table: the GPU recomputes every count, including on the second
ranking pass. The output, including the `bag-stats 4` header, is
byte-identical to `bag-stats`, so `bag-stats-sort` and the rest of the
workflow read it unchanged.

`bag-stats` keeps its CPU path and its caches. The two tools share the
command line, the input loading, the row walk, the ranking passes and the
output code.

## Problem

Every row ends in the same loop. For each combination of word bags W that
fits the row's rest Y, take the leftover X = Y − W. Then, for every inner
dictionary group a that fits X and is at most half its length, check whether
X − a is also a group. `findings/bag-stats-codex.md` puts 97.7% of a
pb,pb run in that loop.

A scratch CUDA prototype ran on an RTX 4080 Laptop GPU (58 SMs, 12 GB, 48 MB
L2) with S1TEST − a used pair (46 letters, 195,988 inner groups, 19,639
word bags). It counted 2,000 random pb,pb rows from
`seed.s1.m4.all.85.15.pairs` (rests averaging 23 letters, 1,478 leftovers
per row):

- One global table of every leftover's pair count doesn't fit. 7.2 billion
  group pairs fit the bag, and an estimated 2–3 billion distinct leftovers
  have a nonzero count. Counts have to be computed per leftover.
- Only about 1 in 19,000 partner lookups finds a group: about 60 per row.
  A bit filter of the groups that fit Y, held in shared memory, answers
  nearly all of them without a hash probe.
- One warp per leftover, with the row's half-length groups in shared memory,
  counted 6,000 rows/s at 8.8e10 fit tests/s. Counts matched a CPU version
  of the same algorithm on every row compared.
- The work is uneven: 17% of rows hold 94% of the fit tests, with about
  2,000 half-length groups fitting their rest.

## Device algorithm

### What lives on the device

Uploaded once at startup:

- The inner groups, shortest first, as `PairsExactCounter::groups_by_length()`
  orders them: each group's key under the bag's `PairsPacking`, its
  letter counts (26 bytes), its length and its size (`group_size()`).
  `lenstart[L]`: the first group with at least L letters.
- The word bags of DICT, as `load_word_bags` orders them: key, letter
  counts and length.

The bag's key type is `uint64_t` when `PairsPacking(bag).bits() <= 64` and
`unsigned __int128` otherwise.

### Rest-local packing

Each row repacks its rest Y with a `PairsPacking` of Y itself. On the s1
sample that takes a median of 36 bits and at most 46, so the leftover scan
runs on 64-bit keys even when the bag needs 128. The host computes each
rest's width as it walks rows. Rows whose rest needs more than 64 bits go
to a 128-bit instantiation of the same kernel. The kernel is templated on
the bag's key type and the rest's key type.

### One block per row

A batch is a list of rests (26-byte letter counts each) and, per row, a
`BagStatsCounts` to fill. The kernel runs a persistent grid, one block per
SM, 1024 threads. Blocks take rows from an atomic counter, and the host
sorts each batch's rows longest rest first, so the heaviest rows start
early.

For a row with rest Y, the block:

1. Builds Y's packing and Y's key under the bag's packing.
2. Scans the groups with at most |Y| − 3 letters and, for each that
   fits Y:
   - sets two bits in a 32 KB shared bit filter, indexed by a mix of its
     rest-local key (`mix-bits.h`'s splitmix64 finalizer);
   - inserts its rest-local key and global index into the block's hash,
     an open-addressed table in global memory sized to twice the bag's
     group count. Each slot also holds a 32-bit stamp, the row's number
     in the stream's running count of rows; a slot with another row's
     stamp is empty, so the table is never cleared between rows. It is
     cleared once when the count wraps. A probe hit reads the group's
     size through its global index;
   - when 2·length ≤ |Y|, appends it to the half-length list, bucketed by
     length so `pos[L]` marks where length L starts. The list is held in
     shared memory as two arrays, rest-local keys and global indexes, up
     to 4,096 entries with 64-bit rest keys and 2,048 with 128-bit, and in
     the block's global scratch past that. A `static_assert` checks that
     the filter, the list, `pos[]` and the counts fit the 101,376 bytes of
     shared memory a block can take on sm_89.
3. Scans the word bags and writes the ones that fit Y, repacked
   rest-local, to the block's word list in global scratch.
4. Has each warp take a first word bag from the word list. For the
   `--ss` word steps after the first, the warp walks the later word bags
   in non-decreasing order, as `RestCounter::visit` does, uniformly across
   its lanes. Each complete combination is one leftover X, and the warp
   updates the row's counts:
   - `word_bags` +1, and `min_word_bags` +1 when every bag has at least
     `--min-wb-len` letters;
   - X empty: `solo` +1 and no pair count;
   - |X| < 2·`INNER_MIN_WORD_LENGTH`: no pair count;
   - otherwise the lanes split the half-length list up to `pos[|X|/2 + 1]`.
     For each entry a that fits X, with remainder r = X − a:
     - r == a: add size·(size − 1)/2;
     - otherwise, when both of r's filter bits are set, probe the block's
       hash for r. If it holds group b, add size(a)·size(b) when
       2·|a| < |X| or b's global index is past a's.

     A warp shuffle sums the lanes into the leftover's pair count PC.
     Then `with_pairs`, `total_pairs`, `max_pairs` and `min_with_pairs`
     follow the same rules as `RestCounter::visit`.
5. Sums each warp's counts into the row's `BagStatsCounts` with
   shared-memory atomics (64-bit add and max), and writes it out.

These are `PairsExactCounter`'s same-group and count-once rules, so a
row's counts equal `bag-stats`' counts.

### Streams and memory

`GpuRowCounter` owns two streams. Each has its own device buffers:

- the batch's rests and counts;
- per block, the hash (2 × groups × 16 bytes, about 6 MB at 196k groups;
  32-byte slots, about 12.5 MB, when the bag needs more than 64 bits),
  the half-length list overflow and the word list.

That comes to about 0.4 GB per stream on the s1 bag. `count(rests, out)`
takes a free stream, uploads, launches, waits and copies the counts back.
A caller that finds both streams busy waits. Kernel run time is not
limited, so a batch's size is set only by host memory and by keeping both
streams busy: `GPU_BATCH_ROWS` = 65,536 rows.

## Host side

### Row census and blocks

The walk enumerates rows without counting them in two places:

- once at startup, to count each unit's rows (a census, pair subtractions
  only);
- once per pass, to collect each block's rests.

From the census, units are grouped into consecutive blocks of about
`GPU_BATCH_ROWS` rows each. `bag-stats` uses blocks of one unit, as today.

### Shared pass driver

The pass functions (`print_rows`, `count_sort_values`, `top_rows`,
`find_row`, `score_rank`, `print_score`, `print_top`) move from
`bag-stats.cpp` into `source/bag-stats-run.h` as templates over a `Walk`
type. They take blocks of units from the shared counter where they take
units today. `print_rows`' window counts blocks instead of units.

`bag-stats-run.h` declares `WalkBase`, the fields every pass shares:
`steps`, `prospect_bags`, `bag`, `word_bags`, `args` (`BagStatsArgs`),
`threads`, `passes`, `units()` and the census blocks. Each tool's `Walk`
derives from it and adds:

- `walker()`: a `RowWalker` over bag-stats' tables, or a `GpuRowWalker`
  over bag-stats-cuda's `GpuRowCounter`;
- `progress_hooks()`: an object with `finished(walker)`, which collects
  that walker's stats since its last call, and `text()`, the progress
  line's trailing text.

Both walkers implement:

```cpp
// Calls visit(unit, row, length) for each row of units [first, last), in
// unit order and walk order within a unit; pair(), pairs(), picks() and
// at() describe that row during the call.
template <typename Visit>
void walk(size_t first, size_t last, PairsCounts const& bag,
          Visit const& visit);
```

`seq` stays a row's place within its unit, so ties break exactly as
today.

### PickWalk

`PickWalk`, in `bag-stats-run.h`, holds the steps, `chosen_`, `pair()`,
`pairs()`, `picks()`, `at()` and `repeats_bag()`, and runs
`RowWalker::descend`'s loop over picks. It calls two hooks:

- `enter(step, rest)` before each step's picks;
- `leaf(rest, letters)` at each row.

The two walkers use it as follows:

- `RowWalker` (bag-stats): `enter` is today's `filter`, and `leaf` counts
  through `RestTable` and `RestCounter` and calls `visit`, as today.
- `GpuRowWalker` (bag-stats-cuda): `enter` does nothing, and `leaf`
  appends the row's `chosen_`, unit, rest and letters to a buffer. When
  the buffer holds `GPU_BATCH_ROWS` rows, mid-unit if need be, and at the
  block's end, the walker flushes it: it calls `GpuRowCounter::count` on
  the buffer's rests, then replays the rows in order, restoring each
  row's `chosen_` and calling `visit` with the counts, and empties the
  buffer. Rows reach `visit` in walk order, so `seq` is unchanged, and
  `count` never takes more than `GPU_BATCH_ROWS` rests.

### Command line and loading

`source/bag-stats-run.{h,cpp}` also gets the code both mains share:

- `BagStatsArgs`, `bag-stats`' `Args` without the cache sizes;
- `BAG_STATS_COMMON_LONG_OPTIONS`, `parse_common_option()` (returning
  `RankOption`, the `parse_rank_option` pattern), `check_common_args()`
  and `help_common_options()`;
- `remaining_letters`, `load_word_bags`, `load_inner_dictionary`;
- `Progress`, built from the seconds, pass, passes, units and the walk's
  progress hooks. `finished(walker, units)` hands the walker to the hooks
  and adds the block's units to the count it reports. `bag-stats`' hooks
  own the cache and pair cache counts and give today's cache text;
  `bag-stats-cuda`'s collect nothing and give no text.

`bag-stats.cpp` keeps the cache options, `PairCountTable`,
`PairCountCache`, `RestTable`, `InnerCounters`, `RestCounter`, `RowWalker`
and its `main`. Its output and speed don't change.

### bag-stats-cuda

- `source/bag-stats-cuda.cpp` (gcc): `main`, `GpuRowWalker`, and the
  upload of groups and word bags.
- `source/bag-stats-gpu.cu` (nvcc): the kernel and `GpuRowCounter`.
- `source/bag-stats-gpu.h`: the plain-C++ interface between them:
  - `GpuInputs`, flat arrays of the groups and word bags;
  - `GpuCounts`, seven `uint64_t` in `BagStatsCounts` order;
  - `GpuRowCounter`.

  It includes no other project header, so nvcc compiles only the `.cu`
  file and standard headers.
- With `--progress`, a startup line names the device, the bag's key bits
  and the GPU memory taken.
- No CUDA device, or a CUDA call failing: `bag-stats-cuda: ` and the CUDA
  error string on stderr, exit 1.

## Build

- `source/meson.build`: `bag-stats` is built from `bag-stats.cpp`,
  `bag-stats-run.cpp` and `bag-stats-rank.cpp`.
- `source/meson.build`: `add_languages('cuda', required: false)`. When it
  is found, build `bag-stats-cuda` from `bag-stats-cuda.cpp`,
  `bag-stats-run.cpp`, `bag-stats-rank.cpp` and `bag-stats-gpu.cu`. It
  links what `bag-stats` links plus `dependency('cuda')`, with
  `cuda_args: ['-arch=native', '-std=c++20', '-O3']`, and its test is
  registered in the same branch. Builds without nvcc skip both.
- nvcc 13.0 (`/usr/local/cuda-13.0`) with gcc 14 as its host compiler built
  the prototype.

## Measurement

Same machine, nothing else running. Before each timed run, check
`pgrep -x` for query-index, dfs-anagrams and bag-stats in the host process
table, and `nvidia-smi` for other GPU processes.

- Run `bag-stats` and `bag-stats-cuda` with the same arguments:

  ```
  --threads 0 -s 1 -n 100 --min-wb-frac --mp --min-wbwp -u "catdog"
  --ss ppb,pb,wb,p --pp ~/code/nutrimatic/results/s1/list
  $S1TEST idx/seed.s1.m4.all.85.15.pairs $DICT
  ```

  `bag-stats` takes about 3.5 minutes. Report wall time per pass and
  rows/s. The outputs must be byte-identical.
- One run without `-u`, 55 letters, which needs 128-bit bag keys. Do not
  run this; it is kept here for reference.

## Tests

One smoke test, `source/test-bag-stats-cuda.sh`, on `test-bag-stats.sh`'s
fixture: `bag-stats` and `bag-stats-cuda` print identical output for the
default `--ss`, and for `--ss pb,pb,wb,wb,p` with `-n` and two sort keys.
It exits 77 (skipped) when `nvidia-smi -L` lists no GPU.

## Alternate approaches

- Size each row's hash to the row. Step 2 first counts the groups that fit
  Y, then uses a table of the next power of two at least twice that count
  and clears only that span. Light rows touch a few KB rather than a
  6 MB table, and the compact table caches better when heavy rows probe it
  past filter false positives. It costs a second scan of the groups per
  row. It combines with stamps, which drop the clear.
