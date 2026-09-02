# Association is not interestingness

> **Status (2026-08-04):** Negative result. Two candidate per-entry quality
> signals were measured against the live index and both were rejected. This
> document exists so neither is re-proposed a third time.

Motivating observation. These two four-entry answers score 82x apart, and the
ordering is counterintuitive:

```
3.105e-31  tiger lily,wooden,heath,movie star
2.542e-29  they were,native somali,light,door
```

Both have $k=4$, so the $(\text{corpus-total}\cdot P)^{k-1}$ divisor is
identical and the entire difference is the product of aggregate counts. The
second answer wins mainly on `they were` (692,889) against `tiger lily` (663).

The question this document answers: is there an index-derived per-entry
statistic that expresses "`they were` is a boring fragment of running text,
`tiger lily` is a thing"? Two candidates were tested. Neither works.

## Rejected 1: aggregate-to-residual ratio

`IndexReader::aggregate_entry_count()` returns the count at an entry's
trailing-space node — the true corpus frequency of the string.
`exact_entry_count()` returns the *residual* after subtracting that node's
children, i.e. occurrences for which the index recorded no continuation.
The ratio of the two looked like a fragment detector:

| entry | aggregate | children | residual | ratio |
|---|---:|---:|---:|---:|
| they were | 692,889 | 657,827 | 35,062 | 19.8x |
| light | 674,574 | 589,478 | 85,096 | 7.9x |
| wooden | 115,775 | 99,823 | 15,952 | 7.3x |
| door | 156,836 | 127,915 | 28,921 | 5.4x |
| heath | 66,510 | 45,861 | 20,649 | 3.2x |
| tiger lily | 663 | 334 | 329 | 2.0x |
| movie star | 2,806 | 1,181 | 1,625 | 1.7x |
| native somali | 16 | 0 | 16 | 1.0x |

The ratio separates the two answers cleanly, and it is **not** merely a
frequency proxy as first suspected. Holding word count fixed in the 650k-2.8M
band, frequency does not predict it:

| entry | count | ratio |
|---|---:|---:|
| village | 1,227,241 | 8.2x |
| light | 674,574 | 7.9x |
| bridge | 651,622 | 3.3x |
| station | 2,827,524 | 2.0x |

`station` is 4x more frequent than `light` and has a 4x lower ratio.

**Why it is still rejected.** What the residual measures is *right-edge
attachment tendency*: how often the string is followed by more recorded text.
That is a positional fact about corpus context, not unithood. `village` scores
8.2x because encyclopedic prose says "village in the...", "village of..."
constantly, and `station` scores 2.0x because it sits at the right edge of
names ("Euston station"). Both are perfectly good standalone nouns. A penalty
built on this ratio would punish `village` as hard as `they were`.

Secondary objection: the residual conflates a genuine end-of-context with the
n-gram window expiring and with rare continuations being pruned at index
construction. For low-count entries the ratio goes to 1.0x mechanically —
`native somali` reads 1.0x because 16 occurrences is too rare for any
continuation to survive pruning, not because it is self-contained. The signal
is only meaningful well above the pruning floor.

## Rejected 2: PMI or $G^2$ over the entry's own words

The prediction was that pointwise mutual information would separate the two
answers hard: `they were` is frequent but built from two of the commonest
words in English, so its association should be near zero, while `tiger lily`
is rare precisely because its parts are rare and should be enormously
overrepresented given that.

**The prediction is false.** Measured against the index, with $n_x$ and
$n_y^{\leftarrow}$ approximated by the aggregate at the `x ` node and
$N_2\approx N$ (the approximations `q_coherence.md` already sanctions):

| bigram | $c_{xy}$ | $c_x$ | $c_y$ | PMI | signed $G^2$ |
|---|---:|---:|---:|---:|---:|
| tiger lily | 663 | 96,564 | 47,079 | **6.26** | 84 |
| brass band | 5,254 | 44,390 | 1,294,807 | 5.79 | 226 |
| silver birch | 609 | 370,111 | 29,094 | 5.31 | 73 |
| wooden door | 570 | 115,775 | 156,836 | 4.72 | 65 |
| **they were** | 692,889 | 5,147,222 | 10,250,540 | **3.85** | **2030** |
| movie star | 2,806 | 359,357 | 719,072 | 3.66 | 123 |
| strong tea | 57 | 400,094 | 97,233 | 1.66 | 10 |
| of the | 32,700,572 | 115.6M | 225.3M | 1.50 | 7571 |
| **native somali** | 16 | 463,669 | 31,643 | **1.36** | 4.5 |
| in the | 22,869,250 | 94.1M | 225.3M | 1.35 | 5735 |

PMI ranks `they were` **above** `movie star` and far above `native somali`.

The reason is straightforward in hindsight: **function-word bigrams are strong
collocations.** `were` follows `they` at 13.5% against a 0.29% base rate — a
47x lift, and $\ln 47 = 3.85$. That is a real association, correctly measured.
PMI is doing its job; the job is just not the one being asked of it.

$G^2$ is worse for this purpose, not better. It scales with evidence, so the
common function bigrams tower over every content phrase: `of the` at 7571,
`in the` at 5735, `they were` at 2030, against `tiger lily` at 84.

## Consequence for the recorded PMI rejection

`q_coherence.md` and `rare-word-defect.md` reject PMI on the rare-event
pathology: a pair seen once, whose words are also rare, receives a spectacular
score on almost no evidence. That holds, and smoothing or a likelihood-ratio
statistic addresses it.

This is a **second, independent failure at the opposite end of the frequency
range**, and no amount of smoothing touches it. Shrinkage pulls under-evidenced
estimates toward the background; `they were` is not under-evidenced. Its
692,889 observations make its association estimate one of the most reliable in
the table. The measure returns a high value because the answer is genuinely
high.

Stated generally: association statistics measure whether two words co-occur
more than chance. That is not the same question as whether a string is an
interesting content phrase, and no reweighting of a co-occurrence statistic
converts one into the other.

## What the measurement does support

**PMI as a floor, not as a ranking term.** `native somali` at 1.36 with 16
occurrences is correctly flagged as a near-chance adjacency — two words that
happen to land next to each other 16 times in three billion. `strong tea` at
1.66 likewise. This matches the structure already in `measure-coherence`,
where the weakest selected boundary is primary and the mean is secondary: the
statistic is useful for rejecting garbage even where it is useless for
ordering good candidates.

**The real distinction is lexical, not relational.** What separates `they
were` from `tiger lily` is that its constituents are closed-class words. That
is a property of the words themselves, not a relationship between them, so no
bigram statistic can recover it. A stopword or content-word test over an
entry's constituents is unglamorous but matches the distinction actually being
drawn, and belongs in $Q_{\text{lexical}}$ rather than $Q_{\text{coherence}}$.

## What this does not license

This document was briefly used to justify deleting `--word-bonus`, on the
reasoning that multi-word entries are not rarer than single-word entries of the
same letter count, so a bonus compensates for nothing. That reasoning is a
category error and the deletion was reverted.

The bonus was never a correction for a measured bias. It is a statement of what
the tool is for: pairs are the objective, so pairs outscore non-pairs and $k$
pairs outscore $k-1$. What is scoped here is the narrower claim that no
*index-derived statistic* recovers a preference for interesting phrases -- and
that claim holds, twice over. The correct conclusion is that the preference
must be asserted as a tunable term rather than inferred, which is exactly what
`--word-bonus` is. See `dfs-score.h`.

## Reproducing

Ratios and counts come from `build/explore-index $IDX "<entry> " N`, reading
the aggregate at the trailing-space node and summing its one-character
children. PMI and $G^2$ use the same lookups; see the tables above for every
input value, so no script is required to check the arithmetic.
