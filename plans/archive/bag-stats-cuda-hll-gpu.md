# Plan: bag-stats-cuda --leftovers --hll walks the picks on the GPU

## Status

Implemented. `bag-stats --leftovers --hll` keeps its CPU walk.

Where the code differs from the plan below:

- `run_leftovers_gpu` (`bag-stats-gpu.cu`) uploads, launches, polls and
  copies back. Loading, numbering and printing are `run_leftovers` in
  `bag-stats-cuda.cpp`, since nvcc compiles no project header.
- Warps count finished units in a device counter, and lane 0 stores the
  new count to the mapped `units_done` with a plain store, so the GPU
  never does an atomic on host memory. The host keeps the largest value it
  has read.
- The kernels hold at most `GPU_MAX_PAIR_STEPS` (16) steps; more fails
  with "--leftovers takes at most 16 pb steps".

## Outcome

```
bag-stats-cuda --leftovers --hll --ss pb[,pb...] [--kernel N] [-u LETTERS] ... PAIRS - DICT
```

The whole pick walk runs on the device. The host uploads the pair bags once,
launches one kernel, polls a progress counter and reads back the counts and
HyperLogLog registers at the end. No CPU thread walks, so nothing waits on
the GPU.

`--kernel N` picks the walk kernel: 1, the simple one (default), or 2, the
experimental one. Both give the same output.

Output is byte-identical to `bag-stats --leftovers --hll` for the same
arguments: the header, the `cache-size N` line (below), `PICKS,
COMBINATIONS` and `LEFT,COMBINATIONS,ROWS,LEFTOVERS_HLL`. The registers come
out the same because the kernel hashes the same keys with the same function.

In `bag-stats-cuda`, `--leftovers` requires `--hll`; the exact distinct
count (`KeySet`) stays in `bag-stats`.

## Why

From the `$S1` `pb,pb` run (`--threads 0`, 4m41s, 92.8 CPU-minutes):

- 592,990 pair bags; 91.9G of the ~175.8G possible pairs fit (52%).
- About 45.8G distinct leftovers after two picks.
- The CPU does about 32M fit checks per core per second.

`pb,pb,pb` on `$S1` scans the bags after each fitting pair, about N/3 ≈
200K each: around 1.8×10¹⁶ fit checks. That's about 300 days on 20 cores.
A packed-key fit check is a subtract and a mask test, so at 10¹¹–10¹²
checks per second the 4080 takes 5–50 hours. On the trimmed `$S1` bags
planned for `pb,pb,pb` testing, the GPU walk sizes a run in minutes where
the CPU takes hours. Its `cache-size` line sizes the `--cache-size` of the
full run.

## Moved to bag-stats-run

`bag-stats-cuda` doesn't link `bag-stats-cpu`, so the pieces both tools
need move to `bag-stats-run.{h,cpp}`:

- `LeftoverSketch`, plus a constructor from 32-bit registers (the GPU's
  layout, below) that narrows each to a byte.
- `LeftoverPart`, renamed `LeftoverCounts`.
- The printing at the end of `count_leftovers`, as
  `print_leftovers(args, letters, pair_bags, counts, hll)`.

`count_leftovers` keeps its walk and calls `print_leftovers`.

## The cache size

`print_leftovers` prints, after `pair bags`, the `--cache-size` that holds
every distinct leftover:

```
cache-size N
```

N is the distinct leftovers × `REST_CACHE_ENTRY_BYTES`, in MB rounded up,
with `--hll` from the estimate × 1.03 (3σ of HLL's 0.8%). It applies to a
`--ss` with the same pb steps; `--leftovers` takes no ppb steps.

`RestTable` holds at most `UINT32_MAX` entries (`rows_index_` is 32-bit),
which is `UINT32_MAX × REST_CACHE_ENTRY_BYTES` in MB rounded up, the cap.
When N is over the cap, the line prints the cap and the size needed:

```
cache-size CAP (capped; N needed)
```

Both
`bag-stats --leftovers` (with or without `--hll`) and `bag-stats-cuda
--leftovers --hll` print it.

## Keys

The CPU's HLL key is `RemainderIndex::number(rest) + 1`. `number` is linear
in the letter counts, so:

```
number(rest) = number(bag) − Σ number(pick)
```

The host computes `number` once for the bag and each pair bag and uploads
them as `uint64_t`. The kernel subtracts as it picks and hashes with
`mix64`, which is the same splitmix64 finalizer as `mix_bits`. Rank and
register index follow `LeftoverSketch::add` exactly.

Fit checks use the bag's packing (`BagKey`, 64 or 128 bits, chosen per bag
as today) and `bag_guards`, through the existing `fits`. `$S1`'s 88 letters
take the 128-bit path.

The leftover's length (`LEFT`) comes from a per-bag letter count uploaded
beside each key: `left = letters(bag) − Σ letters(pick)`.

## The kernels

Both kernels take the same parameters and share the fit check, the
per-combination update (counts and register) and the end-of-block flush,
as `__device__` functions. They differ in how a warp walks a unit.

```cpp
template <typename BagKey>
struct LeftoverParams {
    BagKey const* keys;          // every pair bag, in PAIRS order
    uint64_t const* numbers;     // RemainderIndex::number per bag
    uint64_t const* pairs;       // PairBag::pairs per bag
    uint8_t const* letters;      // letters per bag
    int count;                   // pair bags
    int steps;
    BagKey bag;
    BagKey bag_guards;
    uint64_t bag_number;
    int bag_letters;
    unsigned long long* next_unit;   // work counter
    unsigned long long* units_done;  // mapped, read by the host
    unsigned long long* by_picks;    // [steps]
    unsigned long long* combinations;// [length + 1]
    unsigned long long* rows;        // [length + 1]
    uint32_t* registers;             // [(length + 1) << HLL_BITS]
};

template <typename BagKey>
__global__ void walk_leftovers_simple(LeftoverParams<BagKey> p);

template <typename BagKey>
__global__ void walk_leftovers_ballot(LeftoverParams<BagKey> p);
```

The kernels walk pb steps only, as `--leftovers` allows: every step uses
the one pair-bag list, so each step starts past the previous pick and
`repeats_bag` never fires. Neither kernel has list indices or a repeat
check.

These parts are the same in both:

**One warp per unit.** A warp takes the next first pick from `next_unit`
with `atomicAdd`, the same unit `PickWalk::walk` takes. Low units have the
most work (every later step starts past them), so taking them in order
puts the long units first and the short ones fill the tail.

**Last step by lanes.** Lanes stride the last step's candidates. Each fit
is a combination: the lane adds 1 to `combinations[left]`, the pairs
product × the bag's `pairs` to `rows[left]`, and the leftover's hash to the
registers.

**One step.** With `--ss pb` the unit is the leaf: lane 0 records the
unit's combination when its first pick fits, and the other lanes idle.
593K units on `$S1` take well under a second, so it needs no other shape.

**by_picks.** When a warp takes a unit whose first pick fits, lane 0 adds
1 to `by_picks[0]`. At every step past the first but the last, each
fitting candidate is a combination of that many picks, as the CPU's
`enter` counts it, and adds 1 to that step's `by_picks`. The last entry is
set from the total, as on the CPU.

**Counts.** `combinations`, `rows` and `by_picks` are per-block in shared
memory (at most `GPU_MAX_LETTERS` + 1 = 127 lengths × 2 × 8 bytes plus
`steps`), added with `atomicAdd` while the block runs and flushed to
global with `atomicAdd` when it exits.

**Registers.** 2^14 per length × up to 127 lengths × 4 bytes is about 8.3
MB, which stays in the 4080's 64 MB L2. They are 32-bit words in global
memory, updated with `atomicMax`. Nearly every rank is at or below its
register, so a plain read first skips the atomic almost every time.

The grid is sized to fill every SM with warps; each warp loops until
`next_unit` passes the unit count, and adds 1 to `units_done` (mapped
pinned memory) per finished unit.

### Kernel 1: simple (default)

**Middle steps by loop.** Every lane runs the same loop over a middle
step's candidates, one candidate at a time, so the warp stays in lockstep
and needs no ballot or shared stack: `PickWalk::descend` as one loop over
a per-thread array of steps, each entry a candidate index, rest key,
number, letters and pairs product, identical in every lane. Lane 0 adds
each fitting middle candidate to its step's `by_picks`. The 32 lanes
repeat the same middle-step work, but middle steps are about 1/N of the
fit checks (N²/2 against N³/6 for `pb,pb,pb`).

**Last-step counts.** Each fitting lane adds to `combinations[left]` and
`rows[left]` with its own shared-memory `atomicAdd`.

### Kernel 2: experimental

**Middle steps by ballot.** For each step but the last, the warp
fit-checks 32 candidates at once, one per lane, and `__ballot_sync` gives
the fitting ones. It descends into each set bit in turn, depth first, with
a per-warp stack in shared memory: one `(rest key, number, letters, pairs
product, ballot mask, next candidate)` frame per step. A step's candidates
start one past the previous pick. Each ballot adds its popcount to its
step's `by_picks`.

**Last-step counts.** Lanes with the same `left` would serialize on
kernel 1's shared `atomicAdd`s, so the warp combines them first:
`__match_any_sync(fitting, left)` groups the fitting lanes by `left`, the
group's rows are summed over its mask, and the group's leader adds the
popcount to `combinations[left]` and the sum to `rows[left]`. The register
update stays per lane, since each lane's hash is different. Kernel 1 calls
the per-combination device function with a one-lane group; kernel 2
passes the match mask.

## Options

- `bag-stats-cuda` gains `--leftovers`, `--hll` (and its alias
  `--hyper-log-log`, as in `bag-stats`) and `--kernel N` in its option
  table and usage text.
- `--kernel N` takes 1 (simple, the default) or 2 (experimental); any
  other value fails with "--kernel takes 1 or 2". It goes with
  `--leftovers` only: without it, "--kernel goes with --leftovers".
- The "--ss needs a ppb or pb step and a wb step" check
  (`bag-stats-cuda.cpp`) skips `--leftovers`.
- The check that `--leftovers` takes pb steps alone moves from
  `bag-stats.cpp` into `check_common_args`, so both tools get it. The
  `--exact`/`--all`/`--completions` half stays in `bag-stats`.
- `bag-stats-cuda` fails `--leftovers` without `--hll` ("--leftovers needs
  --hll") and `--hll` without `--leftovers` ("--hll goes with
  --leftovers", as in `bag-stats`).

## Host

A new `run_leftovers_gpu` in `bag-stats-gpu.cu`, called from
`bag-stats-cuda.cpp`'s `main` when `--leftovers` is set:

1. Load inputs as `bag-stats` does (`load_bag_stats_inputs`); build the
   `RemainderIndex` and fail with the same message when it can't number the
   bag.
2. Pack and upload the lists, numbers, pairs and letters; zero the counts
   and registers.
3. Launch the `--kernel` walk on one stream. The main thread then loops:
   sleep 100 ms, then `cudaStreamQuery`, until the stream is done. With
   `--progress N`, each time N seconds have passed it reads `units_done`
   and prints the same `pass 1/1: X/Y pair bags` line through `Progress`.
   Neither `cudaStreamSynchronize` nor `cudaDeviceScheduleBlockingSync`
   will do for the wait: on WSL2 both use a full core for the length of
   the kernel (3.35 s CPU for a 3.5 s kernel), where the loop uses 0.08 s.
4. Once the query reports the stream done, synchronize, check for errors,
   copy back counts and registers, build a `LeftoverSketch` from the
   registers and call `print_leftovers`.

`--threads` and `--streams` have no effect with `--leftovers`.

## Tests

`test-bag-stats-cuda.sh`: one `--leftovers --hll --ss pb,pb` run on the
test bag, compared byte for byte with `bag-stats --leftovers --hll`, and
the same for `--ss pb` and `--ss pb,pb,pb`; each with `--kernel 1` and
`--kernel 2`.

## Later: ppb steps

`--leftovers` with ppb steps gets its own kernel, not more branches in this
one. That kernel takes several lists, a list index per frame, the
`PickWalk::descend` start rule (past the previous pick only when both steps
use the same list) and the `repeats_bag` check, and it reuses the fit,
combination and `by_picks` device functions. The pb kernel stays as it is,
so its timings stay comparable.
