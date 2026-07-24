# Plan: multi-threaded `dfs-anagrams` bound preprocessing

## Outcome

Parallelize the expensive dense score-bound prepass between the phase-1
completion message and the phase-2 DFS. The implementation should make 30+
letter inputs usefully faster on multicore machines without changing search
results, scores, bound values, cache budgets, or single-thread behavior.

Keep the optimization deliberately narrow:

- thread only eager `SCORE_BOUND_DENSE` construction;
- retain the existing recursive, top-down reachable-state traversal;
- retain one shared bound and support-mask cache rather than duplicating either
  per worker;
- do not populate the full per-bag candidate cache from parallel workers;
- leave sparse construction, uncached construction, phase 1, and the final DFS
  single-threaded;
- use one thread for short CLI inputs unless the user explicitly requests more.

The dense restriction targets the case that needs this most and has the
strongest synchronization properties. A dense mixed-radix key gives every bag
one permanent slot, the table cannot exhaust, and the entire state graph
remains available to the later DFS. Sparse insertion, sparse exhaustion, and
fail-open cache rebuilding do not need concurrent variants.

## Evidence and practicality

The exact reference workload is:

```text
idx/wiki-merged.5.index
aaaabdeeeeghhiilmnnrrrstttwwww
-m 4 -n 1 --candidate-cache-mib 64
```

On the 20-logical-CPU development machine, the current single-thread baseline
is:

```text
# phase 1 complete: 459162 entries, 166852 classes, 3054340 trie nodes
# phase 2: precomputed 1406323 bounded states in 89.719s
# phase 2 timing: 89.719080 s setup, 0.004184 s search,
  1501837804 successful bound transitions, 3502860 nextafter calls
wall=91.35 user=90.53 sys=0.18 rss=385584 KiB
```

This makes phase-2 setup 99.99% of phase-2 time. Its 1.50 billion transitions
provide enough work to amortize thread creation and synchronization. Phase 1
and the final DFS are not worthwhile targets for this change.

The recursive bound is a directed acyclic graph:

```text
H(empty) = 0

H(B) = max over fitting classes c:
           score(c) + restart + H(B - c)
```

Every edge removes at least one letter, so dependencies always move to a
strictly smaller bag. Root candidates are independent except for their shared
descendants. A shared, claim-once memo lets workers exploit the independent
work without recomputing those descendants.

Expected scaling is useful but sublinear. The transition loop streams shared
class and support-candidate data, workers briefly contend when initially
constructing support lists, and multiple root branches converge on common
child states. The benchmark should therefore sweep thread counts rather than
assuming all logical CPUs are best. A 2x improvement is enough to retain the
feature; the design should not promise core-count scaling.

## Interface and policy

Add:

```text
--preprocess-threads N
```

with these CLI semantics:

- `0` (the default) selects an automatic count;
- automatic mode uses one thread below 30 remaining letters;
- for 30+ letters, automatic mode uses up to 20 threads, capped by
  `std::thread::hardware_concurrency()`; 20 was the best measured count on the
  development machine;
- `1` explicitly disables preprocessing parallelism;
- `N > 1` requests up to `N` workers even on a shorter input;
- the implementation caps the count at the number of root tasks and falls
  back to one thread unless a dense score table was selected.

Pass the resolved count into `DfsAnagramSearch`. Keep its constructor default at
one so library callers and existing tests remain single-threaded unless they
opt in. Record and expose the number actually used, and print a separate
diagnostic when it is greater than one without changing the established phase
timing line.

Use Meson's platform thread dependency rather than relying on an implicit
`pthread` link.

## Design

### 1. Publish dense bounds through their existing eight-byte slots

Represent each dense bound slot as an atomic 64-bit bit pattern. Reserve two
quiet-NaN payloads:

```text
UNSEEN
COMPUTING
```

All real stored values, including infinities, use their ordinary `double` bit
patterns. Convert between `double` and bits with `memcpy`, which is valid in
C++17 and does not depend on aliasing.

This keeps the dense bound table at exactly eight bytes per theoretical state;
no extra per-state status array is needed and the existing cache charge is
unchanged. Initialize every dense slot to `UNSEEN`.

The parallel lookup protocol is:

1. Acquire-load the slot.
2. Return a ready value immediately.
3. CAS `UNSEEN` to `COMPUTING` to claim an uncomputed state.
4. If another worker owns `COMPUTING`, yield and acquire-load until it becomes
   ready.
5. The owner computes the state and release-stores its final bound.

The wait cannot form a dependency cycle: a worker can wait only for a bag with
fewer letters than its current bag. There is a finite descending chain ending
at the empty bag. Do not use one mutex or condition variable per state; either
would exceed the table's current memory cost and initialization budget.

Use the same storage wrapper in the serial dense and sparse paths, but retain
their current algorithms. Relaxed loads/stores are sufficient there.

Only the claiming worker increments state and rounding counters. Keep these
counters local to each worker during preprocessing and sum them after joining,
which avoids atomics in the transition loop.

### 2. Give each worker a private traversal state

Move the mutable preprocessing traversal fields into a small worker context:

```text
bag[DFS_SYMBOL_COUNT]
bag_mask
bag_key
letters_left
states_computed
successful_transitions
nextafter_calls
```

The immutable packed class arrays, multipliers, restart score, and class-list
buckets remain shared. Refactor the fitting, candidate-building, subtraction,
restoration, and recursive bound helpers to take a context explicitly. The
ordinary phase-2 DFS may continue using the search object's current traversal
state.

Each worker starts from a copy of the full input bag. It subtracts one root
candidate, recursively computes that child, restores the bag, and then claims
another root candidate. This gives each thread stable private mutation while
all large read-only data stays shared.

### 3. Dynamically distribute root candidates

Build the full-bag fitting-candidate list into a temporary vector before
launching workers. It is the same ordered list that serial bound construction
would traverse. This list is small relative to the existing caches, lives only
for the duration of setup, and avoids making root work distribution depend on
candidate-cache admission.

Use one atomic next-index over this stable root list. Launch at most
`requested_threads - 1` background threads and have the caller participate as
the final worker. Dynamic one-candidate tasks handle the large variation in
subtree cost better than fixed contiguous slices.

Each worker retains local maxima for:

```text
max(candidate_bound)
max(candidate_rounding_error)
```

After joining, reduce those local maxima in worker-index order and run the
existing one-per-state conservative upward rounding for the root. This is
mathematically identical to the serial root calculation: both operations are
max reductions, not floating-point sums.

If background-thread construction throws, finish with the threads that were
successfully created plus the caller, join them, and report the actual count.
No exception may escape while a joinable thread exists.

### 4. Share only the support-mask cache during parallel construction

The 30-letter score table is dense, but its full per-bag candidate cache is
sparse. The initial draft proposed atomic dense candidate metadata plus one
mutex around candidate construction. That does not apply to the target
configuration. Extending it directly to the sparse cache would also serialize
many of the 1.4 million per-state misses and make sparse slot and arena
publication substantially more complicated.

The full candidate list has no reuse *within* eager bound construction: the
bound memo computes each bag once, and a repeated visit returns its bound
before consulting candidates. Its setup-time value is only that phase 2 later
inherits lists that happened to be built during the prepass. On the reference
30-letter top-1 run, that later DFS is 0.004 seconds.

Parallel bound workers should therefore skip the full candidate cache. For each
owned bag, they obtain the support-only list for its presence mask, skip the
too-long prefix, test repeated-letter multiplicities, and immediately evaluate
the fitting classes in existing order. If support caching is unavailable, they
scan the ordinary forced-symbol bucket. The full candidate cache remains
allocated under the existing budget and empty; the single-thread final DFS
populates it lazily for bags it actually visits.

The support cache *does* provide substantial reuse because many multiplicity
states have the same presence mask: the 30-letter run computed 1,406,323 bag
states but admitted only 3,213 support masks. Make support keys atomic 64-bit
values while leaving their metadata and append-only ID arena ordinary:

1. Probe keys with acquire loads.
2. On a hit, the acquire pairs with publication and makes the metadata and
   complete arena slice visible without locking.
3. On an empty slot, lock one support-build mutex and repeat the lookup.
4. If it is still absent and admission is available, build the support slice,
   write its metadata, and release-store its key last.
5. Unlock before filtering multiplicities or recursing.

All support arena offsets, fill counters, and charged-byte statistics remain
under that mutex. When metadata or arena admission is exhausted, set an atomic
fast-fallback flag so later new masks use the forced-symbol scan without
repeatedly taking the mutex. Already-published support hits remain usable.

The final DFS is single-threaded but can keep using the atomic support-key
wrapper with relaxed/acquire loads. The wrapper must remain eight bytes so the
existing cache charge is exact.

This split avoids synchronization in the 1.50-billion-transition hot loop,
locks only the few thousand cold support builds, and leaves the more
complicated sparse full-candidate table entirely single-threaded. The tradeoff
is that the final DFS starts with no full per-bag entries; benchmark both setup
and search time, especially for top 10,000, before retaining the design.

### 5. Keep score correctness bit-stable

For each owned non-root state:

- traverse candidates in the existing global class order;
- compute every candidate with the existing two `double` additions;
- retain the same per-state maximum computed value and maximum error;
- call the existing `round_score_bound_up()` exactly once;
- release-publish only the final value.

A state is computed once, so scheduling cannot choose between competing
results. A thread waiting for a child sees the child's fully rounded value.
The parallel root reduction uses max and therefore produces the same bound bit
pattern as the serial traversal. The transition, state, and `nextafter` totals
should remain identical.

The final DFS stays single-threaded, so result discovery order, top-N heap
updates, progress output, node counts, solution counts, and stdout remain
unchanged.

### 6. Preserve fallbacks

Use the existing serial prepass when any of these is true:

- the requested count is one;
- score pruning is unavailable;
- the score table is sparse or disabled;
- the hot class representation is unavailable;
- there are fewer than two fitting root candidates.

The current sparse-table exhaustion path must remain untouched: abort, discard
the incomplete score table, return its memory to the candidate cache, and run
phase 2 without score pruning.

Thread creation failure degrades the worker count; it must not disable a bound
table that other workers can finish. Support-cache allocation or admission
failure retains the current forced-bucket scan fallback. The full candidate
cache retains its existing allocation and admission fallbacks in the final
DFS.

## Implementation sequence

1. Add the CLI option, constructor argument, accessor, resolved automatic
   policy, and thread link dependency.
2. Introduce the atomic 64-bit bound wrapper and retain serial test behavior.
3. Add the private bound-worker context and refactor preprocessing helpers to
   consume it without changing the serial algorithm.
4. Make support-key publication atomic and add the miss-only support-build
   mutex and exhaustion fast path.
5. Add the parallel support-filter traversal, dense bound claiming/waiting,
   and dynamic root work distribution.
6. Aggregate counters, expose the actual thread count, and add the diagnostic.
7. Add focused concurrency tests before running large benchmarks.

## Validation

### Correctness and fallback tests

- Run the existing `dfs-search`, `dfs-search-14`, `dfs-cli`, and
  `dfs-cli-differential` tests.
- Extend the small dense-bound test to run with four requested preprocessing
  threads and compare retained spellings with the exhaustive and serial-bound
  runs.
- Assert that the threaded dense run has the same:
  - bound entries and computed states;
  - successful bound transitions;
  - `nextafter` calls;
  - score-bound mode;
  - DFS nodes and solutions;
  - sorted spelling texts and scores.
- Exercise `--preprocess-threads 1`, `0`, and an invalid value in the CLI smoke
  test.
- Retain the sparse-bound and sparse-exhaustion assertions, including their
  single-thread behavior.
- Run ThreadSanitizer on the focused dense-bound test if the installed compiler
  and dependencies support it.

### Performance tests

Build the optimized configuration and run interleaved warm measurements for:

```text
aaaabdeeeeghhiilmnnrrrstttwwww -m 4 -n 1
```

Sweep `--preprocess-threads 1,2,4,8,10,20`. Capture:

- setup, search, and wall time;
- user and system time;
- peak RSS;
- states, transitions, and `nextafter` calls;
- support, candidate, and bound cache entries/bytes;
- stdout hash.

Choose the automatic cap from the median setup time, preferring the smaller
count when results are within 5%. Retain the feature only if at least one
multithreaded count improves setup by 2x without changing stdout or counters.

Also measure a 25-letter and 28-letter reference bag with automatic mode to
confirm the below-30 default remains one thread, then explicitly request the
chosen count once to quantify whether the threshold should move in a later
change.

## Review result

The separate pre-implementation review found and corrected one material design
error in the first draft: the target has a dense score table but a *sparse*
full candidate cache. The original proposal covered only dense candidate
metadata and would have either missed the target or required a heavily
contended concurrent sparse cache. The reviewed design now shares only the
support-mask cache during parallel construction and leaves the full candidate
cache to the serial DFS, as described in §4.

The remaining checklist passes with these conclusions:

- **Bound correctness:** one owner publishes each state after applying the
  unchanged conservative rounding. Root aggregation uses maxima only, so
  scheduling cannot change a score bit.
- **Wait safety:** dependency waits strictly decrease remaining letter count.
  A worker's owned stack contains only ancestors of the state it is waiting
  for, so a wait cycle cannot form. Yielding may still reduce scaling and is a
  benchmark concern, not a correctness concern.
- **Publication:** a release store of a bound or support key occurs only after
  its ordinary payload is complete; readers use acquire loads. Fixed arenas do
  not move.
- **Races:** bags and hot counters are worker-local; bound slots and support
  keys are atomic; support allocation and statistics are under one mutex; the
  full candidate cache and final DFS remain serial.
- **Failures:** the caller participates, all successfully created threads are
  joined, support failures scan directly, and non-dense score modes keep the
  existing serial/fail-open paths.
- **Memory:** atomic wrappers must be asserted to remain eight bytes. No
  persistent table is added, and temporary worker/root storage is outside but
  negligible relative to the 64 MiB cache.
- **Practicality:** the 89.719-second, 1.50-billion-transition baseline
  justifies implementation, but retention still depends on the 2x benchmark
  gate and on acceptable top-10,000 final-DFS performance without prewarmed
  full candidate entries.

## Implementation and benchmark outcome

The reviewed design was implemented. A short spin with an x86 pause instruction
and an occasional scheduler yield replaced yield-on-every-poll after the first
benchmark exposed excessive system time.

One warm run per count on the 30-letter top-1 workload measured:

| Threads | Setup | Speedup vs. 89.719 s baseline |
|---:|---:|---:|
| 1 | 89.719 s | 1.00x |
| 2 | 45.851 s | 1.96x |
| 4 | 25.161 s | 3.57x |
| 8 | 18.239 s | 4.92x |
| 10 | 14.960 s | 6.00x |
| 16 | 12.729 s | 7.05x |
| 20, yield every poll | 11.065 s | 8.11x |
| 20, reviewed backoff | 10.139 s | 8.85x |

The final 20-thread run used 198.18 user seconds and 1.61 system seconds, down
from 47.65 system seconds before the wait-backoff correction. Peak RSS was
358,920 KiB versus 385,584 KiB in the original serial run because the parallel
prepass does not touch most full-candidate arena pages.

Every run retained exactly:

```text
1406323 bounded states
1501837804 successful bound transitions
3502860 nextafter calls
62432 phase-2 nodes
13 phase-2 solutions
```

and byte-identical top-1 stdout.

The 30-letter top-10,000 run took 10.437 seconds of setup and 2.173 seconds of
search. Its total remains far below the old setup alone, so lazy full-candidate
warming passes the review gate.

Automatic mode remained serial for the reference 25- and 28-letter inputs:
1.982 and 21.413 seconds of setup. An explicit 20-thread request reduced the
28-letter setup to 2.241 seconds with identical counters and output; this is
useful evidence for a future threshold policy change, but the present change
keeps the reviewed conservative 30-letter automatic threshold.
