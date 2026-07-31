# Plan: rebuild the top-N heap to shrink its critical section

## Goal

Cut the work done inside `DfsTopN::heap_mutex` so that a `-S 20` run stops
behaving like a two-core run. The target workload is `-n 1000000`, where the
observed idleness occurs **during the fill phase**, before the heap has ever
reached `result_limit` and published a score floor.

Non-goal: making mutex acquisition fair. `findings/heap-contention.md` already
argues that unfair barging may maximize throughput and that yielding is a
benchmark experiment, not a fix. This plan reduces the serialized work instead.
Wall time is the accepted metric; CPU-utilization graphs are not.

## Why the fill phase is the expensive one

Two effects compound, and both are specific to a large `-n`.

1. **Nothing prunes.** `publish_floor()` returns immediately unless
   `heap.size() == result_limit` (`source/dfs-output.cpp:78-85`). Until the
   millionth spelling lands, `score_floor()` returns false, so the early-out at
   the top of `emit()`, the `pending` cutoff at `dfs-output.cpp:117`, and the
   in-lock cutoff at `dfs-output.cpp:136` all no-op. Every expanded spelling is
   unconditionally serialized through the mutex, and phase-2 search gets no
   bound pruning either.

2. **Every heap swap costs two string hashes.** `positions` maps the word-set
   key to a *heap index*, so keeping it correct means rewriting it on every
   swap:

   ```cpp
   void DfsTopN::swap_heap_entries(size_t a, size_t b) {   // dfs-output.cpp:216
     std::swap(heap[a], heap[b]);
     positions[heap[a].word_set_key] = a;   // string hash + probe
     positions[heap[b].word_set_key] = b;   // string hash + probe
   }
   ```

   A sift is therefore `O(log N)` hashes of 20-60 byte strings with random
   probes into a ~100 MB table, not `O(log N)` double comparisons. At
   `N = 1e6` a full-depth sift is ~19 levels, so ~38 hash+probe operations,
   plus the `erase`/insert pair in the eviction path.

Additionally, `offer()` takes `DfsSpelling const&` and *copies* it into the
heap (`dfs-output.cpp:195`, `:202`), so two string allocations plus one
`unordered_map` node allocation happen while the mutex is held.

### One claim that is only partly established

Within a single `emit()`, `pending` is a max-heap on score (`ExpansionOrder`,
`dfs-output.cpp:42-48`), so spellings pop in strictly descending score. Each is
therefore weaker than the last, which is the adversarial direction for a
min-heap `sift_up`: a new global weakest travels to the root. Across separate
`emit()` calls the arrival order is **not** established, so the true average
sift depth is unknown and may be well below the worst case.

This does not change what phase 1 does. Phase 1 removes the per-swap hashing
whatever the depth turns out to be, and removes the copies and allocations
under the lock regardless. It only affects how large the win is.

## Settled decisions

- **The map node is the payload.** `std::unordered_map` guarantees pointer and
  reference stability for elements across rehash (only iterators are
  invalidated), so the heap can hold pointers into the map. This removes the
  separate slot array, stores the word-set key exactly once as the map key,
  and gives eviction a zero-allocation path via C++17 node handles.

- **The heap stores the score inline.** `HeapSlot` is 16 bytes, so a sift
  touches ~20 predictable cache lines out of a 16 MB array. A bare pointer
  would make every comparison a random dereference into the ~100 MB map,
  which is 2-3 near-certain cache misses per level — inside the mutex, which
  is exactly what we are trying to shrink. The cost is that the score lives in
  two places and the "improve an existing key" path must update both.

- **The heap points at the whole map entry, not at the mapped value.**
  `weaker()` tie-breaks on `word_set_key` and then `text`
  (`dfs-output.cpp:12-17`), and the word-set key is the map *key*. A
  `RetainedSpelling*` cannot reach it; a pointer to the key/value pair can, via
  `->first`. That pair is `RetainedMap::value_type`, typedef'd below as
  `RetainedEntry`.

- **No heap order during fill.** Below `result_limit` nothing is ever evicted
  and nothing reads `heap[0]`, so maintaining heap order buys nothing. Append
  unheapified, then build the heap once in `O(N)` at the moment the limit is
  reached.

- **Deduplication stays globally exact and the top-N stays exact.** Sharding
  the dedup table by key hash is rejected: an exact global top-N would need N
  entries per shard, and 20 x 1e6 is not affordable.

- **Do not rely on `extract()`/`insert()` preserving the element address.**
  Every implementation relinks the same allocation, but the standard does not
  clearly guarantee it. Re-read the pointer from the returned iterator and
  store it back into the heap slot; that is one store.

## Target structures

```cpp
struct RetainedSpelling {          // the map key IS the word-set key
  std::string text;
  double log_score;
  size_t heap_pos;
};

typedef std::unordered_map<std::string, RetainedSpelling> RetainedMap;

// std::pair<std::string const, RetainedSpelling>: the key/value pair the map
// stores. The heap points at this rather than at the mapped value alone, so
// that weaker()'s tie-break can reach the word-set key through ->first.
typedef RetainedMap::value_type RetainedEntry;

struct HeapSlot {                  // 16 bytes; the only thing sifts touch
  double log_score;
  RetainedEntry* retained;
};

RetainedMap retained;
std::vector<HeapSlot> heap;
```

A sift swaps two 16-byte slots and writes back
`slot.retained->second.heap_pos` — two integer stores, no hashing. Per
replacement the map is touched twice (locate the incoming key, recycle the
evicted node) instead of once per swap.

## Phases

Each phase leaves the tree buildable, keeps stdout byte-identical for a fixed
`-n` and letter set, gets a `/code-review` before commit, and states its own
smoke checks. Tests stay minimal per `CLAUDE.md`.

### Phase 1 — decouple the dedup table from heap positions

Done first, with no measurement prerequisite.

Replace `vector<DfsSpelling> heap` + `unordered_map<string, size_t> positions`
with the structures above. Mechanical; preserves every current semantic.

- `offer()` takes its argument by value and moves into the map.
- The eviction path uses `retained.extract(evicted_key)`, overwrites
  `nh.key()` and `nh.mapped()`, and reinserts — no malloc, no free. The
  incoming key is known absent because the `find` happened under the same lock.
- `swap_heap_entries()` writes `heap_pos` through the slot pointer.
- `take_sorted_results()` materializes `DfsSpelling` by moving strings out of
  the map, then sorts as today.
- Preserve: the zero-limit inert case, sink reuse after drain (both covered by
  `heap_churn_test`, `source/test-dfs-output.cpp:155`), and the monotone floor
  contract documented at `dfs-output.h:56-61`.

Smoke check: existing `heap_churn_test` and `concurrent_top_n_test`
(`test-dfs-output.cpp:230`) pass unchanged. Add one case that improves the
score of an existing key *after* eviction has recycled a node, since that is
the path where a stale `heap_pos` or a stale slot pointer would survive.

### Phase 2 — stop heapifying during fill

This is the change aimed squarely at the reported symptom. Add the fill-phase
diagnostic first, so the phase carries its own before/after number.

In `publish_floor()`, on the not-full-to-full transition only, emit
`info: worker %d published first floor (%g) after %zu spellings`.
`dfs_diagnostic` already timestamps every line, so this dates the end of the
fill phase directly. Get the thread id from `gettid()` at the call site and
pass it as an argument — no thread-local, no change to `DfsSolutionSink`, no
new helpers in `dfs-diagnostic.h`. The only new state is a `FILE*` set from
`dfs-anagrams.cpp` under `--verbose` and a `bool` guarding the first
publication; both live under `heap_mutex`, which `publish_floor()` already
holds.

Then add a `bool heapified` flag.

- Below `result_limit`: append to `heap` with no sift, and record
  `heap_pos = heap.size() - 1`. The "existing key, better score" path
  overwrites in place and does **not** sift.
- On reaching `result_limit`: build the heap in `O(N)` by calling the existing
  `sift_down()` for `i` from `n/2 - 1` down to `0`, then publish the floor.
  Use the hand-rolled sift rather than `std::make_heap` — `make_heap` builds a
  max-heap under its comparator, so it would need `weaker` with reversed
  arguments, and getting that polarity wrong fails silently.
- Nothing reads `heap[0]` while unheapified: `floor_log_score()`
  (`dfs-output.cpp:183`) and `publish_floor()` both already guard on
  `heap.size() != result_limit`.

Fill-phase cost per spelling drops to one map find plus one insert.

Smoke checks: a `--verbose` run with a small `-n` prints exactly one
first-floor line, and `-n 0` prints none; a run with `-n` larger than the total
number of spellings never heapifies and still returns correctly sorted results.

### Phase 3 — worker-local batching

Buffer spellings per worker and merge one batch per lock acquisition, cutting
acquisitions by two to three orders of magnitude and amortizing the cache-line
ping-pong on the heap and map.

- The buffer is worker-local, so it needs no lock; the merge loop runs under
  `heap_mutex` exactly as `offer()` does today, which keeps deduplication and
  the top-N globally exact.
- A batch may contain duplicates of itself; the merge handles them the same
  way it handles duplicates against the existing table.
- The floor publishes slightly later. `dfs-output.h:56-61` already documents
  that a stale lower floor only causes extra work and can never prune a
  retained spelling, so this is a throughput trade, not a correctness change.
- Batch size is a tunable; start at 1024 and measure. Flush on
  worker completion.

Smoke check: `concurrent_top_n_test` results are identical to the serial path
for the same input, as it already asserts.

## Memory note

`DfsSpelling` is 72 bytes, so today's `heap.reserve(result_limit)` alone is
72 MB at `-n 1000000`, before the two string allocations per entry and the map
nodes. First-touch page faults on that storage land inside the mutex. The
target structures shrink the sifted array to 16 MB and remove the duplicated
key string, but the payload total is comparable; this plan does not claim a
memory win beyond dropping one `std::string` copy per retained spelling.
