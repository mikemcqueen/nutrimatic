# Defining and calculating coherence quality

> **Status (2026-08-03):** The shrunken-PMI association and input-local
> percentile proposed below failed the existing-index experiment. In
> particular, an unattested continuation of a rare left word stayed near the
> background prior and was then promoted to a high percentile. The proposal is
> retained here as design history, not as the current recommendation.
> `measure-coherence` now uses signed square-root likelihood-ratio evidence
> from the ordered-pair count and both word marginals.
>
> **Update (2026-08-04):** A second, independent failure was measured at the
> opposite end of the frequency range, and the likelihood-ratio replacement
> does not fix it. Function-word bigrams are strong collocations: PMI ranks
> `they were` above `movie star`, and $G^2$ ranks it above every content
> phrase tested. See
> [association-is-not-interestingness.md](association-is-not-interestingness.md)
> for the falsifying table. Association statistics remain usable as a floor on
> pair quality; they do not order good candidates.

There is a sound mathematical foundation for a coherence score, but no
uniquely correct definition of "coherence." The principled part is estimating
whether adjacent words occur together more often than their individual
frequencies predict. The empirical part is deciding how much that signal
should affect Nutrimatic's ranking and what kinds of English-looking answers
the ranking should prefer.

The outline in `rethinking-normalization.md` leaves two distinct choices open:

1. What statistical property should $ q_{\text{pair}} $ measure?
2. How should several pair scores be aggregated and compared across answer
   shapes?

## The probabilistic decomposition

For adjacent rendered words $x,y$, a bigram language model uses

$$ \log P(y\mid x). $$

This measures predictability or conventionality: given $x$, how likely is $y$
to occur next? There is a useful identity:

$$ \log P(y\mid x) = \log P(y) + \log\frac{P(x,y)}{P(x)P(y)}. $$

The final term is pointwise mutual information:

$$ \operatorname{PMI}(x,y) = \log\frac{P(x,y)}{P(x)P(y)}. $$


Bigram fluency therefore decomposes into:

- the ordinary familiarity of $y$, represented by $\log P(y)$; and
- the extra evidence that $x$ and $y$ belong together, represented by PMI.

This decomposition fits the proposed model particularly well.
$Q_{\text{lexical}}$ already rewards individually good words, so
$Q_{\text{coherence}}$ should mostly measure the second term: how much more
expected the adjacency is than it would be from the word frequencies alone.
PMI has a long-standing foundation as a corpus measure of word association;
see Church and Hanks, [Word Association Norms, Mutual Information, and
Lexicography](https://aclanthology.org/J90-1003/).

This also explains why raw bigram count is incomplete:

- `of the` has an enormous count partly because both words are extremely
  common;
- `strong tea` can have a smaller count but a much stronger association
  relative to chance; and
- an unrelated pair can contain two individually frequent words but occur far
  less than independence predicts.

## The counts available from the index

Ideally define:

- $c_{xy}$: occurrences where $y$ immediately follows $x$;
- $n_x$: occurrences of $x$ having a following word;
- $n_y^{\leftarrow}$: occurrences of $y$ having a preceding word; and
- $N_2$: total word-to-word transitions.

Then

$$
\quadP(y\mid x)=\frac{c_{xy}}{n_x},
\quadP(y)=\frac{n_y^{\leftarrow}}{N_2},
$$

and unsmoothed PMI is

$$ \operatorname{PMI}(x,y) = \log\frac{c_{xy}N_2}{n_x n_y^{\leftarrow}}. $$


The existing index is unusually well suited to approximating these counts.
`make-index` emits a rolling corpus suffix starting at each word, so the
aggregate trie count at the space after `x y` is effectively the ordered
bigram count. `measure-f.cpp` already uses the space-node `Choice.count` as a
phrase count.

A reverse map from every word to every phrase containing it is therefore not
necessary merely to score a completed answer. For a rendered adjacency $x,y$,
traverse directly to the space node for `x y ` and read its aggregate count.
If many lookups make that inconvenient, build a sidecar keyed by ordered word
IDs:

```text
(word_id_x, word_id_y) -> bigram_count
```

Reverse outgoing and incoming postings could be added if neighbor enumeration
is needed, but they are not part of the score itself.

One implementation trap is that `exact_entry_count()` returns the residual
count belonging exactly at a terminal after subtracting descendants. For
bigram occurrence frequency, the desired value is the aggregate
`Choice.count` on the phrase-ending space, not necessarily that residual.

It would also be incorrect to sum the frequencies of every longer phrase
containing a pair. The same corpus occurrence can appear under `x y`,
`x y z`, and longer prefixes; summing them would count it repeatedly. Only
ordered adjacency is evidence for a bigram. A phrase that merely contains two
words in non-adjacent positions is not evidence for that pair either.

## Raw PMI needs smoothing

Smoothing addresses only the rare end. The high-frequency failure measured in
[association-is-not-interestingness.md](association-is-not-interestingness.md)
survives it, because a pair with 692,889 observations is not under-evidenced
and shrinkage leaves it where it is.

Plain PMI behaves badly for rare observations. A pair seen once, whose
individual words are also rare, can receive a spectacular PMI score despite
having almost no evidence. This is the classic rare-collocation problem.
Likelihood-ratio methods were developed partly because text contains many rare
events and simple asymptotic statistics behave poorly there; see Dunning,
[Accurate Methods for the Statistics of Surprise and
Coincidence](https://aclanthology.org/J93-1003/).

For Nutrimatic, a useful starting point is shrunken PMI rather than raw PMI.
Let

$$ p_y=\frac{n_y^{\leftarrow}}{N_2} $$

and smooth the conditional distribution toward the unigram background:

$$ \widehat P(y\mid x) = \frac{c_{xy}+\mu p_y}{n_x+\mu}. $$

Then define

$$ a(x,y) = \log\frac{\widehat P(y\mid x)}{p_y}. $$

This has useful semantics:

- $a=0$: the pair occurs about as often as independence predicts;
- $a>0$: $y$ follows $x$ unusually often;
- $a<0$: the adjacency is less common than expected;
- low-count observations are pulled toward zero; and
- an unseen continuation of a rare word is treated as insufficient evidence,
  while an unseen continuation of a very frequent word is stronger negative
  evidence.

Here $\mu$ is the prior strength, measured in pseudo-contexts. It is not a
linguistic constant, but it has a precise meaning. It can be selected by
held-out likelihood, or initially swept over a small range and checked for
ranking stability.

If exact transition marginals are inconvenient, the existing index gives the
useful approximation

$$ n_x\approx c(x), \qquad p_y\approx \frac{c(y)}{N}, $$

where $N$ is the index's total word-start count. The difference is principally
line-final words and other corpus-boundary effects.

## Turning association into pair quality

Because $Q_{\text{lexical}}$ is expressed as a percentile, pair association
can also be put on a calibrated percentile scale:

$$ q_{\text{pair}}(x,y) = F_r\bigl(a(x,y)\bigr). $$

Here $F_r$ is an empirical CDF for pairs with comparable evidence $r$. The
conditioning can use coarse bins of:

- expected count $n_xp_y$;
- $\log n_x$ and $\log n_y^{\leftarrow}$; or
- perhaps word-length pattern if it exhibits a remaining bias.

The reference population matters. It should include possible candidate
adjacencies, including pairs absent from the corpus, rather than only stored
multi-word entries. A practical baseline is to sample pairs from Nutrimatic's
candidate vocabulary while preserving the two marginal-frequency or
word-length bands. Another useful negative baseline is to shuffle the words in
real candidate answers: that preserves lexical quality while destroying most
adjacency information.

Then $q_{\text{pair}}=0.5$ means roughly "an ordinary association for a pair
with comparable evidence," rather than merely "the phrase was stored in the
index."

## What the average means

For rendered words $w_1,\ldots,w_m$, the straightforward definition is the
equal-boundary arithmetic mean:

$$ Q_{\text{coherence}} = \frac{1}{m-1}\sum_{i=1}^{m-1}q_{\text{pair}}(w_i,w_{i+1}). $$

Each rendered boundary is one coherence event, so this should not be
character-weighted. Character weighting makes sense for lexical quality
because words cover regions of different sizes. An adjacency does not cover
either word; it connects them.

There is also a direct probabilistic interpretation if log scores are
averaged:

$$ \frac{1}{m-1}\sum_i \log \widehat P(w_{i+1}\mid w_i) $$

is mean log transition probability. Exponentiating it produces the geometric
mean transition probability; negating and exponentiating gives the usual
perplexity-style quantity. With $a(x,y)$, the average instead means "mean
excess log predictability above the unigram baseline."

The arithmetic mean gives equal expected coherence across word counts, but
not equal variance. A two-word answer has one edge and can receive an extreme
score easily; a five-word answer averages four edges and tends toward the
middle. Thus the shape calibration from `rethinking-normalization.md` is still
needed if all word counts must share one statistically neutral leaderboard:

$$ S_{\text{coherence}} = F_{\text{shape}}\left(Q_{\text{coherence}}\right). $$

The shape could initially be rendered word count, and later ordered word
lengths if enough calibration data exists.

An average can also hide one terrible join among several good ones. Initially
expose the minimum pair score as a diagnostic or tiebreaker rather than
immediately introducing another weight:

$$ Q_{\min}=\min_i q_{\text{pair}}(w_i,w_{i+1}). $$

This will reveal whether the mean is admitting answers with one obvious
word-salad boundary.

For a one-word answer, coherence is genuinely undefined. Score it by lexical
quality alone. Inserting an arbitrary neutral value such as $0.5$ can
unintentionally reward or penalize it when components are combined.

## Limitations of this corpus

The index's counts are useful, but they are not pristine linguistic bigrams:

- punctuation is normalized to spaces, so some cross-sentence or cross-clause
  pairs become ordinary adjacencies;
- the history window is 40 characters;
- article titles are multiplied by ten;
- the merged index has phrase-frequency cutoffs; and
- Wikipedia measures Wikipedia-like conventionality, not universal English
  coherence.

These choices do not invalidate the measure, but they define what it means.

Bigram coherence is also only local. It can recognize conventional neighboring
words but cannot reliably reject globally nonsensical sequences whose
individual transitions happen to be common. A later version could use
interpolated trigram and bigram evidence from the same multi-word data.
Properly smoothed n-gram models remain strong, inexpensive language-model
baselines; see Shareghi et al., [Show Some Love to Your n-grams: A Bit of
Progress and Stronger n-gram Language Modeling
Baselines](https://aclanthology.org/N19-1417/).

Letter frequencies are not especially helpful here. Every complete anagram
has the same letter multiset, so a bag-level letter-frequency statistic is
constant across candidates. Character n-grams could help distinguish
word-like unknown strings, but once the candidates are indexed words, unigram
and word-transition statistics are the relevant signals.

## Recommended first experiment

A defensible first version would be:

1. Obtain $c(x)$, $c(y)$, and aggregate prefix count $c(x,y)$ from the index.
2. Calculate shrunken PMI $a(x,y)$.
3. Convert it to a frequency-conditioned percentile using sampled possible
   candidate pairs.
4. Average equally across rendered word boundaries.
5. Keep rankings bucketed by rendered word count initially.
6. Record both mean and minimum pair quality for inspection.
7. Tune only $\mu$ and the lexical/coherence weight $\alpha$ against a small,
   fixed set of blinded pairwise judgments drawn from real Nutrimatic results.
8. If interleaving shapes, apply the final shape-conditional percentile.

This is not a matter of trying arbitrary formulas until one looks attractive.
PMI, conditional language models, smoothing, and likelihood-ratio statistics
provide principled alternatives with different meanings. Choosing among
"common phrase," "strong collocation," "grammatical sequence," and
"semantically plausible answer" is ultimately a product decision. Empirical
judgment belongs at that final choice and weighting layer, not in place of the
statistical foundation.
