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
today ~19–20 letters and it never completes; approach B ~15–16 (~18–20 tuned and
parallel); approach A ~20–21 (~23–24 tuned and parallel). **These are ~3 letters
below an earlier draft: the prototype's counts undercounted the real exhaustive
search by ~27x at 19 letters (68M nodes / 89 s, not 5.5M / 3.25 s), so §8's
projections are re-anchored — `phrase-recovery-cost.md` §3.5.**

**Anagram collapsing does not discard spellings (§2).** The 306x is a *search-time*
optimisation: the class `{yacht, cathy}` is explored as one branch instead of two,
then re-expanded at output so both are emitted. ancc does exactly this
(`print()`, `ancc.cc:1109-1118`). The only place a class member is preferred is
the pruning bound, which must use the class **maximum** to stay admissible — the
best member decides whether a branch is *explored*, never what is *printed*.

**The stream-then-filter plan works, up to ~13–14 letters if all spellings are
written (§6).** Each solution expands ~10^3-fold into spellings, and with the
corrected solution counts (29.3M at 19 letters, not 156,138) 19 letters is ~1.3 TB
of output. The fix is lazy exact expansion: generate each solution's spellings in
descending score order and stop at the current N-th best. That yields the true
top N over the full ~10^10 while touching one or two spellings per solution,
discards nothing, and keeps the intermediate file useful as a file of *solutions*
— though that file is now much larger than the earlier 60 MB estimate implied.

**In-corpus word adjacency is a requirement, which promoted §5 from optional to
blocking — and §5 has now been measured.** Approach A only preserves adjacency if
phase 1 extracts contiguous phrases, and that costs F^depth in the class-count
growth F. **F is 1.12–1.80 across 14–26 letters (M), and F^depth is a ceiling
rather than an estimate**, because the classes phrases add are all ≥ `2*min_len`
letters and so cannot be picked at the depths where a four-word solution lives.
**That selects approach A.** `findings/phrase-recovery-cost.md`.

**The relaxed streaming requirement changed the order of work (§9).** It removes
the reason design C came first and deletes the IDA\*/threshold-restart machinery
entirely, reducing the whole ranking story to a top-N heap plus about ten lines of
branch-and-bound.

**The two weakest parts of this analysis**, stated up front rather than buried:

- ~~**The phrase-recovery cost is worse than `ancc-inspiration.md` implies.**~~
  **Resolved, and it landed nearer that document's position than this one's.**
  It asserted "bounded by the bag filter"; this document countered with F^depth
  for an unmeasured F. Measured, F is 1.12–1.80 and lands at the *cheap* end of
  the bracket §5 drew — and the F^depth framing itself proved too pessimistic,
  since phrase classes are all long and unavailable at depth. The cheap
  mitigation §5 proposed turned out to be a tautology, for a reason worth
  keeping: its formula was missing a `/total` per restart, which is 9 orders of
  magnitude. See `findings/phrase-recovery-cost.md`.
- **Design C's standalone value is unquantified**, and is left that way here
  rather than given a number. Its measured benefit is already baked into
  approach A's runtime (89 s at 19 letters, corrected) — the prototype used
  forced-letter DFS throughout — so it is a prerequisite, not an additive win.

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

**2. Constant-factor wins buy bag length logarithmically.** Node count grows
roughly **3.2x per additional letter** (the original 19→21 basis, 5.5M → 55.9M
nodes, came from the undercounting prototype and has **not** been re-measured —
see §8's correction note; the slope is retained as a working assumption). So any
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
- **It actually finishes.** 19 letters, ≤4 words, exhaustive: **89 seconds**,
  **29.3M distinct solutions**, on the untuned single-thread DFS **(M, corrected
  — the earlier 3.25 s / 156,138 figures were a buggy prototype undercount;
  `phrase-recovery-cost.md` §3.5)**. The current tool returns a handful of
  results at that size and then dies without ever completing.
- **~5 letters of extra reach from collapsing alone**, and roughly **+6–7
  letters overall** versus today.

### The cost

- **Loses contiguous corpus phrases.** `pen built` scores 7 today as a real
  phrase found in one trie descent; as two independent words it scores 2.1e-05.
  A word-list search cannot see phrases at all. This is recoverable — see §5 —
  and the recovery has now been priced: F = 1.12–1.80 in class count, ≤ 1.3
  letters of reach at 19 letters and probably much less **(M)**.
- **Needs a cap on the word count.** Every measurement above used ≤4 words. That
  cap is load-bearing: at 26 letters with `-m 4` the uncapped search is 6 words
  deep, and two extra levels at a branching factor in the thousands is not a
  rounding error. `find-anagrams` now derives one from `-m` rather than taking a
  flag (§9 item 1) — but the derived cap at 26 letters is 6, not 4, so
  reproducing the 26-letter rows still needs something else.

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
  forced-letter DFS throughout. C is not additive on top of A's runtime (89 s at
  19 letters, corrected); it is a prerequisite for it.

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
enhancement, and it made the F measurement below the blocking unknown the whole
work order turned on.

> **F has since been measured: 1.12–1.80 across 14–26 letters, and the `F^depth`
> model below is an upper bound that the bag's geometry works hard against. It
> selects approach A.** Full results, method, and three corrections to what this
> section assumed are in `findings/phrase-recovery-cost.md`; the summary is
> folded into "The cost" below.

### Here's the win

Makes A a strict superset of today's tool instead of a trade. Phase 1 continues
past the first space and emits multi-segment strings as additional "words"
carrying their true phrase count; phase 2 then finds `pen built` with no special
handling at all.

### The cost — measured

The original document asserts the cost is "bounded by the bag filter". The model
that replaced that claim here was **phase-2 node count scaling as F^depth**,
where F is the growth in the number of distinct *multiset classes*, not the raw
list. Measured **(M)**, at `-m 4`:

| Bag | Word classes | + phrases | F | F^4 | Letters lost |
|---|---|---|---|---|---|
| 14 letters | 2,458 | 2,760 | **1.123** | 1.6x | -0.4 |
| 19 letters | 16,396 | 23,735 | **1.448** | 4.4x | -1.3 |
| 21 letters | 22,811 | 36,053 | **1.581** | 6.2x | -1.6 |
| 23 letters | 38,900 | 70,030 | **1.800** | 10.5x | -2.0 |
| 26 letters | 229,222 | 378,178 | **1.650** | 7.4x | -1.7 |

That sits in the lower half of the 1.2x–3x range this section originally
bracketed — nearer "nearly free" than "eats most of the collapsing win". The
last two columns then overstate even that, for two reasons:

**F does not compound with depth.** All of it arrives at two-word phrases: 19
letters goes 16,396 → 23,604 classes at a two-word cap and then 23,735 and flat
forever. Three-word contiguous phrases whose every word is ≥ 4 letters are
almost nonexistent in the corpus — 239 extra entries at 19 letters, 2,466 at 26.

**F does not apply at every level.** Phrases add *exactly zero* classes at 4–7
letters, which is structural rather than a corpus accident: two words of ≥
`min_len` is ≥ `2*min_len` letters. The multiplier at 19 letters is 1.00x through
length 7, 1.21x at 8, 2.30x at 10, and 33.8x at 14. A four-word 19-letter
solution averages 4.75 letters per word — squarely where F is 1.0 — so the added
classes can only be picked at depth 1–2, and picking one consumes 8–17 letters
and shortens the remaining search. `F^depth` assumes availability at every level
and is therefore a ceiling, not an estimate.

**The proposed mitigation is vacuous.** It was: keep a phrase only if being
contiguous actually helps, `count(phrase) > count(w1) × count(w2) × restart`.
That formula drops a `/total` per restart — from `SearchDriver::step()`, k
segments score `prod(count(w_i)) × (restart / total)^(k-1)`, so each split costs
~15 orders of magnitude against a corpus total of 3.59e9, not 6. Against the
model the search actually implements, **every one of 462,730 phrases measured
passes**, by 6.7–7.3 orders of magnitude; the closest is 2.9 and only two
anywhere are within three. There is no phrase list to prune, and F as measured
is F.

**Multi-segment extraction is cheap**, closing the cost `ancc-inspiration.md`
left unverified: walking past the space costs 3.9x the nodes and 2.4x the time of
the word-only walk, and all 26 letters finish in under 3 s.

**Now closed:** the class-length distribution has been turned into an actual
phase-2 node count via the ancc DFS over the class list (Phase 0 of
`plans/dfs.md`). At 19 letters, operational depth, the phrase cost is **1.04x
nodes** — below even the "much closer to 1.0x than 4.4x" this predicted, and it
*shrinks* with depth (1.76x/1.25x/1.04x at caps 2/3/4) because phrase classes are
too long to place deep in the search. See `findings/phrase-recovery-cost.md` §3.5.

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
| 14 | 27,177 **(M, corrected)** | ~9M | ~370 MB |
| 19 | 29.3M **(M, corrected)** | ~3.4e10 | **~1.3 TB** |

The solution counts here are corrected from the buggy-prototype figures (1,315 /
156,138) to the real exhaustive counts — see `phrase-recovery-cost.md` §3.5. The
21-letter row is dropped rather than re-measured. So **"write everything to a
file, then filter" only survives to ~13–14 letters once all spellings are kept**
— even lower than the ~16–17 an earlier draft claimed, and far below the ~22–23
before that (which assumed the lossy collapse). Above that the expansion has to
stay lazy — the correction makes lazy exact expansion more necessary, not less.

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

> **Corrected (Phase 0 of `plans/dfs.md`).** The prototype figures this section
> was first built on — 156,138 solutions / 5.49M nodes / 3.25 s at 19 letters —
> were a **~12–27x undercount**. The real exhaustive words-only search at 19
> letters is **29.3M solutions, 68.2M nodes, 89 s** on the untuned single-thread
> DFS, verified four ways against `ancc` itself (`phrase-recovery-cost.md` §3.5).
> The A-column times below are re-anchored to that 89 s; the ceilings drop by
> `log(89/3.25)/log(3.2) ≈ 2.9 letters` versus the earlier draft. The
> **conclusions are unchanged** — memory stays flat, the levers rank the same —
> only the absolute reach is ~3 letters shorter than previously claimed.

Extrapolated from the corrected prototype anchor (**89 s / 68.2M nodes at 19
letters, 1 thread**) and an assumed **3.2x nodes/letter** slope, at `-m 4` and a
hard cap of 4 words. The slope itself was **not** re-measured — its original
19→21 basis came from the same undercounting prototype — so the per-letter
extrapolation is now an assumption, not a measurement; only the 19-letter anchor
is solid. All memory figures are *flat* — they do not grow with runtime or
solution count. Every A row is words-only: the measured phrase cost is negligible
at solution depth 4 (1.04x nodes, §5 / `phrase-recovery-cost.md` §3.5).

### Time to exhaust a bag

| Letters | A, prototype 1-thread | A, tuned + 20 cores (60x) **(P)** | B, tuned + 20 cores **(P/U)** |
|---|---|---|---|
| 19 | **89 s (M)** | 1.5 s | ~7 min |
| 21 | 15 min **(P)** | 15 s | ~80 min |
| 23 | 2.6 hr **(P)** | 2.6 min | ~14 hr |
| 25 | 27 hr **(P)** | 27 min | ~6 days |
| 26 | 3.5 days **(P)** | 85 min | ~19 days |
| 28 | 36 days **(P)** | 14 hr | — |
| 30 | ~1 yr **(P)** | 6 days | — |

Only the 19-letter A row is measured; every other cell is `89 s × 3.2^(letters−19)`
under the assumed slope (÷60 for the tuned+parallel column). The 60x is 5x from a
tuned inner loop (ancc's bitmask stack and length-sorted skip index, versus this
throwaway prototype) times 12x from splitting the depth-1 branches across 20
cores. DFS parallelises almost perfectly here — independent subtrees, no shared
state, and the only synchronisation is the top-N heap. The B column is the same
re-anchoring applied to the earlier (U) B estimate and carries all its old
uncertainty plus the assumed slope.

### Practical ceilings, at a ~10-minute budget

| Configuration | Max bag | Peak memory |
|---|---|---|
| **Today** (best-first, trie) | ~19–20 letters, and *never completes* — best-effort results only | **8.7 GB and climbing; OOM at ~40 s (M)** |
| **B** — DFS over trie | ~14–16 letters, exhaustive **(P/U)** | **< 10 MB** (DFS stack + `seen`) |
| **B** + tuned + 20 cores | ~18–20 letters **(P/U)** | < 10 MB + `seen` |
| **A** — word list + collapsing, 1 thread | ~20–21 letters **(M/P)** | **~220 MB measured, ~50 MB packed** |
| **A** + tuned + 20 cores | ~23–24 letters **(P)** | ~50 MB + heap |
| **A** + tuned + parallel + a tight `h` | 27+ letters **(U)** | ~50 MB + heap |

Adjacency is required (§5), so only these last two rows are actually eligible —
and only with phrase recovery included:

| Configuration | Max bag | Peak memory |
|---|---|---|
| **A + phrase recovery**, tuned + 20 cores | **~23–24 letters**, F measured at 1.04x nodes **(P)** | ~100–300 MB |
| **B**, tuned + 20 cores | **~18–20 letters (P/U)** | < 10 MB + `seen` |

That is the real choice: A-with-phrases is worth roughly **+3 to +5 letters over
B**. Phrase recovery costs essentially nothing at solution depth 4 (1.04x nodes,
`phrase-recovery-cost.md` §3.5), so the A+phrase row is at the top of the
words-only A range, not below it. Both rows are ~3 letters lower than the earlier
draft's claim because that draft's 19-letter anchor was a 27x undercount; the
*gap* between A and B is unchanged.

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
**word adjacency is required** (which moved the phrase-recovery measurement to
the front, because it decided the architecture).

1. ~~**`-x/--max-words`.**~~ **Done**, though not as a flag. `-m` now defaults to
   4 and `find-anagrams` derives the cap as `floor(letters / min_word_len)`,
   which is exactly the bound `AnagramFilter` already enforces — every word
   spends at least `min_word_len`, so a path that has finished k words has spent
   k·`min_word_len`, and a separate counter would prune at the same depth. The
   number is reported on the `#` line beside the progress output. Caveat that
   matters for reproducing §8: the derived cap is **6 at 26 letters, not 4**, so
   a hard 4 there still needs an independent cap.
2. ~~**Measure F — the phrase-recovery cost (§5).**~~ **Done: F = 1.12–1.80,
   which selects approach A.** `source/measure-f.cpp`; results and method in
   `findings/phrase-recovery-cost.md`. Its word-only pass reproduces every
   phase-1 figure in `ancc-inspiration.md` exactly, so the extractor is
   trustworthy, and the score model is now verified against
   `SearchDriver::step()` rather than assumed.
3. **Approach A**, which F selects: phase-1 extraction — `source/measure-f.cpp`
   is a working prototype of it, phrases included — plus ancc's DFS with
   rarest-letter forcing (C) and anagram collapsing (D), lazy spelling expansion
   (§6), and a bounded top-N heap. Reaches ~23–24 letters (§8, corrected).

   ~~**Do the phase-2 node count first.**~~ **Done (Phase 0 of `plans/dfs.md`).**
   The DFS is built (`CollapseDFS` in `source/measure-f.cpp`) and the phrase cost
   is measured: **1.04x nodes at 19 letters**, well under the >6x "reconsider B"
   gate, so A proceeds. The "known target" this was meant to check against
   (5,488,296 nodes / 156,138 solutions) turned out to be a **buggy-prototype
   undercount** — the real counts are 68.2M nodes / 29.3M solutions, verified
   four ways against `ancc` itself (`phrase-recovery-cost.md` §3.5). §8's ceilings
   were re-anchored to the corrected figures.
4. **The `h` bound (§7)** — small, pays off in both designs, and the only lever
   with no ceiling.
5. **Parallelise the DFS.** Mechanical, ~12x, do it last since it changes no
   semantics.

Note that B remains a perfectly respectable destination rather than a fallback:
it gives up ~2–4 letters versus the best case for A, needs no measurement to
de-risk, preserves adjacency and exact scoring by construction, and is a smaller
change to code that already exists. What F settles is that A is no longer
*blocked* — not that B was a bad idea.

---

## What is measured vs. projected

Measured on `~/code/nutrimatic/idx/wiki-merged.5.index`: everything in §1, the
93/177/220 MB figures, the collapse ratios, the zero-duplicate property at 12
letters, and — since this document was first written — **§5's F, the class-length
distribution behind it, the multi-segment extraction cost, and (Phase 0 of
`plans/dfs.md`) the corrected phase-2 node/solution counts: 27,177 sol / 53,084
nodes at 14 letters, 29.3M sol / 68.2M nodes / 89 s at 19 letters, verified four
ways against `ancc`** (`phrase-recovery-cost.md` §3.5).

**Corrected, not measured as first stated:** the old 0.03/3.25/37.2 s timings and
the 156,138 / 1.47M / 5.49M solution-and-node counts were a buggy-prototype
undercount (~12–27x at 19 letters); the 3.2x/letter and 3.07x/letter scaling
constants rested on those and are now working assumptions, not measurements; the
306x is a ratio of those same undercounted node counts (the underlying ~7x/level
collapse *ratio* still holds, being a branching-factor property).

Projected or unmeasured: the 5x tuning factor, the 12x parallel factor, B's
100–1000x penalty, the value of `h`, C's standalone benefit, the
spelling-expansion volumes in §6, every row of §8 marked (P) or (U), and the
per-letter slope now that its prototype basis is retracted.

Two caveats on the original prototype no longer apply in full: its
spelling-expansion counts still over-count by ~10% (node and solution counts are
exact), but the score model *has* now been verified against
`SearchDriver::step()`'s restart constants — the check is what exposed the
missing `/total` in §5's mitigation, and it reproduces `pen built` at 7
contiguous versus 2.147e-05 split.

Corrected after measurement: §5's cost table and mitigation, and §9's first two
items, both of which are now done — see the strikethroughs there for what
changed and why.

Corrected in this document after an earlier draft: §6 originally proposed
emitting one best-scoring spelling per solution, which would silently drop
`cathy` in favour of `yacht`. Withdrawn and replaced with lazy exact expansion.
The output-volume limit for the write-to-a-file plan moved from ~22–23 letters
to ~16–17 as a result. §5 and §9 were reordered once adjacency was stated as a
requirement rather than a nice-to-have.

## Related

- `findings/ancc-inspiration.md` — full reasoning, ancc's technique breakdown
  (T1–T7), and the designs in detail. This document is a digest of it.
- `findings/phrase-recovery-cost.md` — §5's F, measured: the tool, the bags, the
  class-length distribution, and the three things §5 assumed that turned out not
  to hold.
- `findings/anagram-perf.md` — idea #3 is the `h` bound; idea #7 is approach A.
- `findings/reduce-permutations.md` — `-x/--max-words`, and why `-c` orders by
  trie node.
- `findings/priority-invariant.md` — the pop-order invariant `seen` rests on,
  which A and B make moot.
