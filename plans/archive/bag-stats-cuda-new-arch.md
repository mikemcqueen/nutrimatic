# Plan: bag-stats-cuda counts each distinct leftover once, off the walkers

## Status

Planned. `bag-stats-cuda` only; `bag-stats` keeps its walk.

## Outcome

Today every `bag-stats-cuda` walker thread walks rows, hands each full
batch of rests to `GpuRowCounter::count()` and blocks in
`cudaStreamSynchronize` (or on the stream pool) until the counts come back.
Walker threads equal `--streams`, so the CPU side runs on one or two
threads, and every row's rest is counted on the GPU, repeats included:
about 2G rests for the 105M distinct leftovers of `$S9` `pb,pb,pb,wb,p`.

After this change:

1. **Find (CPU, `--threads` walkers).** Walk every row once. Each rest is
   claimed in the shared leftover table; a rest claimed for the first time
   goes into the walker's current batch. A full batch goes to the feeder.
2. **Count (GPU, one feeder thread, `--streams` streams).** The feeder
   counts batches on the GPU while phase 1 is still walking, and writes each
   leftover's counts into its table entry.
3. **Emit (CPU, `--threads` walkers).** The existing passes in
   `bag-stats-run.h` (`print_rows`, `count_sort_values`, `top_rows`,
   `find_row`, `score_rank`) walk the rows again. Each row's counts come
   from a table lookup. With a table big enough for every leftover, phase 3
   never touches the GPU.

Walkers never wait on the GPU except when the GPU is the bottleneck: each
walker has at most 2 batches in flight. Output is byte-identical to today
for every mode, `--by-pick`, `--score`, `--score-detail`, `--best-pairs`
and `--top-segments` included.

```
bag-stats-cuda ... [--threads N] [--streams N] [--cache-size N] ...
```

- `--threads N`: walkers for phases 1 and 3 (default 0, every hardware
  thread). No longer tied to `--streams`.
- `--streams N`: the GPU streams the feeder keeps in flight (default 1).
- `--cache-size N`: the leftover table's MB, as today (`RestTable`, 48
  bytes per entry). Set it from `bag-stats --leftovers`' `cache-size` line.

## The leftover table

`RestTable` (`bag-stats-cache.h`) keeps its layout: keys (remainder number
+ 1) in open addressing, a row index per slot, 0 until counted, and the
`CachedCounts` rows. Its `put` splits in two:

```cpp
// A claimed entry: its slot and its row.
struct RestClaim {
    size_t slot;
    uint32_t row;
};

// Claims `rest`'s entry; true with `out` set when this call added it,
// false when it was there or the table is full.
bool claim(PairsCounts const& rest, RestClaim* out) const;

// Writes a claimed entry's counts and publishes them to get().
void fill(RestClaim const& claim, BagStatsCounts const& counts) const;
```

`claim` is `put` up to taking the row: CAS the key into its slot, then
`next_.fetch_add`; past capacity it returns false and the key stays without
a row. `fill` writes `rows_[row]` and release-stores `rows_index_[slot]`.
`get` is unchanged and returns false for a claimed entry until it is
filled. The `cache full` report stays in `claim`.

The table is enabled whenever `RemainderIndex` numbers the bag; without it
every rest is a miss (see "Misses").

## Batches and the feeder

```cpp
// Up to GPU_BATCH_ROWS rests to count, and where each one's counts go.
struct LeftoverBatch {
    std::vector<uint8_t> rests;         // GPU_LETTERS_BYTES apiece
    std::vector<RestClaim> claims;      // phase 1: table entries
    size_t owner;                       // the walker whose semaphore frees
    PendingBlock* block;                // phase 3 misses: null in phase 1
};
```

Each walker owns a `std::counting_semaphore<2>`. Before handing the feeder
a batch it acquires one; the feeder releases it when the batch's counts are
written. A walker therefore holds at most 2 batches in the feeder's queue
or on the GPU, about 2 × 65,536 × 32 bytes of rests.

The queue is a mutex-guarded deque with a condition variable the feeder
sleeps on. Walkers hold the mutex only to push a pointer, once per 65,536
leftovers.

The feeder is one dedicated thread, not a walker. It owns every stream:

- While a stream is free and the queue is non-empty, it sorts the batch by
  rest length (as `count()` does now), copies it into that stream's pinned
  buffer and queues H2D copy, kernels and D2H copy, then records an event.
- When no stream is free or the queue is empty, it waits on the oldest
  in-flight stream's event, writes that batch's counts (`fill` for phase 1
  claims, the block's rows for phase 3 misses) and releases the owner's
  semaphore.
- It exits when the walkers are done and the queue and streams are empty.

`GpuRowCounter` drops `count()`, its stream pool mutex and condition
variable, and gains the feeder's calls:

```cpp
// Streams the feeder can use.
size_t streams() const;
// Starts counting `rows` rests on stream `s`, which must be idle.
void launch(size_t s, uint8_t const* rests, size_t rows) const;
// Waits for stream `s`'s counts; valid until the next launch on `s`.
GpuCounts const* finish(size_t s) const;
```

The one-row `--score` without SORT options (`print_score_row`) runs with
no walkers, so it calls `launch(0, ...)` and `finish(0)` directly.

## Phase 1: find

A new `run_find(walk)` in `bag-stats-cuda.cpp` runs `--threads` walkers over
the units with an atomic next-unit counter, as `count_leftovers` does. Each
walker uses `PickWalk` directly and, per row:

1. `claim(rest)`; when it returns true, appends rest and claim to its
   batch.
2. A full batch: acquire the semaphore, push, start a new batch.

It also counts each unit's rows, which replaces `census_blocks`' own walk:
the blocks are built from these counts once phase 1 ends. At the end each
walker pushes its partial batch; `run_find` returns once the feeder has
drained everything.

Progress: `Progress` with its own pass label, `find`, and hooks text
`", N leftovers, M counted"` (claims, fills).

## Phase 3: emit

`TableRowWalker` replaces `GpuRowWalker`. For a hit it visits the row
straight from `get()`. The passes in `bag-stats-run.h` keep their structure;
`walk.passes` still decides whether `count_sort_values` runs first, and
`--by-pick` keeps working because every row's picks are walked as now.

### Misses

A row whose rest isn't in the table (table full, or no `RemainderIndex`)
still needs counts. The walker buffers the whole block once it meets a
miss: each row's unit, walk sequence, picks, pair length and rest, as
`GpuRowWalker` buffers now, and puts the misses (deduplicated within the
block) into a `LeftoverBatch` pointing at that `PendingBlock`. It acquires
its semaphore, pushes the batch and walks its next block. Before taking a
third block while two are pending, it acquires, which waits for the oldest.
When a pending block's counts are written, the owning walker visits its
rows in order and reports the block done.

Pending blocks hold at most 2 blocks of rows per walker. They are not a
cache: a leftover missing in many blocks is counted once per block.

### Blocks finish out of call order

Because a block can wait for its misses, the passes stop assuming a block
is done when `walker.walk()` returns. The walker interface becomes:

```cpp
// Walks block `b`; calls visit(unit, row, pair_length) for its rows in
// order and then done(b), now or after its misses are counted.
template <typename Visit, typename Done>
void walk_block(size_t b, Visit const& visit, Done const& done);

// Finishes every pending block.
template <typename Visit, typename Done>
void drain(Visit const& visit, Done const& done);
```

`bag-stats`' `RowWalker` implements it by walking the block and calling
`done(b)` at once, so the CPU tool is unchanged in behavior. The per-unit
`seq` in `top_rows` and `score_rank` comes from the buffered rows' walk
sequence, so ties still break in walk order.

### Printing

`print_rows` keeps walk order. Its block's text moves into `lines[b]` in
`done(b)`. `bag-stats-cuda`'s blocks stay `census_blocks` of about
`GPU_BATCH_ROWS` rows (never splitting a unit), and the print window
becomes `PRINT_WINDOW_BLOCKS_PER_THREAD = 4` blocks per walker instead of
65,536 rows per thread, so a slow block rarely stalls the others.
`bag-stats` keeps `unit_blocks` and its row window.

## bag-stats --leftovers: the cache size

This line may already exist: `plans/archive/bag-stats-cuda-hll-gpu.md`
adds it to `print_leftovers` in `bag-stats-run`. If it doesn't,
`count_leftovers` prints, after `pair bags`, the `--cache-size` that holds
every distinct leftover:

```
cache-size N
```

N is the distinct leftovers × `REST_CACHE_ENTRY_BYTES`, in MB rounded up,
with `--hll` from the estimate × 1.03 (3σ of HLL's 0.8%). It applies to a
`bag-stats-cuda` `--ss` with the same pb steps; `--leftovers` takes no ppb
steps.

## Removed

`RestMode`, `GpuRowWalker`, `GpuCacheHooks`' fill/lookup split, the
`streams`-as-threads wiring in `main`, and `GpuRowCounter::count()`. The
`--streams` help line drops "each fed by its own thread".

## Tests

`test-bag-stats-cuda.sh` already compares rows with `bag-stats`. Add one run
with `--cache-size 0` so every row goes through the miss path, and one with
`--threads 4 --streams 1`, both compared with `bag-stats`.

## Future: leftovers on disk

Not built now. For bags whose distinct leftovers don't fit in RAM ($S1
`pb,pb` has about 45.8G):

- Phase 1 walkers append each leftover key to per-thread, per-bucket files
  (bucket by hash), whole fixed-size records, no shared files and no locks.
- Each bucket is loaded, deduplicated and counted on the GPU in turn, and
  written back as a sorted key → counts file. Disk reads stay sequential.
- With `-n` and without `--by-pick`, a row's sort value depends only on its
  leftover, so per-leftover sort values weighted by their row multiplicity
  give the census and the top-N threshold without a walk; phase 3 then
  holds only the leftovers that can make the top N. `--by-pick` scores
  depend on the picks, so it needs another design.
- A full dump needs every row's counts in walk order, which means random
  lookups; it stays limited to bags whose table fits in memory.
