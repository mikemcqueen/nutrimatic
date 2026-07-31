# Plan: collapse word-order permutations via trie-node ordering

## Goal

Stop `find-anagrams` from exploring every k-word solution in all k! orderings.
The target is **peak memory**, not CPU: the redundant orderings sit in
`SearchDriver::nexts` as low-score entries that are never popped before the
`priority_queue`'s backing vector doubles and OOMs. See
`findings/anagram-perf.md` for measurements and `findings/reduce-permutations.md`
for the option analysis this plan implements.

Non-goal: making the permutation work itself faster. If a branch is pruned
before it expands, its CPU cost disappears as a side effect; that's a bonus, not
the objective.

## Settled decisions

These came out of the analysis and are not open for re-litigation in this plan:

- **Ordering, not a visited-set.** A canonical-order constraint costs zero
  additional memory; an `unordered_set` of visited combos trades priority-queue
  memory for hash-table memory at roughly par, which defeats the purpose.
- **Order by trie node offset, not lexically.** Any *total* order collapses
  k! → 1; the ordering rule only has to be consistent, not meaningful. The node
  offset is already computed and needs no string reconstruction.
- **The unit is a segment, not a word.** A segment is the string consumed
  between two restarts. Usually one word, but a contiguous corpus phrase
  (`pen built` at `scale == 1.0`) is one segment spanning two words. Constraining
  order *between* segments keeps contiguous phrases intact; constraining order
  *within* them would reject `pen built` (since `pen > built`) and forfeit the
  main advantage of the trie architecture.
- **Off by default, behind a flag.** It changes which results appear and their
  scores (see Risks).

## Key facts this design rests on

1. **A trie node offset uniquely identifies the string from root to that node.**
   One word = exactly one node ID (not one per character). Verified against
   `index-writer.cpp`: nodes are written when their subtree completes, `pos` is
   monotonically increasing, and each node has exactly one parent path.
2. **Leaves are the exception.** `index-reader.cpp:90,93,96,103` set
   `choice.next == (off_t)-1` for any node with no children. Every leaf segment
   therefore shares one non-identity. This is the hole that must be handled;
   see Phase 3.
3. **The restart block is the only segment boundary.**
   `source/search-driver.cpp:76-86` is the sole point where a path returns to
   the root. A space alone is not a boundary — a space transition may instead
   continue contiguously into a longer phrase, so the segment is only known to
   be finished at the restart push.

## Design

Add to `SearchDriver::Next`:

```
IndexReader::Node last_seg;  // node ID of the most recently completed segment
```

- Seeded to `0` in the constructor (below every real offset, so the first
  segment is unconstrained).
- **Inherited unchanged** by every character-transition child pushed in the loop
  at `search-driver.cpp:44-56`.
- **Set at the restart push** to the ID of the segment just completed, which is
  `next.choice.next` (the node reached after consuming the trailing space).
- **Checked at the restart push**: only push if
  `completed_seg >= next.last_seg`. Use `>=`, not `>`, so a repeated word stays
  legal.

The constraint is generic to the driver, not anagram-specific, so it goes in
`SearchDriver` behind a constructor parameter (default off) rather than in
`AnagramFilter`. `find-expr` must be unaffected with the flag off.

Memory note: `Next` is currently 40 bytes; this takes it to 48, and the field is
paid by `find-expr` too. That is a 20% per-entry increase against a plan whose
goal is reducing memory — acceptable only because the pruning ratio is expected
to be far larger, but see Phase 4 for clawing it back.

## Phase 0 — measure before building

Two numbers decide how much of the rest is worth doing.

1. **How often is the completed segment a leaf?** Instrument the restart block
   to count `next.choice.next == -1` against the total. If leaves are a small
   tail, Phase 3 is optional; if they're common, the simple leaf handling
   throws away most of the benefit.
2. **Frontier composition.** Log `nexts.size()` alongside the existing
   `# <count>` progress lines (reuse `-p` so the output stays readable). This is
   the baseline for judging every later phase, and `anagram-perf.md` notes that
   *none* of its reasoning has been measured.

Run both against `idx/wiki-merged.5.index` at 12, 16 and 20 letters, per the
existing measurement table.

## Phase 1 — `-x/--max-words`

Independent of the ordering work, cheapest win, no soundness questions. It also
independently kills the "fifteen two-letter words" paths that
`anagram-perf.md` identifies as the highest-priority junk in the queue.

### Count spaces, not restarts

The unit here is the **word**, not the segment. A restart is not a word
boundary: `pen built` matched contiguously from the corpus is one restart but
two words, so a counter incremented at the restart would let `-x 4` return a
six-word result made of three two-word phrases.

The internal space in `"pen built "` *is* consumed as an ordinary character
transition out of `children()`. The restart at `search-driver.cpp:76-86`
consumes nothing — it reuses the same `' '` and carries `state` over unchanged
(line 84). So **counting `' '` characters counts words; counting restarts counts
segments.**

Implementation trap that follows: increment where the space-child is *created*,
in the `tmp` loop, and have the restart push inherit the count unchanged. An
increment keyed on "popped entry has `ch == ' '`" double-counts the same space.

### Options considered

**A. A digit in `AnagramFilter`'s mixed radix**, above the existing `min_len`
digit — the trick the `-m` comment at `find-anagrams.cpp:13-22` describes.
Semantically ideal: the filter sees every `' '`, so it counts words natively,
and it would compose with `find-expr`'s `<...>` operator for free.

**Rejected on arithmetic.** The cost is a `(max_words + 1)x` state multiplier:

```
26 distinct letters -> product = 2^26  = 67.1M
x (min_len + 1) = 5                    = 336M
x (max_words + 1) = 7                  = 2.35e9   >  INT_MAX (2.147e9)
```

It overflows precisely on the 26-letter bag the feature exists for, tripping the
"anagram too long" exit at `find-anagrams.cpp:48`. Widening
`SearchFilter::State` to 64 bits would fix it while growing the `state` field
for every `Next` in every tool — the wrong trade in a memory-driven plan.

**B. A counter incremented at the restart push.** Counts segments. Wrong, per
above.

**C. `uint8 words` on `Next`, incremented when a `' '` child is created.**
Chosen. Counts words correctly, costs nothing in filter state space, and prunes
at the same depth option A would:

```c
if (tmp[i].ch == ' ' && next.words + 1 > max_words) continue;
```

### Making it free: flatten `Choice` into `Next`

`IndexReader::Choice` carries a 7-byte hole after `ch`
(`char` + 7 pad + `int64_t count` + `Node next` = 24), and nesting it in `Next`
inherits that hole. Flattening its three members directly into `Next` reclaims
it, so `words` lands in padding already being paid for. Measured:

```
Choice                                        24
Next, current                                 40
Next, Choice flattened in + uint8 words       40   <- unchanged
Next, flattened + words + last_seg            48
```

Cost is copying three fields instead of `new_next.choice = tmp[i]`, plus
updating the `static_assert` at `search.h:38`. Roughly 6 spare bytes remain —
but *not* enough for Phase 2's 8-byte `last_seg`, which still forces 48.

### Details

- `max_words` belongs next to `restart` as a `SearchDriver` constructor
  parameter. This does teach the generic driver that `' '` delimits words, but
  line 77 already special-cases `' '` for the restart, so it is consistent with
  the existing layering rather than a new violation.
- `0` = unlimited, matching the `min_word_len` convention at
  `find-anagrams.cpp:28`. Reject values `> 255` at parse time for the `uint8`.
- **`-x` is frequently non-binding, which will confuse Phase 0's measurements.**
  `min_len` already caps words implicitly at `floor(letters / min_len)`. With 26
  letters and `-m 4` that implicit cap is 6, so `-x 6` does nothing at all; only
  `-x 4` or `-x 5` prunes. Check this before attributing a null result to the
  implementation.

### Out of scope

A stronger feasibility prune — at a space with the bag non-empty, require
`letters_remaining >= min_len` — needs the remaining letter count, which is not
cheaply recoverable from the mixed-radix bag state (it means summing digits).
Worth revisiting only if profiling says it matters.

Re-measure. It's plausible this alone moves the 20-letter case from "nothing in
60 s" to finishing.

## Phase 2 — node-ordered segments

Implement the design above, behind `-c/--canonical-order` (or similar).

Leaf handling for this phase: **skip the constraint when
`next.choice.next == -1`** — always allow the push. That is sound (it only
declines to prune) and costs one condition. Phase 0's first number says how much
is being left on the table.

## Phase 3 — close the leaf hole, only if Phase 0 says it matters

The identity of `"cat "` can be recovered from the node for `"cat"` — the node
*before* the trailing space — which is never a leaf, since it has the space as a
child. Getting it requires carrying the node being expanded (`next.choice.next`
at the time the child was created) on `Next`, i.e. a second 8-byte field, taking
`Next` to 56 bytes.

That is a poor trade on its own. Only do it if Phase 0 shows leaf segments are
common *and* Phase 4 has bought back the space.

Rejected alternative: a bounded open-addressed table of 64-bit combo hashes.
It's the only variant that keeps the *highest-scoring* arrangement rather than
an arbitrary one (see Risks), so it stays on the table if the score loss proves
unacceptable in practice — but it reintroduces a fixed memory cost and is
strictly more machinery.

## Phase 4 — pay for the new field

From `anagram-perf.md` idea #5: `float` log-score instead of `double scale`, and
`int32` count, plus the field reordering already landed in
`source/search.h:29-45`. That takes `Next` back to ~32 bytes even with
`last_seg`, i.e. net *below* where it started. Do this if Phase 2 measurements
show the added field eating into the win.

## Verification

- **Soundness**: on an 8- and a 12-letter bag, collect results with and without
  `-c`. Every word *set* present in the unconstrained run must be present in the
  constrained run. Scores and arrangements may differ (that's expected — see
  Risks); missing sets are a bug.
- **Effect**: peak RSS and `nexts.size()` at matched step counts, with and
  without the flag, at 12/16/20 letters.
- **Regression**: `find-expr` output must be byte-identical with the flag off.
  Run `test-expr`.
- Confirm the leaf-skip path is actually exercised (the counter from Phase 0
  should be non-zero on a real run) so it isn't silently dead code.

## Risks

- **The kept arrangement is not necessarily the best-scoring one.** For pure
  restart paths the score is order-independent (`pilot but when` and
  `but when pilot` tie exactly), so nothing is lost. The loss is confined to
  contiguous phrases: `pen built` scores **7** while `built pen` scores 2.1e-05,
  and node ordering may keep the wrong one. Bounded, but real — hence the flag,
  and hence defaulting it on only for long bags where contiguity is hopeless
  anyway (a 26-letter phrase is near `HISTORY_WINDOW_SIZE` in `make-index.cpp`
  and needs 5+ occurrences to survive the merge cutoff).
- **Dedup is per-segmentation.** `{A,B,C}` reached as three segments and as
  `(A B)(C)` are different segment multisets and are *not* deduped against each
  other. Intended — they score differently and the contiguous one is wanted —
  but it caps the achievable pruning ratio below a true k! reduction.
- **`Next` growth is paid by `find-expr`**, which has no use for the feature.
  Phase 4 exists to neutralize this.
- **The node-identity property is emergent, not enforced.** It follows from
  `IndexWriter` never coalescing identical subtrees. That holds today (streamed
  trie, offsets are file positions), but nothing in the code asserts it. Add a
  comment at the point of use recording the dependency.

## Open question

Whether `Node` (`off_t`, 8 bytes) can be stored as `uint32` in `Next`. It's a
file offset, so it fits only if the index is under 4 GB — worth a runtime check
at construction with a fallback, but only as part of Phase 4, not before.

## Related

- `findings/reduce-permutations.md` — option analysis, including the
  visited-set variants this plan rejects.
- `findings/anagram-perf.md` — why long anagram searches blow up.
