# ancc-inspiration, in plain numbers

A digest of `findings/ancc-inspiration.md` for the case where **score-ranked
output matters but in-order streaming does not** — i.e. it is acceptable to emit
scored-but-unordered results and sort/top-N afterwards.

That relaxation matters more than it sounds. Roughly half of the complexity in
the original document exists only to make a depth-first search produce results
in descending score order as it goes (IDA\*, threshold restarts, admissible
heuristics). Drop that requirement and the recommendation gets simpler and the
recommended ordering of work changes.

---

## TL;DR

**The multipliers.** §0 has the full table; the short version is that **306x** is
DFS nodes visited (≈ phase-2 CPU time, *not* memory), **7.0x/5.8x/3.1x** is a
branching-factor reduction (dictionary entries → distinct letter multisets),
**×2** is one extra bit of `AnagramFilter` state against the `INT_MAX` budget
(neither time nor memory), and **290 MB/s** is the only genuinely memory-flavoured
number in the original document.

Two relations the original leaves implicit, which are what make the rest legible:

1. **Branching-factor wins compound with depth.** 7.0x is not a 7x speedup, it is
   7x *per level* — the measured 306x ≈ 7.0^2.9, and at a 4-word search it would
   be 200–1100x.
2. **Node count grows 3.2x per letter** (measured, 19→21). So a speedup of S buys
   `log(S)/log(3.2)` letters. A fully tuned 20-core implementation is 60x =
   **+3.5 letters**. Anagram collapsing alone is **+4.9**. That ranking is the
   whole argument of this document.

**Length and memory (§8).** The headline is that **memory stops being the
constraint under any of these designs.** Every configuration in §8 sits between
10 MB and 300 MB and stays there regardless of runtime or solution count. Today's
8.7 GB is a best-first *frontier*; a DFS has no frontier. At a 10-minute budget:
today ~19–20 letters and it never completes; approach B ~17–19 (~21–23 tuned and
parallel); approach A ~23 (~26–27 tuned and parallel).

**Anagram collapsing does not discard spellings (§2).** The 306x is a *search-time*
optimisation: the class `{yacht, cathy}` is explored as one branch instead of two,
then re-expanded at output so both are emitted. ancc does exactly this
(`print()`, `ancc.cc:1109-1118`). The only place a class member is preferred is
the pruning bound, which must use the class **maximum** to stay admissible — the
best member decides whether a branch is *explored*, never what is *printed*.

**The stream-then-filter plan works, up to ~16–17 letters if all spellings are
written (§6).** Solution counts grow 3.07x/letter, but each solution expands
~10^3-fold into spellings, so 19 letters is ~7 GB of output and 21 letters ~46 GB.
The fix is lazy exact expansion: generate each solution's spellings in descending
score order and stop at the current N-th best. That yields the true top N over the
full 10^9 while touching one or two spellings per solution, discards nothing, and
keeps the intermediate file useful as a file of *solutions* (60 MB at 21 letters).

**In-corpus word adjacency is a requirement, which promotes §5 from optional to
blocking.** Approach A only preserves adjacency if phase 1 extracts contiguous
phrases, and that costs F^depth for an unmeasured F. If F is bad, **approach B is
the answer** — it preserves adjacency natively with nothing to measure. This is
now the decision the work order turns on.

**The relaxed streaming requirement changed the order of work (§9).** It removes
the reason design C came first and deletes the IDA\*/threshold-restart machinery
entirely, reducing the whole ranking story to a top-N heap plus about ten lines of
branch-and-bound.

**The two weakest parts of this analysis**, stated up front rather than buried:

- **The phrase-recovery cost is worse than `ancc-inspiration.md` implies.** That
  document asserts it is "bounded by the bag filter"; the cost is actually
  F^depth in the class-count growth F, so it is either nearly free or it eats
  most of the collapsing win. §5 has a cheap mitigation and makes measuring it
  step 3 in the work order, because it decides whether design B needs to exist
  at all.
- **Design C's standalone value is unquantified**, and is left that way here
  rather than given a number. Its measured benefit is already baked into
  approach A's 3.25 s / 37 s — the prototype used forced-letter DFS throughout —
  so it is a prerequisite, not an additive win.

§8's B-row penalty (100–1000x) and the 5x/12x tuning factors are estimates from
mechanism, not measurements. The closing section separates measured from
projected explicitly.

---

## 0. Reading the numbers

The original document mixes several kinds of multiplier without labelling them.
Here is what each one means:

| Figure in `ancc-inspiration.md` | What it actually measures |
|---|---|
| **306x** (collapsing) | **DFS nodes visited**, i.e. phase-2 CPU time. 7,709,970 → 25,157 on a 14-letter bag. Not memory. |
| **7.0x / 5.8x / 3.1x** (collapse ratio) | **Dictionary entries → distinct letter multisets.** This is a *branching factor* reduction, applied once per level. |
| **×2** (design C's cost) | **Filter state-space encoding size** — one extra bit in `AnagramFilter`'s mixed-radix `State`, measured against the `INT_MAX` ceiling. Not time, not memory. |
| **26x** (rejected first-letter scheme) | Same units as above: state-space multiplier. |
| **720x** (6!) | **Redundant orderings** of one word set explored by the current best-first driver. Work, and frontier entries. |
| **191,800 vs 35,041** (5.5x) | Same: orderings reached vs distinct word sets, at 12 letters. |
| **290 MB/s** | **Frontier growth rate** — memory, in the current tool. |
| **+6-7 letters** | **Bag-length headroom.** |

The two most important relations, which the original document leaves implicit:

**1. Branching-factor wins compound with depth.** A collapse ratio of 7.0x is
not a 7x speedup; it is 7x *per level*. The measured 306x ≈ 7.0^2.9 — the
14-letter bag with `-m 4` is a 3-word search. At a 4-word search with the
19-letter ratio of 5.8x, the same lever is worth 5.8^3–5.8^4 ≈ 200–1100x.

**2. Constant-factor wins buy bag length logarithmically.** Measured node count
grows **3.2x per additional letter** (19→21 letters: 5.5M → 55.9M nodes). So any
speedup S buys `log(S)/log(3.2)` letters:

| Speedup | Extra letters bought |
|---|---|
| 5x | +1.4 |
| 12x (20-core parallel DFS) | +2.2 |
| 60x (tuned + parallel) | +3.5 |
| 306x (anagram collapsing) | +4.9 |
| 1000x | +6.0 |

This is the single most useful number in the document. It says that *throwing
hardware and hand-optimisation at this problem buys three or four letters, and
nothing more* — which is why the algorithmic levers (collapsing, forcing, and a
pruning bound) are where the value is.

Throughout: **(M)** = measured, **(P)** = projected from measured scaling,
**(U)** = mechanism understood, magnitude unmeasured.

---

## 1. Where we are today

Current `find-anagrams`, best-first over the trie, `-m 4 -c`:

| | |
|---|---|
| Memory | 4.2 GB at 14 letters / 30 s; 8.7 GB at 19 letters / 30 s, still climbing **(M)** |
| Growth rate | ~290 MB/s of frontier, essentially independent of bag size **(M)** |
| Practical ceiling | **~30–50 seconds of search on a 15 GB machine, then OOM** |
| Completes? | **Never.** It streams the best few results and dies. |

The important reframe: the current tool's limit is *not really a bag length*.
It is a **wall-clock budget of ~40 seconds**, because the frontier grows at a
fixed byte rate regardless of what you ask it. Short bags finish inside that
budget and long ones don't, which is what "caps out around 20 letters" is
actually describing.

Everything below replaces the frontier with a depth-first stack, which removes
this wall entirely and converts the problem into a pure time budget.

---

## 2. Approach A — extract words from the trie, then ancc's DFS

*(Design A + D in `ancc-inspiration.md`. These are one thing in practice: the
collapsing that makes A fast only works on a word list, so A without D is not
worth building.)*

### Here's the win

- **Memory stops mattering.** Peak RSS becomes **flat at a couple hundred MB**
  regardless of bag length, runtime, or how many solutions come back. Measured
  93 MB / 177 MB / 220 MB at 14 / 19 / 21 letters **(M)**, against 8.7 GB and
  climbing for the current tool at 19.
- **It actually finishes.** 19 letters, ≤4 words, exhaustive: **3.25 seconds**,
  156,138 distinct solutions. 21 letters: **37 seconds**, 1,474,843 solutions
  **(M)**. The current tool returns a handful of results at those sizes and then
  dies without ever completing.
- **~5 letters of extra reach from collapsing alone**, and roughly **+6–7
  letters overall** versus today.

### The cost

- **Loses contiguous corpus phrases.** `pen built` scores 7 today as a real
  phrase found in one trie descent; as two independent words it scores 2.1e-05.
  A word-list search cannot see phrases at all. This is recoverable — see §5 —
  but the recovery has an unmeasured price.
- **Needs `-x/--max-words`, which does not exist.** Every measurement above used
  ≤4 words. That cap is load-bearing: at 26 letters with `-m 4` the uncapped
  search is 6 words deep, and two extra levels at a branching factor in the
  thousands is not a rounding error. Build the flag first
  (`findings/reduce-permutations.md` §5).

### Details

As in `ancc-inspiration.md` §"Designs, ranked / A", with two additions:

**Phase 1 is free and is not the bottleneck.** 0.78 s to pull 144,103 words for
a 21-letter bag, 1.30 s for all 26 letters **(M)**. The bag filter prunes the
1.2 GB index to 313k visited nodes. Phase 2 dominates at every size above ~14
letters.

**The 220 MB is an artifact, not a floor.** It is phase 1's word list stored
carelessly (`std::string` + a 26-byte count array per word) plus touched mmap
pages. Packed ancc-style — a 16-byte mask array plus inline characters — 144k
words is **~5 MB**, and the entire a–z-makeable dictionary (707,803 words) is
**~23 MB** **(P)**. The DFS itself is O(depth): kilobytes. So the real
algorithmic memory for A is:

```
word list (≤ ~50 MB, bounded by the dictionary, NOT by bag size)
  + DFS stack (kilobytes)
  + top-N heap (N × ~48 bytes)
```

### Collapsing does not discard spellings

Worth stating explicitly because it is easy to read the other way: **anagram
collapsing is a search-time optimisation only. Every spelling still comes out.**

If `yacht` and `cathy` are both in the word list, they share one letter multiset
and the DFS explores that branch **once** instead of twice — that is the 306x.
At output time the class is re-expanded and **both** are emitted. This is
literally what ancc does: `print()` (`ancc.cc:1109-1118`) walks each slot of a
found solution and, while the next dictionary word is flagged `BIG_INT` (the
"is an anagram of the previous word" marker set at `ancc.cc:687`), advances that
slot and recurses — enumerating the full cross-product of class members. Nothing
is dropped and no member is preferred.

The one place a class member *is* preferred is search-time pruning: a class needs
a single score for the `h` bound (§7) to compare against, and that score must be
the **maximum** count over its members to stay admissible. So the best member
decides whether a branch is worth *exploring*; it does not decide what gets
*printed*. Exact per-member scores are computed at expansion.

---

## 3. Approach B — depth-first driver over the trie

### Here's the win

- **Same memory fix as A, and cheaper**: O(depth) only — kilobytes, not
  megabytes, since there is no word list. Deletes `nexts`, `crumbs`, and the
  whole of `collect()`.
- **Loses nothing.** Contiguous phrases, exact scoring, `-m`/`-c`/`-u` semantics
  all survive untouched. It is the minimum change that fixes the stated problem.

### The cost

- **It cannot use collapsing**, which is the 306x. The trie is indexed by
  spelling; collapsing requires grouping by sorted letters. Estimated
  **100–1000x more nodes than A** for the same bag **(U)** — the collapse ratio
  compounded over the search depth, partly offset by the trie's prefix sharing.
- In bag-length terms that is roughly **A's reach minus 5 letters**. B replaces
  the memory wall with a time wall about 5 letters lower down.

### Details

See `ancc-inspiration.md` §"Designs, ranked / B". `IndexWalker` in
`source/index.h` is the existing template. The one real subtlety is the caveat
on deleting `seen`: it also dedups different *segmentations* of the same word
set (`{pen built}` vs `{pen}{built}`), keeping the better-scoring one, which
canonical ordering does not subsume. Keep it — it is O(matches), not
O(frontier).

**Positioning:** B is not a competitor to A, it is a different product. B is
"today's tool, exhaustive, in flat memory, to ~20 letters". A is "a different
and weaker scoring model, to ~26–30 letters". The original document's suggestion
to build both as modes is right.

---

## 4. Approach C — rarest-letter forcing in `AnagramFilter`

### Here's the win

Honestly: **as a standalone change to the current tool, unquantified and
probably modest.** Its value is as an enabler.

- It costs **one bit** of `AnagramFilter` state — a ×2 on a state space that has
  ~2^26 of headroom against `INT_MAX`. Effectively free.
- Its fail-first effect shrinks the best-first frontier by an unmeasured
  factor **(U)**. Real, but it does not change the memory asymptotics — the
  frontier still grows without bound, just more slowly. This will not move the
  ~40-second wall by much.
- **It is already baked into approach A's measured numbers.** The prototype used
  forced-letter DFS throughout. C is not additive on top of A's 3.25 s / 37 s;
  it is a prerequisite for them.

### The cost

- **C and `-c` conflict and will silently drop solutions if combined naively.**
  Fix per `ancc-inspiration.md` §C: reset `last_seg` to 0 whenever the forced
  letter advances, so non-decreasing node order binds only among segments
  covering the same forced letter. (Whether the conflict is reachable in
  practice was reasoned on paper, not observed.)

### Details

See `ancc-inspiration.md` §"Designs, ranked / C" — the invariant that makes it
need no stored state, and the `entry_point` tie-break, are both there.

**Under the relaxed streaming requirement, C drops in priority.** The original
document listed it first because it improved the existing best-first driver
cheaply. If the plan is to replace that driver with a DFS, C is polish on the
component being replaced. Build it as part of A or B, not before them.

---

## 5. Recovering contiguous phrases — **required, not optional**

In-corpus word adjacency is a stated requirement: `pen built` scoring 7 as a real
corpus phrase, rather than 2.1e-05 as two independent words, is a feature that
has to survive. That makes this section a **precondition on approach A**, not an
enhancement, and it makes the F measurement below a blocking unknown rather than
a nice-to-have. If F turns out to be bad, **approach B is the answer**, because B
preserves adjacency natively with nothing to measure.

### Here's the win

Makes A a strict superset of today's tool instead of a trade. Phase 1 continues
past the first space and emits multi-segment strings as additional "words"
carrying their true phrase count; phase 2 then finds `pen built` with no special
handling at all.

### The cost — and this is the one thing worth measuring before committing

The original document asserts the cost is "bounded by the bag filter". That
understates it. **Phase-2 node count scales as F^depth**, where F is the growth
in the number of distinct *multiset classes*, not the raw list:

| Class growth F | Node cost at depth 4 | Letters lost |
|---|---|---|
| 1.2x | 2x | -0.6 |
| 1.5x | 5x | -1.4 |
| 3x | 81x | -3.8 |

So this is either nearly free or it eats most of the collapsing win, and which
one is an empirical question nobody has answered.

**Cheap mitigation:** only keep a phrase if being contiguous actually *helps* —
i.e. `count(phrase) > count(w1) × count(w2) × restart_penalty`. A phrase that
does not beat its own words scored separately adds a class for nothing. This
should prune the phrase list hard, and it is a one-line test in phase 1.

---

## 6. Getting ranked output without in-order streaming

**Correction to an earlier draft of this document, which suggested emitting one
best-scoring spelling per solution.** That would have discarded `cathy` in favour
of `yacht`, and it is withdrawn. Every spelling is preserved below. See §2,
"Collapsing does not discard spellings", for why the 306x itself was never the
lossy part.

### The volume problem, stated honestly

A solution is a set of k multiset classes; its spellings are the full
cross-product of the class members. Mean class size is the collapse ratio — 5.8
at 19 letters, ~5.3 at 21 **(M)** — so a 4-word solution expands ~10^3-fold:

| Letters | Solutions | All spellings **(P)** | As a file @ ~40 B/line |
|---|---|---|---|
| 14 | 1,315 **(M)** | ~450k | 18 MB |
| 19 | 156,138 **(M)** | ~1.8e8 | **~7 GB** |
| 21 | 1.47M **(M)** | ~1.2e9 | **~46 GB** |

So **"write everything to a file, then filter" only survives to ~16–17 letters
once all spellings are kept** — not the ~22–23 an earlier version of this
document claimed, which assumed the lossy collapse. Above that the expansion has
to stay lazy.

### The fix: lazy exact expansion, nothing discarded

Sort each class's members by count descending, once, in phase 1. Then a
solution's spellings can be generated **in descending score order on demand**
(the standard k-best-combination heap over member-index tuples). So:

- Keep a bounded top-N heap over *spellings*.
- On finding a solution, generate its spellings in descending order and **stop at
  the first one below the current N-th best**.

Typically that emits one or two spellings per solution and touches no others, yet
the result is **exactly** the true top N over the full 10^9 — every spelling is
reachable, none is preferred a priori, and memory stays at N × ~48 bytes
(N = 10,000 → 0.5 MB; N = 1,000,000 → 48 MB).

### Two output modes, because they answer different questions

Top-N over spellings is exact but can fill with near-duplicates — 1,000 rows
might be 1,000 rearrangements of three good word sets. The mode a human usually
wants is:

- **top-N solutions, each fully expanded** — the best N word sets, with *every*
  spelling of each listed underneath. 1,000 solutions × ~10^3 spellings ≈ 1M
  lines, entirely manageable, and it is the presentation that makes `yacht` and
  `cathy` both visible as alternatives for the same slot rather than competing
  rows.
- **top-N spellings** — exact global ranking, for when you want a flat list.

Both run off the same lazy expansion; they differ only in when the heap cuts off.

**The intermediate-file plan still works** — it is just a file of *solutions*
(60 MB at 21 letters, per the counts above), with expansion happening at filter
time rather than search time. That keeps its real advantage: re-filtering with a
different N or different criteria without re-running the search.

---

## 7. The pruning bound (`h`)

### Here's the win

**The only lever that attacks the 3.2x-per-letter exponent instead of the
constant.** Everything else in this document shifts the curve; a good admissible
bound bends it, and its value grows with bag size.

Under the relaxed requirement it also gets much simpler than the original
document's version. The IDA\*/threshold-restart machinery existed to produce
*ordered streaming*. Without that requirement it reduces to: **carry `g` = the
accumulated log-score, prune when `g + h(bag) < floor`, where `floor` is the
current N-th best in the heap.** That is standard branch-and-bound and it is
about ten lines.

### The cost

Magnitude is entirely a function of how tight `h` is, and that is unmeasured
**(U)**. A loose bound prunes nothing. Worth a prototype specifically because
the payoff is unbounded where everything else is capped at ~4 letters.

### Details

The admissible `h` is in `ancc-inspiration.md` §"Keeping what matters" — per-letter
best-rate, `h(bag) = Σ_{c ∈ bag} best_rate_containing[c]`. Note this is the same
function as idea #3 in `anagram-perf.md`, which that document calls its highest-
leverage untouched item.

---

## 8. How far can each combination actually go?

Extrapolated from the measured prototype at **3.2x nodes/letter** and **1.54M
nodes/s**, at `-m 4 -x 4`. All memory figures are *flat* — they do not grow with
runtime or solution count.

### Time to exhaust a bag

| Letters | A, prototype 1-thread | A, tuned + 20 cores (60x) **(P)** | B, tuned + 20 cores **(P)** |
|---|---|---|---|
| 19 | 3.3 s **(M)** | 0.05 s | 15 s |
| 21 | 37 s **(M)** | 0.6 s | 3 min |
| 23 | 6 min | 6 s | 30 min |
| 25 | 1 hr | 1 min | 5 hr |
| 26 | 3.4 hr | 3.4 min | 17 hr |
| 28 | 35 hr | 35 min | — |
| 30 | 15 days | 6 hr | — |

The 60x is 5x from a tuned inner loop (ancc's bitmask stack and length-sorted
skip index, versus a throwaway prototype) times 12x from splitting the
depth-1 branches across 20 cores. DFS parallelises almost perfectly here —
independent subtrees, no shared state, and the only synchronisation is the
top-N heap.

### Practical ceilings, at a ~10-minute budget

| Configuration | Max bag | Peak memory |
|---|---|---|
| **Today** (best-first, trie) | ~19–20 letters, and *never completes* — best-effort results only | **8.7 GB and climbing; OOM at ~40 s (M)** |
| **B** — DFS over trie | ~17–19 letters, exhaustive **(P)** | **< 10 MB** (DFS stack + `seen`) |
| **B** + tuned + 20 cores | ~21–23 letters **(P)** | < 10 MB + `seen` |
| **A** — word list + collapsing, 1 thread | ~23 letters **(M/P)** | **~220 MB measured, ~50 MB packed** |
| **A** + tuned + 20 cores | ~26–27 letters **(P)** | ~50 MB + heap |
| **A** + tuned + parallel + a tight `h` | 30+ letters **(U)** | ~50 MB + heap |

Adjacency is required (§5), so only these last two rows are actually eligible —
and only with phrase recovery included:

| Configuration | Max bag | Peak memory |
|---|---|---|
| **A + phrase recovery**, tuned + 20 cores | **~23–26 letters**, depending entirely on F **(U)** | ~100–300 MB |
| **B**, tuned + 20 cores | **~21–23 letters**, no unknowns **(P)** | < 10 MB + `seen` |

That is the real choice: A-with-phrases is worth roughly **+2 to +4 letters over
B**, but only if F lands at the low end, and F is unmeasured. B's number is the
safe one.

### The two conclusions worth taking away

**Memory ceases to be the constraint under any of these designs.** Today's 8.7 GB
is a frontier; a DFS has no frontier. Every row above sits between 10 MB and
300 MB and stays there no matter how long the search runs or how many solutions
it finds. The design question stops being "will it OOM" and becomes "how long am
I willing to wait".

**Beyond that, bag length is bought at 3.2x per letter, so the levers rank by
their multiplier and nothing else.** Anagram collapsing (+4.9 letters) is worth
more than a completely tuned 20-core implementation (+3.5). The pruning bound is
the only item with no ceiling. Everything else is a constant factor wearing a
large number.

---

## 9. Revised order of work

Changed from `ancc-inspiration.md` on two counts: in-order streaming is no longer
required (which removes the reason C came first, and removes IDA\* entirely), and
**word adjacency is required** (which moves the phrase-recovery measurement to
the front, because it decides the architecture).

1. **`-x/--max-words`.** Prerequisite for reproducing any measurement here, and
   independently useful today. `findings/reduce-permutations.md` §5.
2. **Measure F — the phrase-recovery cost (§5).** Extend the throwaway phase-1
   extractor to walk past the first space, apply the
   `count(phrase) > count(w1) × count(w2) × restart` filter, and count the
   resulting **multiset classes** against the word-only baseline (95,629 words at
   19 letters, so ~16.5k classes at the measured 5.8x ratio). One afternoon of
   prototype work, and it decides
   whether the next item is A or B. Do not build either first.
3. **Then whichever F selects:**
   - *F ≲ 1.5* → **approach A** with phrase extraction: phase-1 extraction,
     ancc's DFS with rarest-letter forcing (C) and anagram collapsing (D), lazy
     spelling expansion (§6), bounded top-N heap. Reaches ~23–26 letters.
   - *F ≳ 2* → **approach B**: DFS driver over the trie, deleting `nexts`,
     `crumbs`, and `collect()`. Keeps every current feature, reaches ~21–23
     letters, no unknowns.
4. **The `h` bound (§7)** — small, pays off in both designs, and the only lever
   with no ceiling.
5. **Parallelise the DFS.** Mechanical, ~12x, do it last since it changes no
   semantics.

Note that B is now a perfectly respectable destination rather than a fallback:
it gives up ~2–4 letters versus the best case for A, needs no measurement to
de-risk, preserves adjacency and exact scoring by construction, and is a smaller
change to code that already exists.

---

## What is measured vs. projected

Measured on `~/code/nutrimatic/idx/wiki-merged.5.index`: everything in §1, the
93/177/220 MB and 0.03/3.25/37.2 s figures, the 306x, the collapse ratios, the
solution and node counts, the 3.2x/letter and 3.07x/letter scaling constants,
and the zero-duplicate result at 12 letters.

Projected or unmeasured: the 5x tuning factor, the 12x parallel factor, B's
100–1000x penalty, the phrase-recovery cost F, the value of `h`, C's standalone
benefit, the spelling-expansion volumes in §6, and every row of §8 marked (P) or
(U). The prototype was throwaway code; its spelling-expansion counts over-count
by ~10% (node and solution counts are exact), and the score model was not
verified against `SearchDriver::step()`'s restart constants.

Corrected in this document after an earlier draft: §6 originally proposed
emitting one best-scoring spelling per solution, which would silently drop
`cathy` in favour of `yacht`. Withdrawn and replaced with lazy exact expansion.
The output-volume limit for the write-to-a-file plan moved from ~22–23 letters
to ~16–17 as a result. §5 and §9 were reordered once adjacency was stated as a
requirement rather than a nice-to-have.

## Related

- `findings/ancc-inspiration.md` — full reasoning, ancc's technique breakdown
  (T1–T7), and the designs in detail. This document is a digest of it.
- `findings/anagram-perf.md` — idea #3 is the `h` bound; idea #7 is approach A.
- `findings/reduce-permutations.md` — `-x/--max-words`, and why `-c` orders by
  trie node.
- `findings/priority-invariant.md` — the pop-order invariant `seen` rests on,
  which A and B make moot.
