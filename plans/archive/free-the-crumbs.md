# Plan: reclaim dead path history from `SearchDriver::crumbs`

## Goal

Stop `crumbs` growing monotonically for the life of a search. It gains one
8-byte entry per productive step and is never released
(`source/search-driver.cpp:82-97`) — `findings/anagram-perf.md` #6 measured
~290 MB at 36M steps. Secondary goal, but the one that can't be reached any
other way: `Next::crumb` is an `int`, so a search that allocates 2.1e9 crumbs
breaks regardless of how much RAM is available. Reclaiming makes the ceiling
apply to *live* crumbs rather than cumulative ones.

Non-goal: reducing the frontier. `Next` is 48 bytes and a step pushes one per
surviving transition, versus one 8-byte crumb per step, so the frontier remains
the larger consumer. This plan does not compete with `anagram-perf.md` #5; it
removes a growth term that #5 doesn't touch.

## Settled decisions

- **Mark-and-compact, not refcounting.** `Crumb` is 8 bytes today
  (`int parent; char ch;` at `search.h:80-83`). A refcount must be 32-bit — a
  crumb accumulates references indefinitely as descendants are popped, so 16
  bits can overflow — which takes `Crumb` to 12 bytes. Freeing would have to
  reclaim >33% just to break even. Compaction adds zero per-crumb overhead and,
  unlike a free list, actually returns memory instead of merely capping growth.
- **Collect at the top of `step()`, before the pop.** That is the only point
  where the root set is exactly `nexts.c`. After the pop, the local `next` is
  also a root; inside the child loop, the partially-built `new_next.crumb` is a
  third. Collecting before the pop keeps the invariant to one sentence.
- **Growth-triggered with adaptive backoff, not a fixed step interval.** A fixed
  interval decouples the cost from the thing it pays for. See Frequency.

## Key facts this design rests on

1. **Only two things reference crumbs.** `Next::crumb` on frontier entries
   (`search.h:51`) and `Crumb::parent` chains. Nothing else: `seen` holds
   `make_seen_key()` strings, and `text` points into `match`, a `std::string`
   copy built at `search-driver.cpp:115-121`. **A reported result therefore pins
   no crumb**, and collecting between steps cannot invalidate `text`.
2. **Parents always have lower indices than their children.** A crumb is
   appended only when a child transition survives the filter
   (`search-driver.cpp:92-97`), so `parent < index` always holds. Compaction can
   be a single forward scan with no fixups.
3. **`crumb` is not part of the queue ordering.** `Next::operator<`
   (`search.h:71-73`) compares only `choice.count * scale`, so rewriting every
   entry's `crumb` in place needs no re-heapify.
4. **The whole frontier is enumerable.** `NextQueue` exposes the backing
   container `c` (`search.h:93-95`), added for `queue_median_score()`. Without
   it this plan would not be implementable at all — `std::priority_queue` alone
   offers no way to reach or rewrite the entries.
5. **`crumbs` is a `deque`** (`search.h:102`). `resize()` down frees whole
   blocks off the back. A `vector` would need `shrink_to_fit`, which
   reallocates — and a doubling-style spike is precisely the failure mode
   `anagram-perf.md` blames for the OOM.

## Design

### Mark

Roots: the `crumb` field of every entry in `nexts.c`. Walk `parent` chains,
stopping at the first already-marked crumb and at `-1`. The early exit is what
makes this affordable — chains share ancestors heavily, so total work is
O(live crumbs + queue size), not O(queue x depth).

### Remap via rank, not a side table

The obvious `vector<uint32_t> newindex` costs 4 bytes per crumb: a 144 MB
transient at 36M crumbs, allocated in a process that is about to `bad_alloc`.
Wrong shape entirely.

Instead the mark bitvector *is* the remap. With per-block popcount summaries,
the new index of a live crumb is `rank1(old)` — the number of live crumbs before
it. ~1.06 bits/crumb, so ~5 MB at 36M, and O(1) per lookup.

### Compact

Forward scan over `crumbs`. For each live crumb `i`, write
`{rank1(parent), ch}` to slot `rank1(i)`. Fact 2 guarantees `parent`'s new index
is already computable; fact 5 makes the trailing `resize(live)` actually return
memory. `parent == -1` passes through untouched.

### Rewrite the frontier

`nexts.c[i].crumb = rank1(old)` for every entry, per fact 3. The seed's
`crumb == -1` (`search-driver.cpp:22`) is left alone.

## Frequency

```c
if (crumbs.size() >= gc_threshold) collect();

// at the end of collect():
double reclaimed = 1.0 - double(live) / double(before);
if (reclaimed < 0.10) gc_slack = std::min(gc_slack * 2.0, 16.0);
else                  gc_slack = 1.5;
gc_threshold = std::max(kMinCrumbs, size_t(live * gc_slack));
```

`kMinCrumbs` ~1M, so short searches never collect at all.

Two properties earn the complexity:

- **Amortized cost is bounded by the growth factor.** At 1.5x, each collection
  does O(live + queue) work in exchange for 0.5 x live steps' worth of new
  crumbs — a small constant per step, expected in the low single-digit percent.
- **It self-corrects on the open question.** Whether crumbs are mostly dead or
  mostly live is unmeasured (see Risks). Mostly live: the backoff walks the
  threshold to 16x and the collector fades to near-zero cost, having spent a
  couple of percent to discover that. Mostly dead: the 1.5x trigger holds the
  high-water mark near the live set. The policy is safe without knowing the
  answer in advance.

A further "skip while the frontier is still growing fast" heuristic is tempting
but `kMinCrumbs` mostly covers it. Leave it out until measured.

## Implementation notes

- **The restart push aliases the parent's crumb.** `search-driver.cpp:134` sets
  `new_next.crumb = next.crumb`. It matters for any refcount scheme (the
  reference can't be dropped at pop) and is harmless for mark-and-compact, but
  it must not be mistaken for a bug when reading the reachability argument.
- **The lazy-allocation idiom stays intact.** `new_next.crumb = crumbs.size()`
  plus the `if (int(crumbs.size()) == new_next.crumb)` guard
  (`search-driver.cpp:82, 92`) still works, because compaction keeps indices
  dense and append-only between collections. This is the other reason to prefer
  compaction over a free list, which would force that idiom to be rewritten.
- **`find-expr` must be unaffected.** The collector is generic to the driver and
  triggers on size alone; no flag, no behaviour change, and with `kMinCrumbs`
  most `find-expr` searches never reach it.

## Verification

`crumbs_size()` is already in the progress line
(`source/search-printer.cpp:11-13`), so the collector reports on itself:

- **Sawtooth** — reclaiming; compare peak RSS against baseline at 20 and 25
  letters per the `anagram-perf.md` measurement table.
- **A straight line through the backoff** — the crumbs really are nearly all
  live, the collector has correctly given up, and `anagram-perf.md` #5 (frontier
  size) is the only lever left.

Correctness check that matters most: the match set and scores must be
byte-identical with and without the collector, at 8/12/16 letters where runs
complete. Path reconstruction reading a compacted-away crumb would corrupt
output rather than crash, so a diff is the test.

## Risks

- **The reclaimable fraction is unmeasured, and smaller than
  `anagram-perf.md` #6 implies.** That finding framed the full ~290 MB as
  recoverable. It isn't: the current code already allocates lazily, so a step
  whose transitions are *all* rejected pushes no crumb
  (`search-driver.cpp:92-97`). For an anagram search, walking into a letter the
  bag can't supply is the common death and is already free. Only crumbs whose
  subtrees died *after* producing at least one surviving child are reclaimable.
  The backoff in Frequency is the mitigation — this risk costs a couple of
  percent CPU, not a rewrite.
- **`-c` should reclaim better.** Suppressed restarts under `canonical_order`
  kill whole subtrees outright (`out_of_order`, `search-driver.cpp:35-40`), so
  measure with and without; a null result on the default path is not a null
  result for the mode long bags actually use.
- **Rank/select is fiddly to get right.** Keep the block summaries simple
  (one uint32 per 512 bits, plus a popcount over the partial word) and unit-test
  `rank1` directly rather than debugging it through the search.

## Related

- `findings/anagram-perf.md` — #6 is this plan; #5 (shrink `Next`, avoid the
  doubling cliff) is the larger, independent frontier win.
- `plans/trie-node-ordered-permutations.md` — delivered `-c` and `last_seg`,
  and the `NextQueue` exposure this plan depends on.
