# Measuring F — what contiguous phrases cost approach A

`findings/ancc-inspiration-summary.md` §9 item 2 calls this the blocking unknown:
approach A must keep contiguous corpus phrases (`pen built` scoring 7 as one
segment, not 2.1e-05 as two words), phase 1 therefore has to emit multi-word
strings as entries, and those entries add letter-multiset classes. **F** is the
factor by which they grow the class count. §5 asserts the phase-2 cost is
`F^depth` and gives the decision rule: F ≲ 1.5 → build A, F ≳ 2 → build B.

Tool: `source/measure-f.cpp`, run against `idx/wiki-merged.5.index` at `-m 4`.
It extracts twice from the same bag — once stopping at the first space, once
walking past it — and reports both class counts. All figures below are **(M)**.

## The answer

**F = 1.12 to 1.80, and the `F^depth` model overstates the cost badly.**
Approach A survives this measurement.

| Bag | Words | Word classes | Phrase entries | All classes | **F** |
|---|---|---|---|---|---|
| 14 letters | 17,274 | 2,458 | 1,025 | 2,760 | **1.123** |
| 19 letters | 95,629 | 16,396 | 28,231 | 23,735 | **1.448** |
| 21 letters | 113,001 | 22,811 | 43,161 | 36,053 | **1.581** |
| 23 letters | 165,137 | 38,900 | 81,658 | 70,030 | **1.800** |
| 26 letters (`a`–`z`) | 707,803 | 229,222 | 308,655 | 378,178 | **1.650** |

Taken at face value that is "A, but it costs something": F^4 is 4.4x at 19
letters and 10.5x at 23, or 1.3–2.0 letters of reach at the summary's measured
3.2x nodes/letter. The three findings below say the real number is smaller than
that, and the last one says how much smaller is still unmeasured.

## 1. The extractor reproduces the original prototype exactly

Worth stating first, because everything else rests on it. The word-only pass
matches every phase-1 figure in `ancc-inspiration.md` on the nose:

| Bag | Words, this tool | Words, original | Collapse, this tool | Collapse, original |
|---|---|---|---|---|
| 14 letters | 17,274 | 17,274 | 7.03x | 7.0x |
| 19 letters | 95,629 | 95,629 | 5.83x | 5.8x |
| 26 letters | 707,803 | 707,803 | 3.09x | 3.1x |

The summary predicts "~16.5k classes at 19 letters"; measured, 16,396. The bags
are the ones the original results were spelled from — `feat studio tsen` and
`fire station team used`; the 21- and 23-letter rows above are extensions of the
19-letter bag, not the original 21-letter bag, which the documents do not record
(it had 144,103 words; this one has 113,001).

The score model is confirmed end to end. `measure-f -d` on a `penbuilt` bag
prints `phrase "pen built" scores 7 contiguous, 2.147e-05 split into 2
segments`, against the 7 and 2.1e-05 quoted in §5 and the `7.000 pen built` that
`find-anagrams` itself reports.

## 2. F is entirely a two-word effect — it does not compound with depth

Rerunning with the phrase length capped shows the whole of F arrives at `-x 2`:

| Cap | 19 letters | 26 letters |
|---|---|---|
| `-x 1` (words) | 16,396 | 229,222 |
| `-x 2` | 23,604 | 375,712 |
| `-x 3` | 23,735 | 378,175 |
| `-x 4` | 23,735 | 378,178 |
| `-x 5`, `-x 6` | 23,735 | 378,178 |

Three-word contiguous phrases whose every word is ≥ 4 letters are almost
nonexistent in the corpus: 239 extra entries at 19 letters, 2,466 at 26. So F is
a one-time constant on the entry list, not something that grows as the search is
allowed to go deeper.

## 3. The new classes are all long, so F does not apply at every level

This is the finding that undermines `F^depth`. Classes by letters used, 19
letters:

| Letters | Word classes | + phrases | Multiplier |
|---|---|---|---|
| 4 | 799 | 799 | **1.00x** |
| 5 | 1,692 | 1,692 | **1.00x** |
| 6 | 2,700 | 2,700 | **1.00x** |
| 7 | 3,408 | 3,408 | **1.00x** |
| 8 | 3,329 | 4,016 | 1.21x |
| 9 | 2,419 | 3,716 | 1.54x |
| 10 | 1,307 | 3,003 | 2.30x |
| 11 | 517 | 2,125 | 4.11x |
| 12 | 177 | 1,261 | 7.12x |
| 13 | 40 | 629 | 15.7x |
| 14 | 8 | 270 | 33.8x |
| 15–17 | 0 | 116 | — |

**Phrases add exactly zero classes at 4–7 letters**, and that is structural, not
a corpus accident: two words of ≥ `min_len` is ≥ `2*min_len` letters. Every bag
measured shows the same shape, including all 26 letters (1.00x through length 7,
1.46x at 8, 25.7x at 13).

`F^depth` assumes the extra classes are available at every level. They are not.
A 19-letter four-word solution averages 4.75 letters per word, which is squarely
in the range where F is exactly 1.0 — the added classes can only be picked at
depth 1 or 2, while the bag is still nearly full, and picking one eats 8–17
letters and *shortens* the remaining search. So 4.4x at 19 letters is an upper
bound that the geometry of the bag is actively working against.

**What this does not do is give the real number.** Turning the length
distribution into a node count needs phase 2 itself — the ancc DFS over the
class list. That is a ~100-line prototype now that the class list exists, and it
would settle the question against a known target (5,488,296 nodes at 19 letters,
words only). Until it is run, the honest statement is "somewhere between 1.0x
and 4.4x at 19 letters, and much closer to 1.0x than to 4.4x".

## 4. §5's mitigation is vacuous, and its formula is missing a term

§5 proposes keeping a phrase only where being contiguous beats spelling the same
words as separate segments — `count(phrase) > count(w1) × count(w2) × restart`.

That formula drops a `/total` per restart. From `SearchDriver::step()`: a restart
multiplies the running score by `restart` and re-seeds the count at the corpus
total, which the next segment's own count then divides back out. So k segments
score

```
prod(count(w_i)) * (restart / total)^(k-1)
```

and with `restart` 1e-6 against a corpus total of 3,586,472,603, each split
costs ~15 orders of magnitude, not 6. Measured against the correct model the
test is a tautology: **every phrase at every bag size passes**, by 6.7–7.3 orders
of magnitude on average, and the closest case across 462,730 phrases is 2.9.
Only 2 phrases anywhere are within three orders of magnitude of their own split.

There is no cheap mitigation here. F as measured is F.

For completeness, §5's test *as written* — which is a much harsher threshold
than the search it means to model — keeps 8–10% of phrases and would give F =
1.013 / 1.068 / 1.096 / 1.142 / 1.086 across the five bags. That is a real knob
if a cap on the entry list is ever wanted, but it is a deliberate quality filter,
not a no-op.

## 5. Multi-segment phase 1 is cheap, as asserted

`ancc-inspiration.md` lists "the multi-segment phase-1 extraction cost —
asserted as bounded by the bag filter, not measured" among its unverified
claims. It holds:

| Bag | Nodes, words | Nodes, phrases | Time, words | Time, phrases | Peak RSS |
|---|---|---|---|---|---|
| 14 letters | 35,028 | 94,798 | 0.01 s | 0.02 s | 74 MB |
| 19 letters | 202,384 | 784,390 | 0.07 s | 0.16 s | 186 MB |
| 21 letters | 240,281 | 1,021,007 | 0.07 s | 0.20 s | 195 MB |
| 23 letters | 368,247 | 1,689,509 | 0.11 s | 0.34 s | 240 MB |
| 26 letters | 1,826,815 | 7,155,149 | 0.80 s | 1.89 s | 687 MB |

Walking past the space costs ~3.9x the nodes and ~2.4x the time, and the whole
of phase 1 for all 26 letters is under 3 seconds. RSS is the same careless
storage the summary already discounts (`std::string` keys in two hash tables);
the packed form it describes would be a small fraction of it.

## Verdict

By §9's own rule — F ≲ 1.5 → A — this comes out **A**, at every bag size that
matters. 19 letters is 1.448; the bags above 21 exceed 1.5 on the raw class
count, but §3 shows that count overstates what phase 2 actually pays, and §2
shows it does not get worse with depth.

The remaining honest gap is §3's: nobody has converted the class-length
distribution into a phase-2 node count. That is the next measurement, and it is
cheap now.

## Related

- `findings/ancc-inspiration-summary.md` — §5 poses this question, §9 item 2
  commissions this measurement, §0 supplies the 3.2x nodes/letter conversion.
- `findings/ancc-inspiration.md` — the original prototype figures §1 reproduces.
- `source/measure-f.cpp` — the tool. Not installed; measurement only.
