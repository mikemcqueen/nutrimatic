# Eliminating permutations of the same word set

The problem this addresses is not CPU time but **memory**: every k-word solution
is explored in all k! orderings, and the redundant branches sit in
`SearchDriver::nexts` as low-score entries that are never popped before the
`priority_queue`'s vector doubling OOMs. See `findings/anagram-perf.md` for the
measurements and the surrounding failure analysis; this document is specifically
about collapsing the orderings.

## Where the hook goes

The restart block, `source/search-driver.cpp:76-86`. That is the only point in
the search where a path returns to the trie root, and it is exactly where a
permutation branch is born. Everything below concerns what to do there.

Terminology: a **segment** is the string consumed between two restarts. That is
usually one word, but it can be a *contiguous multi-word phrase* pulled straight
from the corpus (`pen built` at `scale == 1.0`) — which is the case that makes
this tool better than a plain bag-subtraction script, so the design has to
preserve it.

## 1. The trie node is already a free segment ID

At the restart, `next.choice.next` is the node reached by consuming the segment
plus its trailing space. A trie has exactly one path per node, so **that `off_t`
is a collision-free identifier for the segment**, already computed. `listen `
and `silent ` land on different offsets; two paths landing on the same offset
consumed the same characters, by construction.

So no `vector<string>` of the vocabulary, no `unordered_map<string, uint16_t>`,
and no reconstruction of the word from crumbs in order to hash it.

Two notes on the vocabulary-index approach this replaces:

- A Wikipedia-derived index has millions of distinct tokens, not 65k (numbers,
  names, misspellings, foreign words all survive the merge cutoff), so a
  `uint16_t` word ID was never going to fit.
- A word vocabulary can't name a contiguous multi-word segment at all. Node IDs
  handle it naturally.

**Assumption to verify before building on this:** that `IndexWriter` never
coalesces identical subtrees into a shared node. Reading `index-writer.cpp` it
is a straight streamed trie with offsets being file positions, so distinct
strings should always get distinct offsets — but it is the load-bearing
assumption here.

## 2. Canonical ordering — recommended, costs zero memory

Rather than remembering which combos have been seen, **require the segment IDs
along a path to be non-decreasing.** Carry a `uint64 last_segment` field on
`Next`; at the restart, only push if `next.choice.next >= last_segment`. Any
multiset of segments has exactly one non-decreasing arrangement, so k! orderings
collapse to 1. Use `>=` rather than `>` so repeated words stay legal.

Why this beats a visited-set, given that memory is the actual concern:

- **No set at all.** An `unordered_set<WordComboIdx>` is itself an unbounded
  memory consumer: 16 bytes of key becomes ~50+ with node and bucket overhead,
  and the number of distinct reachable combos on a 26-letter bag is plausibly in
  the tens of millions. That trades priority-queue memory for hash-table memory
  at roughly par.
- **It prunes earlier.** A visited-set only fires when a combo is *re-reached*,
  by which time the redundant path has already expanded character-by-character
  through the whole of its second word. Ordering rejects the branch at the
  restart, before any of that.
- **No crumb-walking, no sorting, no hashing** — one comparison.
- **It is a strict generalization of idea #2 in `anagram-perf.md`** ("first
  letter >= previous") without that idea's cost. That version was expensive
  because it encoded the constraint into `AnagramFilter`'s state, paying a 26x
  state multiplier against the ~2.1e9 `int` ceiling. Carrying it out-of-band on
  `Next` makes it free *and* gives full ordering rather than first-letter
  ordering.

Cost is 8 bytes on `Next`, which currently sits at 40 and would pack to 48.
Worth pairing with the `float` log-score change (idea #5 in `anagram-perf.md`)
to stay flat.

## 3. The caveat for any canonical scheme

Both schemes below and above keep exactly one arrangement of each word set, and
it is not necessarily the highest-scoring one.

For paths that are pure restarts the score is order-independent — which is why
the measurements showed `pilot but when` and `but when pilot` tying exactly — so
nothing is lost there. The loss is confined to the contiguous case: `pen built`
scores **7** as a real corpus phrase while `built pen` scores 2.1e-05, and if
the node ordering happens to disfavor the contiguous arrangement, the result
comes back with the bad score or not at all.

Since segments are the unit being ordered, a contiguous phrase is a single
segment and survives internally intact; it is only lost when it competes with
another segment for position. Damage is real but bounded. Put it behind a flag,
and default it on only for long bags where contiguity is hopeless anyway — a
26-letter phrase is near the 40-char `HISTORY_WINDOW_SIZE` in `make-index.cpp`
and would need 5+ occurrences to survive the merge cutoff.

## 4. The visited-set, if the caveat matters — but bound it

There is a legitimate reason to prefer a visited-set: it prunes duplicates
*without* choosing a winner, so the first arrangement reached — which in a
best-first search is the highest-scoring one — is the one that survives. That
directly fixes the problem in section 3.

If that tradeoff is worth it, make it memory-safe. Hash the sorted segment IDs
to a single `uint64`, and store it in a **fixed-size open-addressed table**
(power-of-two, e.g. 2^24 entries = 128 MB, or 2^26 = 512 MB) with
replace-on-collision, rather than an `unordered_set`. Memory becomes a constant
chosen at startup instead of a growth curve competing with the frontier.

Failure modes: a lost result on 64-bit hash collision (negligible), plus a lost
*dedup* on table collision, which only costs permutation work — the thing this
document's whole premise says is cheap.

## 5. Cap the word count

A fixed-size combo key implies a maximum word count that nothing currently
enforces. Make it explicit: a `-x/--max-words` flag with a `words` counter on
`Next`, incremented **when a `' '` child is created**, not at the restart —
a restart is not a word boundary, since a contiguous corpus phrase like
`pen built` is one restart but two words. See
`plans/trie-node-ordered-permutations.md` Phase 1 for the full option analysis,
including why folding it into `AnagramFilter`'s state overflows `INT_MAX` on
exactly the bag sizes that need it.

It is the natural complement to `-m`, and independently kills the "fifteen
two-letter words" paths that `anagram-perf.md` identifies as the
highest-priority junk in the queue. Note `-m` already caps words implicitly at
`floor(letters / min_len)`, so with 26 letters and `-m 4` only `-x 4` or `-x 5`
actually binds.

## Framing

Enforcing non-decreasing segment IDs turns `SearchDriver` into exactly the
canonical recursive bag-subtraction enumeration of idea #7 in
`anagram-perf.md` — same search space, same "each word set once" property —
except it keeps the priority queue, so results still come out in frequency order
and contiguous-phrase scoring still works. That keeps the architecture's one
real advantage while dropping the combinatorial cost that buys nothing.

## Suggested order of work

1. `-x/--max-words`. Cheapest, biggest immediate win, no soundness questions.
2. Non-decreasing segment IDs, behind a flag.
3. The bounded hash table, only if losing contiguous orderings proves to matter.

## Related

- `findings/anagram-perf.md` — why long anagram searches blow up, and the wider
  set of ideas this one refines.
- `findings/how-to-limit-runtime.md` — the `# <count>` progress-line protocol.
