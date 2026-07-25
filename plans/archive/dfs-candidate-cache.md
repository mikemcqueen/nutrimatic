# Plan: bounded candidate caching for `dfs-anagrams`

## Outcome

Replace the repeated candidate-fit scan in `DfsAnagramSearch::walk()` with a
bounded cache from the remaining letter bag to the sorted global class indexes
that fit that bag.

The implementation must retain the current DFS node count, solution count,
solution order, scores, and output bytes. Cache exhaustion or an input that
cannot be packed safely must fall back to the current scan and affect
performance only.

The cache removes the 38.9 billion repeated fit tests, but its replacement is
still exceptionally hot: 70.6 million node lookups and 181.6 million
known-fitting candidate traversals on the reference workload. Treat cache
lookup, suffix seek, class metadata access, subtract/restore, and recursion as a
single low-level kernel.

The reference target is the measured vector-cache result in
`findings/dfs-codex-perf.md`: about 3.3 seconds for the 19-letter count-only
workload instead of 80.4 seconds. The production representation below removes
the prototype's per-entry vector allocations and uses 32-bit class indexes, so
it should meet or improve that time with less than the prototype's 39 MiB RSS
increase.

## Settled design

### 1. Give each remaining bag a mixed-radix key

Store the hot bag in rarest-rank order rather than character-code order; this
makes bag index zero the first forced rank and lets one bit mask serve both
forced-rank discovery and support checks. At the start of a run, let the
original count at rank `r` be `limit[r]`.
Precompute:

```
radix[r]      = limit[r] + 1
multiplier[0] = 1
multiplier[r] = product(radix[0..r-1])
bag_key       = sum(bag[r] * multiplier[r])
```

This is a collision-free integer ID for every sub-bag of the input. Compute the
state product and every multiplication with checked `uint64_t` arithmetic. If a
radix or product cannot be represented, disable caching for that run and use
the existing fit loop.

Precompute one `uint64_t class_delta` per class:

```
class_delta[c] =
    sum(class[c].letters[i].count * multiplier[class[c].letters[i].rank])
```

On the recursive path, update the key once with `key -= class_delta[c]`, and
restore it with one addition. Continue subtracting and restoring `bag` itself,
because it is still needed to find the forced symbol and to build new entries.
This avoids repeating a multiply for every distinct symbol on every transition,
which was already visible in the cached profile.

### 2. Store cached candidates in one contiguous 32-bit arena

The cache value is a sorted list of global class indexes, in the exact order in
which the class bucket is already stored. Store each dense entry in one
`uint64_t`, with the arena offset in one half and count in the other:

```
uint64_t entry_meta;       // packed uint32_t offset + uint32_t count
uint32_t candidate_ids[];  // one contiguous arena
```

Reserve two impossible packed values for unseen and bypassed/not-admitted.
Offsets can never reach `UINT32_MAX` under the configured budget, so these
sentinels cannot alias a valid slice. A count of zero is therefore an ordinary
cached empty list. One aligned 64-bit load performs the dense lookup; there is
no separate state-array load.

Allocate the metadata and arena to their fixed maximum sizes with C++17 aligned
allocation on 64-byte boundaries. Track the arena's used length manually, so it
never reallocates and recursive insertions never invalidate a parent slice.
Align the arrays, not each entry or slice: padding thousands of short lists to
cache-line boundaries would waste bandwidth and cache capacity.

Only enable the cache when `classes().size() <= UINT32_MAX`. A larger class list
continues with the existing `size_t` scan. Keep the public path and sink indexes
as `size_t`; the 32-bit type is an internal cache encoding.

This layout is preferable to `vector<uint32_t>` per bag:

- no allocator call or vector header per visited bag;
- compact, sequential candidate storage;
- stable slices during recursion;
- exact accounting against the byte budget.

### 3. Use a dense lookup when it fits, otherwise a fixed sparse lookup

The key space size is the mixed-radix state product.

- **Dense fast path:** allocate one packed 8-byte entry per theoretical state
  when that metadata fits its share of the cache budget. Lookup is the single
  aligned load `metadata[bag_key]`. This is the path for the documented
  55,296-state workload.
- **Sparse fallback:** use a preallocated, open-addressed table keyed by
  `uint64_t bag_key`, with a fixed maximum load factor and no deletion. Avoid
  `std::unordered_map`: node allocations and implementation-dependent overhead
  make it slower and prevent exact memory accounting. Use `UINT64_MAX` as the
  empty-key sentinel; valid mixed-radix keys are smaller than the checked state
  product. Store keys and packed entry metadata in separate 64-byte-aligned
  arrays: an unsuccessful probe touches only dense key cache lines, while a hit
  loads metadata once. Use a power-of-two capacity, a strong 64-bit finalizer
  for the patterned mixed-radix keys, and linear probing. Start at a 50% maximum
  load factor; raise it only if budget sweeps show that extra candidate storage
  is worth more than shorter probe chains.
- **Uncached fallback:** if key construction, 32-bit ID encoding, metadata
  allocation, or candidate-list admission is unavailable, run the current
  bucket scan for that state.

Choose dense, sparse, or uncached once in `run()`, not at each node. Dispatch
once into separately instantiated `walk<dense>`, `walk<sparse>`, or
`walk<uncached>` kernels so the compiler removes cache-mode branches from every
recursive call. Allocate all cache metadata and reserve the candidate arena
before entering `walk()`.

Start with a 64 MiB default total cache budget, configurable from the CLI as
`--candidate-cache-mib` (`0` disables it). Budget accounting includes dense or
sparse metadata and the candidate-ID arena. Allocation reserves the address
range up front, while pages are committed as they are touched; charge only
admitted IDs. Sweep smaller budgets in the benchmark phase before locking the
default.

For sparse mode, give metadata enough fixed slots to cache the smaller of the
theoretical state count and the count allowed by the metadata share, then give
the remaining bytes to candidate IDs. Dense mode uses its exact metadata cost
and assigns all remaining bytes to IDs.

### 4. Build entries from the whole forced-symbol bucket

At a cache miss:

1. Derive the forced symbol from the current bag as today.
2. Scan from `candidate_begin(forced_symbol)` to
   `candidate_end(forced_symbol)`, regardless of the current `entry_point`.
3. Apply the current multiplicity fit test and append every fitting global
   class index to a reusable build buffer.
4. If the entry fits the remaining arena and metadata budgets, copy the indexes
   into the arena and publish the entry only after the copy is complete.
5. If it cannot be admitted, mark it bypassed where metadata permits and
   execute the original scan for this visit.

The build buffer is reused across misses and is consumed before recursion
starts, so it does not need to be recursion-depth-sized. Stop a build early once
it is certain not to fit the remaining ID budget; the fallback scan then
preserves behavior without temporarily exceeding the budget.

At a cache hit, lower-bound `entry_point` within the cached slice and traverse
that suffix. Do not copy the slice and do not re-test candidate letters.

Conceptually:

```
entry = cache.find(bag_key)
if entry is cached:
    first = lower_bound(entry.ids, entry_point)
    visit each class ID in [first, entry.end)
else if entry can be built and admitted:
    build from the complete forced-symbol bucket
    first = lower_bound(new_entry.ids, entry_point)
    visit each class ID in [first, new_entry.end)
else:
    run the existing scan from max(candidate_begin, entry_point)
```

Factor the body that processes a known-fitting class into a small inline helper
or a local loop abstraction shared by cached and fallback traversal. Do not
route every transition through a virtual call or `std::function`.

### 5. Pack the class data consumed by the kernel

The existing `DfsAnagramClass` is rich output data and points to a separately
allocated `letters` vector. Do not access it from the optimized inner loops.
Build a compact, read-only phase-2 view after the class order is final:

```
struct HotClass {
  double best_member_log_score;
  uint64_t bag_key_delta;
  uint64_t support_mask;
  uint32_t letters_offset;
  uint32_t packed_length_and_count;
};

uint32_t packed_letters[];
```

Keep `HotClass` at 32 bytes so exactly two records occupy a 64-byte cache line.
Store all requirements in one 64-byte-aligned `packed_letters` array. Pack a
symbol and count into one 32-bit word when the input bounds permit it; otherwise
select the unoptimized representation for that run. The class ID indexes
`HotClass` directly, eliminating the current 88-byte stride and per-class
pointer chase.

Encode each class support mask in rarest-rank bit order and maintain the bag in
that same order with a 36-bit nonzero-rank mask. For first-time cache builds and
the uncached fallback:

1. reject on `(candidate.support_mask & ~bag_mask) != 0`;
2. treat all count-one requirements as proven by that mask;
3. inspect only the packed requirements whose count is greater than one.

This gives the 79.75% absent-symbol rejection case one sequential metadata load,
one `and-not`, and one predicted branch. Store the repeated-count range
contiguously or put its count first in the class requirement slice so the
multiplicity loop does not branch on `count == 1`.

On a known-fitting transition, load the `HotClass` record once, use its length,
score, and delta, and touch the packed full-requirement slice only if the path
will actually recurse. A complete solution needs no bag subtraction; a
depth-rejected candidate needs neither score arithmetic nor path mutation.

### 6. Shape branches and memory traffic for the common case

- Mark cache hits and ordinary non-progress nodes as likely, and cache misses,
  progress printing, overflow fallbacks, and solution completion as unlikely,
  using small compiler-portable wrappers around `__builtin_expect`. Confirm the
  resulting layout in annotated assembly; hints that worsen layout are removed.
- Replace `nodes % progress_interval` with `next_progress`. The node kernel does
  one unlikely equality comparison, and only the reporting path advances the
  threshold.
- Maintain the bag's nonzero-rank mask during subtract/restore. After the
  existing empty-bag completion check, find the forced rank with
  `__builtin_ctzll`, which is a single `bsf`-class operation under the existing
  `x86-64-v2` target, instead of scanning up to 36 counters. Packed
  requirements contain the rank directly, so subtract/restore needs no
  symbol-to-rank lookup.
- Keep `bag`, the rank mask, current key, path size, and cache pointers in the
  search object/local frame with narrow integer types where their bounds permit.
  Hoist invariant pointers and limits out of candidate loops.
- Express subtraction and restoration as tight pointer loops over packed
  requirements. Use `__restrict__` only where ownership proves non-aliasing and
  assembly confirms it removes reloads.
- Specialize the CLI `-n 0` path to pass a null sink, avoiding tens of millions
  of virtual no-op `emit()` calls in the count-only benchmark.
- Keep the recursive kernel out of line and hot; force-inline only small
  non-recursive lookup/decode helpers. Inlining a large candidate body twice can
  increase instruction-cache pressure, so choose the shared-helper boundary
  from the final profile and generated code.

Use a branchless lower-bound implementation as an A/B candidate against
inlined `std::lower_bound`. Cached lists are short on average, so also test a
small-list linear scan with a threshold before binary search. Select from
measured end-to-end time, branch profile, and code size; do not assume the
nominally logarithmic version wins.

Likewise, test software prefetching of `HotClass` records two to four candidate
IDs ahead only as an isolated variant. Candidate lists are often too short and
recursion interrupts streaming, so retain prefetch only if alternating runs
show a repeatable improvement. Do not prefetch rich member/output data.

### 7. Keep instrumentation out of the kernel

Production statistics may be updated on cache admission, exhaustion, or
progress output, which occur infrequently. Do not increment hit, probe, fit,
letter-check, or branch counters on every node/candidate in the timed build.
Collect those with a compile-time measurement mode or sampling profiler. Verify
the release disassembly contains no disabled-statistics branches.

## Correctness invariants

These are implementation requirements, not merely test expectations:

1. **The key contains only the remaining bag.** It excludes `entry_point`,
   forced rank, path, accumulated score, and output state.
2. **Cache construction ignores `entry_point`.** A later path to the same bag
   may have a lower entry point.
3. **Every use applies `lower_bound(entry_point)`.** The permutation tie-break
   remains path-dependent.
4. **Indexes retain existing global class order.** Enumeration order and
   deterministic output tie-breaking do not change.
5. **Only fitting class IDs are cached.** Scores, paths, representatives, and
   spelling expansions remain uncached.
6. **Empty candidate lists are valid cached values.** They must be distinct from
   an unseen or bypassed entry.
7. **An entry becomes visible only after its full slice is stored.** Recursion
   must never observe a partially built entry.
8. **Every failure is fail-open for correctness.** Overflow or budget
   exhaustion selects the baseline scan; it never truncates a list.

## Source changes

### `source/dfs-search.h`

- Add an optional candidate-cache byte budget to `DfsAnagramSearch`, with the
  default supplied by the CLI rather than hidden in the search.
- Add mixed-radix multipliers, per-class deltas, current bag key, cache storage,
  bag rank mask, packed hot-class storage, and small cache setup/lookup/build
  helpers.
- Keep cache implementation details private. If the structures make the header
  noisy, put a non-virtual `DfsCandidateCache` helper in new
  `dfs-candidate-cache.{h,cpp}` files and retain it by value or `unique_ptr`.
- Expose only low-frequency post-run diagnostics useful for tests/benchmarking,
  such as cache mode, admitted entry count, and charged bytes. Do not add a
  cache-hit counter increment to every DFS node in the optimized build.

### `source/dfs-search.cpp`

- Build and validate the mixed-radix encoding.
- Build the aligned `HotClass` and packed-requirement arrays, including
  `class_delta`, once per search object/run.
- Initialize the bounded dense or sparse cache before `walk()`.
- Instantiate dense, sparse, and uncached recursive kernels so mode checks are
  outside the recursion.
- Update `bag_key` and the bag rank mask alongside packed bag subtract/restore.
- Replace progress modulo with a threshold and add only measured branch/layout
  hints.

### `source/dfs-anagrams.cpp`

- Add `--candidate-cache-mib` with a 64 MiB provisional default and `0` as the
  explicit baseline/off mode.
- Parse MiB with checked conversion to bytes and pass the byte budget into the
  search.
- Keep normal output unchanged. Cache diagnostics, if printed at all, belong on
  `stderr` in the phase-2 completion line.

### `source/meson.build`

- Add a helper source to `dfs_class_list_lib` only if the cache is split out of
  `dfs-search.cpp`.

### Tests

- Extend `source/test-dfs-search.cpp` to run the same fixture with caching
  disabled, dense caching enabled, sparse caching forced by a small metadata
  budget/test hook, and candidate storage exhausted. Compare the ordered
  emitted class-index paths and representative scores, not only a set of
  solutions.
- Include a regression in which the same bag is reached with different
  `entry_point` values. This catches both building from the first entry point
  and forgetting the per-use lower bound.
- Keep the existing 14-letter node and solution count validation.
- Extend the CLI differential test to compare cache-on and cache-off stdout
  byte-for-byte on the generated index.

## Implementation sequence

1. Capture a cache-off correctness checksum, timing, profile, and annotated
   assembly for the current kernel.
2. Add the aligned packed phase-2 class view, support/rank masks, progress
   threshold, and checked mixed-radix deltas. Keep the original scan and verify
   exact output after each representation change.
3. Implement the packed candidate arena and single-load dense cache. Validate
   the entry-point regression and exact ordered emissions before tuning.
4. Split dense, sparse, and uncached template kernels; add the aligned
   structure-of-arrays sparse table and every budget/overflow fallback.
5. A/B `std::lower_bound`, branchless lower-bound, and linear/binary hybrid.
   Separately A/B branch hints, helper inlining, and look-ahead prefetch. Make
   one change per run and keep only repeatable wins.
6. Add the CLI budget, cache-off differential test, low-frequency diagnostics,
   and null-sink `-n 0` path.
7. Run the full correctness/performance gates, tune sparse load factor, dense
   threshold, and default budget, inspect final assembly, and remove all
   measurement-only counters from the release kernel.

## Validation and performance gates

Build the existing Release/LTO/`x86-64-v2` configuration, then:

1. Run the normal Meson smoke suite.
2. With `$IDX` set, run `test-dfs-search --validate-14`.
3. Run the 14-letter top-10k workload with `--candidate-cache-mib 0` and with
   the default cache. Require identical stdout SHA-256 plus identical node,
   solution, expansion, and retention counts.
4. Warm the index page cache and alternate at least three baseline/cached runs
   of:

   ```
   dfs-anagrams -n 0 -p 100 \
     idx/wiki-merged.5.index firestationteamused

   dfs-anagrams -n 10000 -p 100 \
     idx/wiki-merged.5.index firestationteamused
   ```

   Record median wall time and `/usr/bin/time -v` peak RSS. Require identical
   phase-2 node and solution counts. The initial performance gate is at least a
   20x count-only speedup and no more than the configured cache budget plus
   small allocator/accounting overhead above baseline RSS.
5. Sweep cache budgets `0, 8, 16, 32, 64` MiB on the count-only workload. Select
   the smallest default that is within 5% of the fastest median and confirm
   every exhausted-budget run remains exact.
6. Profile the final count-only binary. Repeated letter-fit testing should no
   longer dominate; cache lookup, lower-bound traversal, key/bag restore, and
   recursion should account for the search core. Inspect cache misses, branch
   misses, instruction count, and cache/TLB counters on machines where PMU
   access works; use sampled annotated assembly under WSL. Require the dense hit
   path to compile to one metadata load, sentinel test, slice address
   calculation, suffix seek, and direct class-ID traversal, with no allocation,
   hash, mode switch, virtual call, modulo, or fit test.
7. If arena lookup is slower than the measured vector prototype, compare
   generated assembly and lookup-mode costs before changing the representation.

## Explicitly deferred

The exact `DfsTopN::emit()` cutoff and branch-and-bound are separate algorithmic
changes. They should be measured after this cache kernel lands. Progress
thresholding, support masks, and class metadata packing are included here
because they directly remove work and pointer chasing from cache construction,
fallback, and the remaining 181.6 million transitions.
