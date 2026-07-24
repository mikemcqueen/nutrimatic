## IDEA

take a look at ~/code/clues/src/tools/an/ancc.cc.  that's some old-school anagram-finder
code that is using a heavily optimized algorithm for (in part) ensuring no duplicate
permutations of same-words.  it relies on a dictionary of words.

compare to source/find-anagrams.cpp, which proceeds (i believe) based on scores in an
index file, and uses some hackery to avoid permutations.  this version is "fine" from
an execution time perspective but unacceptable from a memory-usage perspective.  memory
use grows exponentially and we cap out at about length 20 and every single size above
hat is a doubling.  ancc suffers from no such issues.

so, what i'm wondering, and submitting to your big brain for consideration, is whether
we can leverage some technique from ancc into find-anagrams, in order to a) potentially
reduce the search space, but mostly b) to eliminate the bookeeping memory required and
allow for much greater anagram length analysis.

take your time, think carefully and deeply, consider multiple possible approaches, and
add your findings below.

## FINDINGS

### The short answer

Yes, and the single most valuable thing to take is **not** the permutation trick.

The premise in the IDEA section needs one correction before anything else: ancc
does not avoid the memory blowup *because* it avoids duplicate permutations. It
avoids it because it is **depth-first**. Its whole live state is `found[level]`
and one `bitmask_t new_phrase_mask[num_phrase_masks]` VLA per stack frame —
a few kilobytes at depth 30, and nothing accumulates across the run because
results are streamed and forgotten. `find-anagrams` is **best-first**, and a
best-first search is *defined* by keeping every unexpanded path resident. That
frontier is the memory, and no amount of permutation cleverness changes its
asymptotics.

What ancc's permutation trick actually buys is what makes depth-first
*affordable*: a DFS has to enumerate exhaustively rather than stopping after the
first few good hits, so it can only win if the tree it walks is small.

So the transplant is two separate things, and they are worth different amounts:

1. **Depth-first traversal** — fixes memory, completely and permanently. This is
   the answer to goal (b).
2. **Rarest-letter forcing + anagram-multiset collapsing** — shrinks the tree by
   two to three orders of magnitude. This is goal (a), and it is what makes (1)
   practical.

Measurements below; they are lopsided enough to be worth reading first.

### Measured, against `~/code/nutrimatic/idx/wiki-merged.5.index`

Current `find-anagrams`, `-m 4 -c`, killed at 30 s:

| Letters | Peak RSS at 30 s | Output |
|---|---|---|
| 14 | **4.2 GB** | a few results, e.g. `6.429e-19 feat studio tsen` |
| 19 | **8.7 GB** | a few results, e.g. `6.611e-09 fire station team used` |

~290 MB/s of frontier growth, still climbing when killed. This matches the
"caps out around 20" report.

A throwaway two-phase prototype (`get the words out of the trie`, then ancc's
DFS) on the same index and same bags, min word length 4:

| Bag | Words (phase 1) | Result | Total wall | Peak RSS |
|---|---|---|---|---|
| 14 letters, unbounded words | 17,274 | **1,315** multiset solutions, 25,157 DFS nodes | **0.03 s** | 93 MB |
| 19 letters, ≤4 words | 95,629 | **156,138** solutions, 5,488,296 nodes | **3.25 s** | 177 MB |
| 21 letters, ≤4 words | 144,103 | **1,474,843** solutions, 55,895,156 nodes | **37.2 s** | 220 MB |

Wall time is the whole run, both phases. Phase 1 measured separately at 0.78 s
for the 21-letter bag (144,103 words, 313,441 trie nodes visited) and 1.30 s for
all 26 letters (707,803 words) — so phase 2 dominates everywhere above.

The RSS figures are almost entirely phase 1's word list, stored carelessly
(`std::string` plus a 26-byte count array per word); the DFS itself is O(depth).
The important property is not that the number is small, it is that it is **flat**
— it does not grow with depth, runtime, or solution count.

26 distinct letters (`a`–`z`) still did not finish in 120 s at ≤4 words. That
bag is pathological and probably not worth optimizing for, but it means the
technique buys roughly +6-7 letters of headroom, not unlimited length.

Two component measurements that explain the phase-2 numbers:

- **Anagram collapsing is worth ~300x.** Collapsing words that are anagrams of
  each other into one search branch took the 14-letter search from 7,709,970
  DFS nodes to 25,157 — **306x** — because the factor applies at every level.
  The collapse ratio is 7.0x at 14 letters, 5.8x at 19, 3.1x at 26.
- **Permutation elimination is structural and free.** On a 12-letter bag the
  forced-letter DFS emitted 35,041 solutions and **35,041 distinct sorted word
  sets — zero duplicates, with no dedup structure of any kind.** Those same
  35,041 sets are 191,800 orderings, which is what `find-anagrams` reaches and
  then filters through `seen`.

### What ancc is actually doing

Itemized, because the pieces are separable and worth different amounts:

**T1. Depth-first recursion.** `check_dict` recurses; state per frame is one
`int` and a short mask array. Nothing is retained. *(The memory fix.)*

**T2. Rarest-letter forcing.** `high_letter_num` is the MSB of `phrase_mask`,
i.e. the least-frequent-in-dictionary letter still in the bag. Only words
*containing that letter* are candidates, found in O(1) via `let_hash[]` because
words are sorted by their own rarest letter. Two independent effects:

  - *Fail-first.* The hardest constraint is resolved at depth 1 instead of
    depth k. A bag with a `j` in it deals with the `j` immediately, where the
    candidate list is tiny, instead of consuming 20 easy letters and only then
    discovering the leftovers spell nothing.
  - *Canonicalization, for free.* The word covering letter L must be chosen
    *now*. If L occurs once in the bag, exactly one word of any solution set
    contains it, so its position in the enumeration order is **forced** — the
    k! orderings collapse to 1 with no bookkeeping at all. When L occurs more
    than once the choice is ambiguous, and `entry_point` (non-decreasing word
    index *within the same letter group*) breaks the tie.

**T3. Bag as a stack of bitmasks.** `phrase_mask[k]` = the set of letters
occurring at least k+1 times. Subset test is one AND/compare per level (usually
1–2), and `num_word_masks[n] > num_phrase_masks` is an O(1) pre-reject.
Subtraction is a borrow trick, `(tempmask | phrase_mask[n]) - tempmask`.

**T4. Dictionary pre-filtering.** `get_words` keeps only words makeable from the
whole bag, once, up front.

**T5. Anagram collapsing.** Words with identical mask arrays sort adjacent, and
all but the first get `num_word_masks = BIG_INT`, which the search's existing
pre-reject skips. `print()` re-expands them at output time. The search therefore
runs over *letter multisets*, not spellings — spellings are a formatting concern.
*(The 306x.)*

**T6/T7. Length-sorted skip index and short-circuit.**
`word_len_hash[phrase_len][L]` jumps straight to the first word short enough to
fit; because words are sorted long→short, the first word that is too short means
every remaining one is, so it `return`s rather than `continue`s.

### What `find-anagrams` would be giving up

This has to be stated plainly, because a naive transplant is a regression on the
tool's whole reason to exist:

1. **Ranked output.** Results come out best-first by corpus frequency, so five
   results are useful and you can stop. ancc emits everything, unranked. At 19
   letters "everything" is 156,138 word sets / ~10^9 spellings.
2. **Contiguous multi-word segments.** `pen built` scores 7 because it is a real
   corpus phrase found in one trie descent; `built pen` needs a restart and
   scores 2.1e-05. A word-list search cannot see phrases at all — this is
   exactly the advantage `findings/anagram-perf.md` idea #7 identifies.

Both are recoverable. See "Keeping what matters" below.

### Designs, ranked

#### A. Two-phase: extract words from the trie, then ancc's DFS

This is `anagram-perf.md` idea #7, but with a finding that document did not
have: **phase 1 is essentially free, because the trie already is the
dictionary.** Walking the index with the bag as a filter and stopping at the
first space visits only 313k nodes for a 21-letter bag — the bag prunes the
1.2 GB index down to nothing. 144k words in 0.78 s.

Phase 1 also captures the right score for free: the child count at the `' '`
transition *is* the corpus count of that word, which is what `SearchDriver`
scores segments by, so the score model transfers directly. (Verify the restart
constants against `SearchDriver::step()` before trusting absolute numbers.)

Phase 2 is `check_dict`, with T2/T3/T5/T6/T7. Memory O(depth) plus the word list.

Highest throughput, simplest correctness story, biggest capability loss (#2).

#### B. Depth-first driver over the trie

Keep the trie walk and everything it can express; replace the frontier with an
explicit DFS stack of `(node, count, filter state, log_score, last_seg)`.
`IndexWalker` in `source/index.h` is already a stack-based trie DFS and is the
template.

What this deletes outright:

- `nexts` — the path *is* the stack.
- `crumbs` and the whole of `collect()`/`Marks` — the path is the stack.
- `seen` — see the caveat below.

Memory becomes a constant. Contiguous phrases and the existing `-m`/`-c`/`-u`
semantics all survive untouched. This is the **minimum change that fixes exactly
the stated problem with no loss of capability**, and it is my recommendation as
the first substantial piece of work.

*Caveat on deleting `seen`:* it is not purely a permutation filter. It also
dedups different *segmentations* of the same word set — `{pen built}` as one
contiguous segment versus `{pen}{built}` as two — keeping the better-scoring
one, which is a real feature. Canonical ordering does not subsume that. Either
keep `seen` (it is O(matches), not O(frontier), so it is the least of the three
problems) or handle segmentation collisions deliberately.

#### C. Rarest-letter forcing inside `AnagramFilter` — cheap, orthogonal, do it first

T2 can go into the existing filter with **one extra bit of state and no driver
change at all**, because of a small fact that makes it work:

> Fix a global letter priority order (ascending corpus frequency). Let L be the
> highest-priority letter still in the bag. Within a segment, L is **stable**
> until it is consumed: letters only leave the bag, and removing letters other
> than L cannot promote anything above it.

So the rule is self-consistent without storing L anywhere:

- Carry one flag: "this segment has consumed the letter that was forced at its
  start."
- On a character transition, if the flag is clear and `ch == rarest(bag)`, set it.
- `' '` is only allowed when the flag is set; the flag resets to 0 after it.

`rarest(bag)` is a pure function of the state, which already *is* the bag. The
state-space cost is exactly **×2** — note the 26x multiplier that
`anagram-perf.md` §2 rejected was a different scheme; one bit is nothing against
`product` (2^26 for a 26-distinct-letter bag).

`allowed_chars()` expresses it directly: just don't offer `' '` when the flag is
clear, alongside the existing `word < min_len` test.

**Soundness warning: C and `-c` conflict, and combining them as written will
silently drop solutions.** `-c` requires segment node IDs non-decreasing
*globally*; C imposes its own order (whichever segment covers the rarest letter
goes first). These are independent partial orders and a word set can satisfy
neither jointly — e.g. rarest letter L sits in segment A, next-rarest L' sits in
segment C, forcing A before C, while `node(A) > node(C)` forbids exactly that.
The fix is ancc's: **restrict the node ordering to the ambiguous case only** —
reset `last_seg` to 0 whenever the forced letter advances, so non-decreasing node
order is required only among segments covering the *same* forced letter, which
are genuinely interchangeable. That is `entry_point`, and it is a small precise
change to `out_of_order()` rather than a new mechanism.

Note what C does *not* do: it does not change the memory asymptotics. In
best-first it shrinks the frontier by fail-first pruning, which is real but
bounded. Its value is that it is nearly free, it is strictly better than `-c`
at the same job, and it is a prerequisite for B being fast.

#### D. Anagram-multiset collapsing

Worth 306x on its own and independent of everything above, but it does not fit
the trie walk — the trie is indexed by spelling, and collapsing requires
grouping by sorted letters. It needs the phase-1 word list, i.e. design A.

This is the strongest argument for A over B. It is also the thing `seen` is
currently doing the expensive way: `make_seen_key()` sorts the words of a match
*after* the search has walked every spelling and every permutation to find it.

### Keeping what matters

**Ranking, under DFS.** Two options, both O(depth) or O(N):

- *Top-N heap.* Run the DFS to completion keeping the best N results. Exactly
  ranked output for bounded memory, at the cost of no streaming. Fine for
  N = 10k.
- *Branch and bound / IDA\*.* Maintain `g` = accumulated log-score and prune when
  `g + h(bag) < threshold`; stream anything that beats the threshold immediately;
  restart with a halved threshold until enough results. Each pass is O(depth).

  An admissible `h` is easy here: let `rate = max over makeable words w of
  (log2(count(w)) + log2(restart)) / len(w)`, then `h(bag) = |bag| * rate`. Any
  completion partitions the remaining letters into words, each contributing at
  most `len * rate`. Tighter: `h(bag) = sum over remaining letters c of
  best_rate_containing[c]`, still admissible since charging every letter its own
  best rate can only over-estimate.

  Worth noting that this `h` **is** idea #3 in `anagram-perf.md`
  ("goal-directed priority"), which that document calls the highest-leverage
  untouched item. The ancc line of thinking and the existing TODO converge on
  the same function.

**Contiguous phrases, under design A.** Recoverable: phase 1 does not have to
stop at the first space. Let it continue past a space and emit multi-segment
strings as additional "words" carrying their real phrase count. The bag filter
bounds the explosion, and the resulting list contains `pen built` as a single
scored entry, so phase 2 finds it with no special handling. This makes A a
strict superset rather than a trade — worth measuring the list-size cost before
committing.

### Suggested order of work

1. **C, minus the `-c` interaction** — forced-letter flag in `AnagramFilter`,
   with `last_seg` reset on forced-letter advance. Small, self-contained,
   testable against current output, and it helps the existing best-first driver
   immediately.
2. **Prototype A properly** and measure phase 1 with multi-segment extraction
   turned on. Cheapest route to "much greater anagram length" — the 19- and
   21-letter results above are already usable and the current tool cannot reach
   them at all.
3. **B**, if contiguous phrases and exact score ranking matter more than the
   306x. Consider doing B *and* A as two modes rather than choosing.
4. **The `h` bound**, which pays off in all three designs and closes
   `anagram-perf.md` idea #3.

### Things I did not verify

- That the score model reproduces exactly in a word-list design. The per-segment
  count transfers; the restart constants were not checked against
  `SearchDriver::step()`.
- The multi-segment phase-1 extraction cost — asserted as "bounded by the bag
  filter", not measured.
- The prototype's spelling-expansion count over-counts solutions that use the
  same letter multiset twice (it multiplies class sizes where it should choose
  unordered pairs), so the `spellings` figures above are ~10% high. Solution and
  node counts are exact.
- Whether the `-c` conflict described in C is reachable in practice or merely
  in principle. I constructed it on paper; it was not observed.

### Related

- `findings/ancc-inspiration-summary.md` — digest of this document: what each
  multiplier above actually measures, projected bag-length and memory ceilings
  per design, and a revised order of work for the case where score ranking
  matters but in-order streaming does not.
- `findings/anagram-perf.md` — idea #3 (`h`, above) and idea #7 (design A).
- `findings/reduce-permutations.md` — why `-c` orders by trie node, and the
  segment-identity argument that design C's tie-break reuses.
- `findings/priority-invariant.md` — the approximate pop-order invariant `seen`
  rests on, which designs A and B make moot.
