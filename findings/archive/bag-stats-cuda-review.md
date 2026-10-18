# bag-stats-cuda Review: GPU Row Counting and the Shared Run Code

`/code-review medium` on the uncommitted change that moves the shared walk,
argument parsing, loading and printing out of `source/bag-stats.cpp` into
`source/bag-stats-run.{h,cpp}`, and adds `bag-stats-cuda`
(`source/bag-stats-cuda.cpp`, `source/bag-stats-gpu.{h,cu}`,
`source/test-bag-stats-cuda.sh`). Plan: `plans/bag-stats-cuda.md`. Line
numbers refer to that working tree.

The CPU refactor (`RowWalker` → `PickWalk`, `Progress` → `CacheHooks`, the
extracted arg parsing and loading, `picks_rest` reusing its input as output)
was checked against the code it replaces, and no behavior change was found.
The GPU kernel counts the same things as the CPU `RestCounter`.

Each finding lists what the code does, when it matters, and a proposal.
Decisions are recorded under each finding as they're made.

## Status

| # | Finding | Status |
|---|---------|--------|
| 1 | Race on `s.counts` between rows | fixed |
| 2 | `print_rows` window counts blocks | fixed: window counts rows |
| 3 | `--threads 1` leaves the second stream unused | open: async `count()` with one orchestrating thread under consideration |
| 4 | Repeated rests counted again on the GPU | decided: fill `RestTable` in pass 1, serve pass 2 from it on the CPU; not yet built |
| 5 | `Packing` duplicates `PairsPacking` | dropped: GPU keys never meet CPU keys |
| 6 | `GpuRowCounter` never frees memory | fixed |
| 7 | Required `dependency('cuda')` | fixed |
| 8 | Shared sources compiled per executable | fixed: `bag-stats-rank` and `bag-stats-run` libraries |
| 9 | `CacheHooks` heap-allocates its counts | fixed: `Progress(walk, pass)` builds hooks in place |
| 10 | `census_blocks` silent under `--progress` | open |

The fixes build, and the `bag-stats`, `bag-stats-sort` and
`bag-stats-cuda` tests pass. The race in #1 shows up rarely and depends on
timing, so the tests can't confirm that fix. The no-CUDA meson path in #7
wasn't run on this machine. Line numbers in the findings below are from
before the fixes.

## 1. Race on `s.counts` between rows in `count_rows`

`count_rows` (`bag-stats-gpu.cu:201`, row loop at :217) loops over rows per block. The end of
an iteration is:

```
__syncthreads();
if (tid == 0)
    p.out[row] = {s.counts[0], ..., s.counts[6]};
```

and the top of the next is:

```
if (tid == 0) { s.row = atomicAdd(p.next, 1); ... }
if (tid < 7) s.counts[tid] = 0;
```

No barrier sits between thread 0's read of `s.counts` and threads 1–6
zeroing them. Threads 1–6 are in warp 0 with thread 0. With independent
thread scheduling (Volta and later, including the RTX 4080), the
programming model doesn't guarantee that the warp reconverges after the
`if (tid == 0)` before lanes 1–6 run the zeroing store.

**When it matters:** whenever the scheduler runs lanes 1–6 ahead of
lane 0. That row's `WB_WITH_PAIRS..MIN_WB_WITH_PAIRS` (`s.counts[1..6]`)
come out as 0 or partial. The results may be right on most runs and wrong
on a few, depending on the compiler and the GPU.

**Proposal:** have thread 0 zero `s.counts[0..6]` itself right after it
copies them to `p.out[row]` (and set them to 0 once before the loop), so
the read and the reset happen in one thread. Or move the zeroing to after
the `__syncthreads()` that follows the `s.row` claim, before any
`atomicAdd` into `s.counts`.

**Decision: fixed.** Threads 0–6 zero `s.counts` once before the row loop,
and thread 0 zeroes them right after copying them to `p.out[row]`. The next
row's first `atomicAdd` into them comes after at least one
`__syncthreads()`.

## 2. `print_rows` window counts blocks, but CUDA blocks are ~65536 rows

`print_rows` (`bag-stats-run.h:328`) lets workers run at most
`PRINT_WINDOW_PER_THREAD * walk.threads` blocks ahead of the next block to
print (`PRINT_WINDOW_PER_THREAD` = 16, `bag-stats-run.h:34`). Each finished
block's formatted lines are held in `lines[block]` until printed.

In `bag-stats` a block is one unit (a first-step pick), which is small. In
`bag-stats-cuda`, `census_blocks(..., GPU_BATCH_ROWS)`
(`bag-stats-cuda.cpp:241`) groups units into blocks of about
`GPU_BATCH_ROWS` = 65536 rows, so the window is in effect 16 × 65536 rows
per thread.

**When it matters:** `bag-stats-cuda --threads 16` with no `-n`, writing
to slow stdout (a pipe into a slow consumer, a network filesystem). Up to
256 finished blocks of ~65536 rows can be held, about 17M formatted rows,
well over 1 GB of strings.

**Proposal:** measure the window in rows. Pass a per-thread row budget to
`print_rows` (or derive it from `walk`), and let a worker take block `b`
only while the rows in `[printed, b]` stay under the budget. `bag-stats`
keeps its current behavior if the budget is 16 units' worth.

**Decision: fixed.** `PRINT_WINDOW_PER_THREAD` (blocks) is now
`PRINT_WINDOW_ROWS_PER_THREAD` = 65536 rows. `print_rows` tracks `waiting`,
the rows of finished blocks not yet printed, and a worker starts a block
only while `waiting < window`. The memory held is at most the window plus
one block per thread in flight. It can't deadlock, because when `waiting` is
over the window, the next block to print is already taken.

## 3. Default `--threads 1` leaves the second stream unused

`BagStatsArgs::threads` defaults to 1 (`bag-stats-run.h:45`), and
`bag-stats-cuda` uses `bag_stats_threads(args)` unchanged
(`bag-stats-cuda.cpp:235`). `GpuRowCounter` has `STREAMS` = 2
(`bag-stats-gpu.cu:24`) so one batch can upload or run while another is
filled, but `count()` (`bag-stats-gpu.cu:647`) doesn't return until its
stream synchronizes. With one thread, only one `count()` is ever in flight.

**When it matters:** every default run. The thread walks 65536 rows while
the GPU sits idle, then waits for the GPU while the CPU sits idle. The
two-stream design only helps with `--threads >= 2`.

**Proposal:** in `bag-stats-cuda`, default threads to `STREAMS` (export it
from `bag-stats-gpu.h`) when `--threads` isn't given, capped by hardware
threads as now.

**Decision:** pending. One thread could drive both streams if `count()` were
split into a non-blocking submit and a later wait, and `GpuRowWalker`
double-buffered its batches. Each launch already fills every SM, so the
second stream only overlaps copies and host-side sorting, not a second
kernel.

## 4. Repeated rests within a batch are counted again on the GPU

`GpuRowCounter::count` (`bag-stats-gpu.cu:647`) uploads every buffered
rest and runs the full per-row work on each: group scan, hash build,
word-bag enumeration. The CPU path has `RestTable` because different pick
orders and pair combinations leave the same remainder.

**When it matters:** multi-step searches such as `--ss pb,pb,wb,p`, where
many rows in a batch share a rest. Each duplicate costs a full kernel row.

**Proposal:** `count()` already sorts the rows into `order` by width and
length. Extend the sort key with the rest bytes, upload each distinct rest
once, and copy each result back out to every row that shares it when
unpacking `out`.

**Measured.** A temporary count in `count()`, since removed, compared
repeats within a batch to the CPU `RestTable` hit rate over the whole run:

| Search | Repeats, whole run | Repeats within a batch |
|---|---|---|
| $S9, one `-u` word, `--ss pb,pb,wb,p` | 20.8% | 1.4% |
| $S8, two `-u` words, `--ss pb,pb,wb,p` | 41.9% | 2.2% (first 20M rows) |
| $S7, one `-u` word, default `--ss` | cache off (one step) | n/a |

Repeats come from different pick combinations with equal letter sums,
which the walk reaches far apart, so deduplicating within a batch would
save about 2%.

**Decision: not as proposed.** No deduplication per batch, and no cache
lookups during pass 1. Instead, when ranking needs two passes,
`bag-stats-cuda` keeps a `RestTable`. Pass 1 counts every row on the GPU
and `put`s each rest's counts into the table without calling `get`.
Pass 2 reads the table on the CPU, so the GPU does no work there. In the
CPU-only tool, pass 2 then runs in seconds, for example `pass 2/2: done
in 5s, cache hits 887968/887968 (100.0%)` on a `--ss ppb,pb,wb,p --pp`
run. Only rests missing from a full table go to the GPU in pass 2.
`bag-stats-cuda` gets no pair cache (`--pair-cache-size`).


## 5. `Packing` and `rest_bits` duplicate `PairsPacking`

`Packing` (`bag-stats-gpu.cu:391`) and `rest_bits` (`:418`) re-implement the field
widths, guard bits and shifts of `PairsPacking` in `pairs-impl.h`, because
the `.cu` file stays free of project headers. The 64/128-bit key choice and
the key layout both depend on that rule.

**When it matters:** any change to `PairsPacking`'s widths or guard
placement. The GPU keys would stop matching what the CPU side assumes,
with no compile error.

**Proposal:** build `PairsPacking(inputs.bag)` in `bag-stats-cuda.cpp` and
pass `shifts`, `guards` and `bits` (and the pre-packed group and word keys
if useful) through `GpuInputs`. The `.cu` still includes only
`bag-stats-gpu.h`.

**Decision: dropped.** GPU keys never meet CPU keys. The group keys, the
word keys, `bag_shifts` and `bag_guards` all come from the one `Packing`
inside the `.cu`, and `bag-stats-cuda.cpp` only prints `key_bits()`. A
change to `PairsPacking` would leave the GPU consistent with itself and
its counts correct.

The fix would need only one new `PairsPacking` accessor, `shifts()`, since
`bits()`, `guards<Key>()` and `pack<Key>()` are already public. But it
would move the packed keys into `GpuInputs`, which is one plain struct.
Today the width is chosen once (`key_bits`), and `upload_keys<Key64>` or
`upload_keys<Key128>` packs into a vector of exactly that width.
`GpuInputs` would need both widths' vectors, a 128-bit vector narrowed on
upload, one or two `uint64_t` words per key, or a key-type template on
`GpuRowCounter`. The raw `group_letters`, `word_letters` and `bag` would
stay, because the kernel and `lenstart` read them. Only `Packing` would go:
`rest_bits` and the kernel's own packing of each rest would stay in the
`.cu` either way.


## 6. `GpuRowCounter` never frees GPU or pinned memory

`~GpuRowCounter()` is `= default` (`bag-stats-gpu.cu:639`), and `Impl` has
no destructor. The device buffers, the hash slots, both `cudaStream_t`s and
the `cudaMallocHost` pinned buffers are never released.

**When it matters:** not today. The counter lives until `main` returns, and
process exit frees everything. It would leak (about 1 GB or more of hash
slots for a large dictionary, plus pinned host memory) only if a counter
were built more than once in one process.

**Proposal:** give `Impl` a destructor that calls `cudaFree`,
`cudaFreeHost` and `cudaStreamDestroy` for each allocation. Or leave it,
since nothing builds a second counter.

**Decision: fixed.** `Impl::allocate` records each `cudaMalloc` buffer, and
`~Impl` destroys the streams and frees the pinned host buffers and every
device buffer.

## 7. `dependency('cuda')` is required inside `if have_cuda`

`meson.build:268` sets `have_cuda` from
`add_languages('cuda', required: false)`, then line 275 uses
`dependency('cuda')`, which is required.

**When it matters:** a host where `nvcc` is on `PATH` but meson can't find
the CUDA toolkit libraries (non-standard install location, partial
toolkit). `add_languages` succeeds, `dependency('cuda')` errors, and
configuring the whole project fails, so `conan build .` breaks for every
target instead of skipping `bag-stats-cuda`.

**Proposal:**

```
have_cuda = add_languages('cuda', required: false, native: false)
cuda_dep = have_cuda ? dependency('cuda', required: false) : disabler()
if have_cuda and cuda_dep.found()
```

**Decision: fixed,** as proposed.

## 8. `bag-stats-run.cpp` and `bag-stats-rank.cpp` compile once per executable

`bag-stats` (`meson.build:262`) and `bag-stats-cuda` (`meson.build:271`)
both list `bag-stats-run.cpp` and `bag-stats-rank.cpp` as sources, and
`bag-stats-sort` (`meson.build:282`) lists `bag-stats-rank.cpp` too. Each
executable compiles its own copy.

**When it matters:** build time on every change to those files. It doesn't
affect output.

**Proposal:**
`bag_stats_run_lib = static_library('bag-stats-run', ['bag-stats-run.cpp', 'bag-stats-rank.cpp'], ...)`,
linked by all three executables the way `bag_stats_lib` is. Or split out
`bag-stats-rank.cpp` alone if `bag-stats-sort` shouldn't pull in the run
code.

**Decision: fixed.** Two static libraries, `bag_stats_rank_lib`
(`bag-stats-rank.cpp`) and `bag_stats_run_lib` (`bag-stats-run.cpp`).
`bag-stats` and `bag-stats-cuda` link both, and `bag-stats-sort` links only
the rank library.

## 9. `CacheHooks` puts its counts on the heap only to be movable

`CacheHooks` (`bag-stats.cpp:748`) holds `std::unique_ptr<Counts>`
(`bag-stats.cpp:799`). `Counts` holds two `SharedCacheCounts`, which hold
atomics and so can't be moved. `progress_hooks()` (`bag-stats.cpp:819`)
returns a `CacheHooks` by value, and `Progress` takes it by value.

**When it matters:** one extra allocation and indirection per pass. It
doesn't affect results or measurable speed.

**Proposal:** have `Progress` take its hooks by reference (or construct them
in place) and let `CacheHooks` hold `SharedCacheCounts` directly.

**Decision: fixed.** `Progress` is now templated on the walk and built as
`Progress(walk, pass)`. It takes the program, interval, passes and units
from the walk, and initializes its hooks member straight from
`walk.progress_hooks()`, which guaranteed copy elision allows without a
move. `CacheHooks` holds its two `SharedCacheCounts` directly.

## 10. `census_blocks` walks every row with no progress output

`bag-stats-cuda` calls `census_blocks` (`bag-stats-run.cpp:280`) before
building the `Walk`. It runs `PickWalk` over every unit just to count rows
per unit, so it can cut blocks of about `GPU_BATCH_ROWS`. It prints
nothing.

**When it matters:** large `LETTERS`/`--pairs` runs with `--progress`. The
census is a full extra walk of all pick combinations, and the user sees a
silent stall before the `pass 1/N` reports start.

**Proposal:** when `args.progress != 0`, report the census's start and its
time (and row total) to stderr, in the same style as the device line at
`bag-stats-cuda.cpp:231`.

**Decision:** pending.
