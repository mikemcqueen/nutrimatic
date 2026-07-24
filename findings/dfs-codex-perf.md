# Where `dfs-anagrams` spends its time

## Headline

`dfs-anagrams` has none of `find-anagrams`'s frontier problems. There is no
large heap, median scan, monotone-priority opportunity, or multi-gigabyte
randomly accessed allocation. On the representative 19-letter search,
**99.2% of count-only CPU time is in `DfsAnagramSearch::walk()`**, almost all of
it repeatedly testing whether candidate classes fit the remaining bag.

The repetition is extreme:

- 70,608,083 DFS nodes visit only **18,379 distinct remaining bags**.
- The baseline tests **38.92 billion candidates** and 54.67 billion
  `(symbol, required_count)` pairs.
- Only 181.61 million candidates pass: **99.53% of the tests reject**.
- One bag is revisited 1,486,215 times.

A measurement-only prototype cached the sorted list of fitting class indexes
for each remaining bag. It preserved `entry_point` by lower-bounding into that
list and did not cache scores, paths, or output. At `-p 100`:

| 19-letter workload | Baseline | Vector cache | Speedup |
|---|---:|---:|---:|
| `-n 0` (search core) | 80.40 s | 3.30 s | **24.4x** |
| `-n 10000` (end to end) | 81.33 s | 4.78 s | **17.0x** |
| peak RSS, `-n 0` | 183,680 KiB | 223,300 KiB | +39 MiB |

The node and solution counts remained exactly 70,608,083 and 29,783,883.
On the 14-letter workload, all 10,000 output rows were byte-identical between
baseline and prototype (the same SHA-256), as were the node, solution,
expansion, and retention counts.

This is the clear first optimization. The prototype was removed after
measurement; this document is the only intended source-tree change.

## Method and caveats

- Build: the repository's Release build, static libraries, LTO, and
  `-march=x86-64-v2`.
- Index: `idx/wiki-merged.5.index` (1.2 GB), warm page cache.
- Main workload:

  ```
  dfs-anagrams -n {0,10000} -p 100 \
    idx/wiki-merged.5.index firestationteamused
  ```

  This is the documented 19-letter bag. It produces 123,860 extracted entries,
  23,735 classes, 784,390 phase-1 trie nodes, 70,608,083 phase-2 nodes, and
  29,783,883 class solutions.
- Short checks: `computers` (9 letters) and `featstudiotsen` (14 letters).
- Machine: i7-13700H under WSL2, Linux 6.6.114.1, 15 GiB RAM.
- Sampling: `/usr/lib/linux-tools-6.8.0-136/perf`, `task-clock:u`, 499 Hz.
  WSL exposes no useful PMU counters here, so cache/TLB explanations remain
  inference. The profiles still locate the hot instructions reliably.
- Timing under WSL varies by a few percent. The headline comparison reran both
  variants with the requested `-p 100`. That prints every 10 million nodes.
  Earlier 25-second sampling runs used a larger `-p` only to keep sample output
  quiet; they are not the headline timings.

The 9- and 14-letter top-10k workloads take only 0.02 s and 0.08 s
respectively. They are useful correctness checks but too short to profile.

## Baseline profile

### Count-only (`-n 0`)

| Symbol | CPU share |
|---|---:|
| `DfsAnagramSearch::walk()` | **99.18%** |
| `DfsExtractor::walk()` (both clones combined) | 0.38% |
| `IndexReader::children()` | 0.14% |
| `DfsTopN::emit()` | 0.10% |

`-n 0` still calls the sink for every solution, but `DfsTopN::emit()` returns
immediately. That virtual/call overhead is not important.

Within `walk()`, the annotated assembly is unusually decisive:

| Instruction/operation | Share of all samples |
|---|---:|
| load `candidate.letters[i].second` | **32.83%** |
| advance to the next rejected class | **13.80%** |
| loop/control around required-letter checks | substantial remainder |
| per-node progress modulo | 0.25% |
| score arithmetic, recursion call, bag subtract/restore | individually tiny |

The hot load follows a pointer from the 88-byte-ish `DfsAnagramClass` into a
separate allocation for its `letters` vector. The profile cannot prove cache
misses, but the scattered data layout and the volume below explain why that
single load dominates.

### Top 10,000 (`-n 10000`)

| Symbol/area | CPU share in a 25 s sample |
|---|---:|
| `DfsAnagramSearch::walk()` | **96.89%** |
| `malloc` | 1.02% |
| `DfsTopN::emit()` itself | 0.29% |
| expansion heap push | 0.24% |
| extractor and index reader | about 0.7% |

The output stage matters only after the fit loop is fixed. Baseline end-to-end
time is just 0.93 s above count-only time, but with the vector cache the gap is
1.48 s and output becomes about 31% of wall time.

## What the fit loop is doing

Temporary counters gave:

| DFS depth | Nodes | Candidate tests | Letter/count checks | Fits |
|---:|---:|---:|---:|---:|
| 0 | 1 | 8,589 | 61,385 | 8,589 |
| 1 | 8,589 | 43,812,955 | 108,886,838 | 2,858,347 |
| 2 | 2,854,321 | 5,181,620,613 | 10,026,111,212 | 69,832,791 |
| 3 | 67,745,172 | 33,695,496,732 | 44,539,923,233 | 108,911,925 |
| **Total** | **70,608,083** | **38,920,938,889** | **54,674,982,668** | **181,611,652** |

Of the 38.74 billion rejections:

- 30.89 billion (79.75%) first fail because a required symbol is absent;
- 7.85 billion (20.25%) fail because the symbol is present but its
  multiplicity is too low.

A rejection takes only 1.40 required-letter checks on average, so reordering
the pairs within a class cannot buy much. The problem is reaching and testing
the same classes again, not doing a long comparison once.

The source of the repetition is structural. A remaining bag determines:

- the forced rarest symbol;
- the candidate bucket;
- whether every candidate fits.

It does **not** determine `entry_point`, the path, or the accumulated score.
Many different class paths reach the same bag, hence 70.6 million node visits
but only 18,379 bags. The theoretical mixed-radix state space for this input is
only 55,296 bags.

## Measured candidate-cache prototype

The prototype assigned each bag a mixed-radix integer key. Subtracting a class
updated the key incrementally using one precomputed multiplier per symbol.
Each cache entry held all fitting class indexes from the forced-symbol bucket,
in the bucket's existing order.

At a node:

1. Look up the remaining bag.
2. On the first visit, scan the whole forced-symbol bucket and cache its fitting
   class indexes.
3. Use `lower_bound(entry_point)` on the cached indexes.
4. Traverse exactly the same suffix, in exactly the same order, as baseline.

The cache changed the work from:

| Work | Baseline | Cached |
|---|---:|---:|
| repeated candidate fit tests | 38,920,938,889 | 0 |
| one-time cache-build scans | 0 | 70,265,295 |
| known-fitting candidates traversed | 181,611,652 | 181,611,652 |

That is a 154.5x reduction in candidate examinations. The remaining
181.6 million candidates are real search transitions or fitting candidates
that the depth/remaining-length rules subsequently reject; caching cannot
remove them without changing the search algorithm.

### Correctness details that matter

- Cache from `candidate_begin(forced_symbol)`, **not** from the first call's
  `max(candidate_begin, entry_point)`. Caching the latter would silently omit
  valid lower-index classes on another path to the same bag.
- Keep indexes sorted and apply `lower_bound(entry_point)` at every use. The
  entry-point tie-break is path-dependent even though fitting is not.
- The forced rank is derived from the bag and need not be in the key.
- Do not cache representative scores or output. Those are path-dependent.
- Preserve existing class-index order so enumeration and deterministic
  tie-breaking remain unchanged.
- Guard mixed-radix multiplication against overflow.

### Memory representation

The fast prototype used `vector<size_t>` per visited bag. It costs about 39 MiB
on this workload. `uint32_t` class indexes would likely cut a material part of
that without changing lookup behavior, provided class count is checked to fit.

A second measured prototype stored a compatibility bitset over each bag's
forced-symbol bucket:

| Cache representation, `-n 0` | Time | Peak RSS | vs baseline |
|---|---:|---:|---:|
| sorted index vectors | **3.30 s** | 223,300 KiB | 24.4x, +39 MiB |
| per-bucket bitsets | 4.06 s | **194,024 KiB** | 19.8x, +10 MiB |

The bitset permits an O(1)-ish seek at `entry_point` and saves memory, but
scanning/decoding set bits was slower overall. It is a useful low-memory mode,
not the default performance choice.

Do not allocate one vector header for every theoretical mixed-radix state
without a limit. This bag has 55,296 states, but 26 distinct letters already
have 67,108,864 sub-bags, and 36 distinct symbols have 2^36. A production
version should use one of:

- dense entries only when the state product is under a configured memory
  threshold, otherwise a sparse map keyed by the packed bag;
- a sparse map for all inputs, with reserved capacity;
- a bounded cache with eviction or fallback to the current scan.

The cache's stored candidate vectors also need a byte budget. Exhausting the
budget should only reduce performance, never change results.

## Applicability of `findings/perf-analysis.md`

| Earlier finding | Applies to DFS? | Reason |
|---|---|---|
| OpenFST is absent | Yes | `dfs-anagrams` also does not link OpenFST. |
| Remove/cheaply compute queue median | No | DFS has no frontier or median diagnostic. |
| Binary heap to monotone bucket queue | No | Phase 2 is recursive and has no search queue. The bounded top-N min-heap is not hot. |
| Static linking + LTO | Yes, already enabled | It allows cross-library inlining here too. The DFS profile does not isolate its gain, but reverting it would be perverse. |
| `-march=x86-64-v2` | Already enabled, less specifically relevant | The DFS hot path has no `__popcountdi2`; the original measured POPCNT win does not transfer directly. |
| Predeclare allowed characters | Yes, already present in phase 1 | `DfsExtractor` constructs a `CharSet` from the bag and `children()` skips disallowed children before decoding. |
| Optimize `IndexReader::children()` | Not for the long workload | Phase 1 plus `children()` is below 1% while the 80-second phase 2 runs. It becomes a few percent only after caching. |
| Huge pages for the frontier | No | Baseline RSS is about 180 MiB, not a multi-gigabyte randomly accessed frontier. |
| Chunk bucket vectors / avoid `memmove` | No | Those frontier buckets do not exist here. |
| Faster `log2f` | No | Phase 2 uses precomputed `double` logs; log functions are absent from the hot profile. |
| Goal-directed priority | Not directly | DFS is exhaustive rather than best-first. A safe top-N branch bound is the corresponding algorithmic idea. |

In short: the build flags and phase-1 character mask transfer, and they are
already in the tree. The large measured wins from the old document target data
structures DFS deliberately eliminated.

## What to do after candidate caching

These were originally ordered by likely value. Items 1--5 and the candidate
cache have since landed; item 6 is now implemented as the memoized score bound
described below.

1. **Add a cheap top-N early exit in `DfsTopN::emit()`.**

   `representative_log_score` is the score using member zero from every class,
   hence the maximum score of any spelling expansion for that class path. Once
   the global heap is full:

   ```
   if (representative_log_score <= floor_log_score()) return;
   ```

   can run before allocating `first.member_indexes` and the local priority
   queue. The current code performs those allocations and then makes the same
   comparison on `pending.top()`. This is exact, not heuristic, and attacks the
   1.48-second output-stage gap exposed by the cache.

2. **Precompute one mixed-radix delta per class.**

   The prototype updated the cache key once per distinct required symbol, doing
   an integer multiply on subtract and again on restore. In the cached profile,
   bag/key subtract and restore become visible hotspots. The input-specific
   radix multipliers are fixed for the run, so compute
   `class_delta = sum(count * multiplier[symbol])` once per class and update the
   key with one subtraction/addition.

3. **Replace progress modulo with a threshold.**

   Baseline hides the integer division at 0.25%, but after caching the
   `nodes % progress_interval` division is about 3.7% of `walk()` samples.
   Keep `next_progress`, compare `nodes == next_progress`, then add the
   interval after printing. This preserves `-p 100` behavior.

4. **Add a support mask to each class.**

   About 80% of baseline failures are caused by a completely absent symbol.
   Maintain a 36-bit nonzero-symbol mask for the bag and reject with
   `(class_mask & ~bag_mask) != 0` before checking multiplicities. Better still,
   let the mask account for all count-one requirements and keep a compact list
   only for symbols whose class count exceeds one.

   This will not approach the cache's win—the cache removes repeated tests
   altogether—but it speeds first-time cache construction and provides a good
   fallback when caching is capped.

5. **Pack phase-2 search metadata separately from rich class data.**

   The hot scan currently strides through `DfsAnagramClass` and follows a
   separately allocated `letters` vector. A structure-of-arrays search view
   could hold support masks, packed repeated-count requirements, key length,
   best-member log score, and perhaps a 32-bit class ID contiguously. Keep
   strings and member vectors cold for phase 3.

6. **Memoize an exact relaxed completion-score bound. — Implemented**

   Caching removes repeated fit tests but deliberately leaves the exhaustive
   path tree intact. The next large reduction comes from collapsing the scoring
   question onto the same small remaining-bag state graph used by the candidate
   cache.

   For a remaining bag `B`, define:

   ```
   H(empty) = 0

   H(B) = max over classes c fitting B:
              best_member_log_score[c]
            + restart_log_rate
            + H(B - c)
   ```

   `H(B)` is memoized by the existing collision-free mixed-radix bag key. It
   intentionally ignores `entry_point`: the unrestricted recurrence may admit
   suffixes that canonical DFS would reject, so it can only raise the result.
   At every non-root DFS node, once the top-N heap is full:

   ```
   if (representative_log_score + H(remaining_bag) <= heap_floor)
       return;
   ```

   This is exact pruning, not a heuristic. Each class contributes its
   maximum-member score, phrases remain one class and therefore pay no internal
   restart, and every class after the already-chosen prefix pays exactly one
   restart. Every spelling below the node is bounded by the representative
   class path. Heap dedup does not weaken the proof: a spelling no better than
   the current floor cannot enter the retained top N whether its word-set key is
   new, retained, or was previously evicted.

   The root is special. `H` charges a restart to every class it adds, while the
   first class in a complete search does not pay one. The implementation uses
   `H` to prove a root dead but does not compare the root score against the heap
   floor.

   ### Measured effect

   The production implementation was compared with the post-cache binary on the
   same machine and index as the headline measurements. Output was
   byte-identical in every comparison:

   | Top-10k workload | Cached exhaustive DFS | Memoized bound | Node reduction | Wall-time speedup |
   |---|---:|---:|---:|---:|
   | 19 letters, `firestationteamused` | 70,608,083 nodes, 2.08 s | 802,917 nodes, 0.44--0.45 s | **87.9x** | **4.6--4.7x** |
   | 20 letters, `firestationteamusedb` | 957,821,082 nodes, 15.62 s | 1,409,052 nodes, 1.19 s median | **679.8x** | **13.1x** |

   The 19-letter top-1 and top-100 prototypes were about 5.0x and 4.6x faster
   respectively. The 9- and 14-letter results also matched byte for byte. The
   bound changes the number of class solutions *visited* in a top-N run because
   losing subtrees are no longer enumerated; `-n 0` remains exhaustive and
   retains its exact 70,608,083-node / 29,783,883-solution statistics.

   ### Why the earlier per-letter `h` was rejected

   The bound proposed in `plans/dfs.md`,
   `sum(best_rate_containing[letter])`, is admissible but far too relaxed for
   this class list. A faithful prototype pruned **zero nodes** on the 19-letter
   workload and made the run slightly slower. It lets every remaining letter
   independently choose a different ideal class, even when those choices cannot
   coexist. The bag DP prices complete compatible class partitions instead.

   ### Bounded storage and cache integration

   Score memoization shares `--candidate-cache-mib`'s existing hard budget. Up
   to one quarter is available to score bounds and the rest remains candidate
   metadata and IDs.

   - If one `double` per theoretical mixed-radix state fits, use an aligned dense
     array. This is the path for the documented 55,296-state workload.
   - Otherwise use a fixed, open-addressed sparse table with separate key and
     value arrays and a 50% maximum load factor.
   - If the sparse table fills, discard it, restore the candidate cache's full
     budget, and run without score pruning. Exhaustion changes performance only.
   - Build `H` only for a score-aware sink. Count-only and ordinary solution
     sinks neither allocate nor compute it.

   Bound construction obtains fitting classes through the candidate cache and
   populates cache misses as it goes. The subsequent DFS therefore does not
   repeat the bound pass's bucket scans. A memoized `-infinity` means the bag has
   no completion and is an unconditional dead-state prune, even before the heap
   fills.

   The recurrence is evaluated in `long double` and rounded upward into the
   stored `double`, with extra ulps for the production score's two additions.
   The final comparison also includes a depth-scaled floating-point margin. A
   rounding discrepancy can therefore suppress a prune, never invent one.

   ### Candidate ordering was measured and rejected

   Sorting the first two DFS levels by
   `class_score + H(child_bag)` reduced the 19-letter node count slightly
   further, from about 803k to 694k, but it found a worse spelling-heap floor:
   spelling expansions rose from 23,604 to 72,429 and wall time rose from about
   0.44 s to 0.62 s. Representative class-path order is not the same as the best
   order after spelling expansion and word-set dedup. The implementation keeps
   the existing class-index order.

## Recommended implementation order

1. Implement the sorted-index candidate cache with an explicit memory budget
   and uncached fallback.
2. Use 32-bit cached class IDs when the class count permits.
3. Add the exact `DfsTopN::emit()` pre-allocation cutoff.
4. Precompute per-class bag-key deltas and replace progress modulo.
5. Re-profile before changing class layout or adding masks.
6. Add the memoized remaining-bag score bound; retain class-index traversal
   order.

The first item is large enough that optimizing the current fit loop in place
before caching would target work that should almost entirely disappear.
