# Practicality of a multi-threaded search implementation

Naming the priority queue actually inverts where the difficulty lies. A
threadsafe PQ is *necessary* for the naive design and simultaneously the thing
that will *prevent* it from scaling.

## First: what kind of workload is this really?

This matters more than any data-structure choice, because it sets the ceiling.

The inner loop of `step()` is: pop one `Next`, call `reader->children()`
(pointer-chasing into the mmap'd trie), run a few `has_transition()` array
lookups, push a handful of entries back. Two consequences:

1. **It's memory-latency bound, not compute bound.** The dominant cost is
   cache/TLB misses walking a large mmap'd trie, not arithmetic. That cuts both
   ways: latency-bound workloads *do* benefit from parallelism via latency hiding
   (while one thread stalls on a miss, another runs), but they're capped by
   **memory bandwidth and shared page cache**, not core count. You will not get
   N× on M cores — 2–4× on a big machine is a realistic optimistic ceiling.

2. **The per-node work is tiny relative to any synchronization.** That's the
   Amdahl killer for a shared PQ (below).

And the crucial usage fact: the real workload is **early termination** — the
consumer takes the top ~100 and quits. That makes any thread beyond the first do
*speculative* work on frontier nodes a sequential search would never have
reached. This is the classic parallel branch-and-bound efficiency problem: extra
threads explore subtrees whose results get thrown away when the consumer says
"enough." So even a "successful" parallel search has poor work-efficiency for the
dominant use case.

## Why the naive "threadsafe PQ" design underdelivers

Say N workers each pop from one concurrent PQ, expand, push back. Problems:

- **The top of a strict priority queue is a single global contention hotspot** —
  by definition every thread wants the same element. Strict concurrent PQs
  (heap+lock, or fine-grained skiplist) are exactly the structure known *not* to
  scale; the literature invented *relaxed* PQs (MultiQueue, SprayList)
  specifically because of this. With per-node work this small, threads spend most
  of their time contending on the PQ rather than searching.
- **The PQ isn't the only shared mutable state.** You'd also have to make
  threadsafe:
  - `seen` (std::set) — touched on every accepting node; another global hotspot.
    Needs a concurrent/sharded hash set with atomic insert-if-absent.
  - `crumbs` (append-only deque, indexed by int) — reads are fine (deque doesn't
    invalidate), but the "did I already push a crumb this expansion" sharing
    optimization (`search-driver.cpp:47`) is inherently single-threaded. You'd
    give each thread its own arena with a global addressing scheme, or atomically
    append.
- **You lose the invariants anyway.** Strict best-first gives you two things the
  code quietly relies on: results emerge in decreasing score order, and the
  *first* time a string is popped it's at its maximum score (so `seen` dedup
  keeps the best score). Under N concurrent pops, both become approximate — two
  threads can reach the same string via different paths "simultaneously," and the
  winner of the atomic insert isn't necessarily the higher-scoring one. For a
  heuristic "more common first" ranking this is *tolerable*, but note you're
  sacrificing the ordering guarantee regardless of how careful the PQ is. So you
  pay for a strict concurrent PQ and don't even get strict results.

Net: the threadsafe PQ is the *start* of the problem, not the solution.

## The two architectures actually worth considering

### (A) Coarse-grained partition + k-way merge — recommended

Don't parallelize one search; run several independent searches and merge their
sorted output streams. Partition by **first character**: one `SearchDriver` per
starting letter/digit (~30 shards), each in its own thread, each with its own
private `crumbs`/`seen`. Then merge their outputs with a `priority_queue` of
stream heads.

Why this fits *this* codebase unusually well:

- **The k-way best-first merge pattern already exists** — `merge-indexes.cpp`
  does exactly this with a `priority_queue<IndexWalker*>`. You'd be reusing a
  proven pattern.
- **No shared hot structures.** Each shard's PQ, seen, and crumbs are fully
  private. `IndexReader` is read-only const mmap access — already safe for
  concurrent reads, sharing one page cache.
- **Global ordering is restored for free** by the merge (each shard emits
  score-descending; scores are comparable across shards since every shard uses
  the same `reader->count()` and `restart`).
- **Dedup stays clean:** a result's first character fixes its shard, and
  `restart` continues an existing path without changing the first character — so
  there are *no cross-shard duplicates*. Per-shard `seen` is sufficient.
- **Early termination is handled with backpressure:** bound each shard's
  read-ahead to a small buffer so speculative work stays limited when the
  consumer takes top-100.

Cost: load imbalance (`t`, `s`, `a` shards are far heavier than `x`, `z`). Fix
with a work-stealing thread pool over more, finer shards (partition by first
*two* chars), rather than one-thread-per-letter. This is a few hundred lines and
no new concurrency primitives.

### (B) Relaxed concurrent PQ (MultiQueue)

The "proper" parallel best-first: each thread keeps a local PQ, with randomized
push/pop across a pool of queues for approximate global ordering, plus a
concurrent dedup set. This is the textbook scalable answer for parallel
Dijkstra/A*. It's more general than (A) and load-balances naturally, but it's
substantially more engineering, still bandwidth-capped, and still relaxes
ordering — for uncertain benefit here. Only reach for it if (A)'s partitioning
proves too imbalanced.

## The bottleneck might not even be here

Before writing any of this: **profile which stage is slow.** The search
traversal is capped by the CGI at 1M node-expansions / 30s, and a plain
regex-ish query usually blows through that in well under a second. The queries
that feel slow are the **anagram and `&`-intersection** ones — and their cost is
largely in single-threaded **OpenFST `Determinize`/`Minimize`/`Intersect`**
during compilation (`expr-optimize.cpp`, `expr-intersect.cpp`,
`expr-anagram.cpp`), which can blow up combinatorially *before the search even
starts*. Parallelizing the search would do nothing for those. If those are your
slow cases, the leverage is in the FST construction (or bounding it), a different
and harder target.

## Bottom line

- Threading is plausible but the honest ceiling is bandwidth-limited (single-digit
  ×), and the early-termination usage erodes work-efficiency further.
- A strict threadsafe PQ is an anti-pattern here — it's the contention hotspot,
  forces `seen`/`crumbs` to also go concurrent, and you lose strict ordering
  anyway.
- If you do it, use **(A) partition-by-first-char + k-way merge**, which reuses
  the existing `merge-indexes` structure and avoids shared hot state entirely.
  Save the relaxed concurrent PQ for later if imbalance demands it.
- **Profile first** — for the queries users actually find slow, the cost may be
  FST compilation, not the search loop.
