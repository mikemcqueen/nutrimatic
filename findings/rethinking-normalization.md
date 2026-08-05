# Rethinking normalization and segmentation neutrality

With segment penalty $P=1$, the DFS score for selected index entries
$w_1,\ldots,w_k$ is

$$
S = T\prod_i p(w_i)
  = \frac{\prod_i c(w_i)}{T^{k-1}},
$$

where $c(w)$ is an entry's corpus count and $T$ is the corpus total.
Removing the explicit segment penalty therefore does not make the score
neutral to segmentation. Every additional selected entry still contributes
its corpus probability:

$$
\frac{c(w)}{T} = p(w) \le 1.
$$

Consequently, more index-entry boundaries generally mean multiplying by more
numbers below one. This indirectly favors solutions made from fewer, and
often longer, index entries. It is not an absolute preference: separately
selecting $a$ and $b$ beats the indexed phrase $ab$ when

$$
p(a)p(b) > p(ab).
$$

Nevertheless, the joint-probability model has a structural tendency toward
fewer selected entries. Removing the corpus normalization instead would
reverse the problem: a product of raw counts would strongly favor solutions
made from many short, frequent entries.

## Start with the meaning of neutrality

Neutrality is best defined as an invariance rather than as the absence of one
particular constant.

The strongest useful starting point is:

> A candidate's score should depend on the rendered answer, not on how the
> search happened to divide it into index entries.

This distinguishes two kinds of boundaries:

- **Index-entry boundaries** are properties of the index and search path.
  They should not affect the score of otherwise identical output.
- **Rendered word boundaries** are part of the answer. They may legitimately
  affect lexical quality and natural-language coherence.

For example, `the classroom` should have the same score whether the search
finds it as one indexed phrase or as two selected entries. The current output
does not preserve this distinction, so scoring and deduplication would need
to operate on a canonical rendered representation rather than on the DFS
path.

A useful neutral model should aim for the following properties:

1. **Representation invariance.** Identical rendered text receives an
   identical score.
2. **No mechanical segment drift.** Adding an index-entry boundary does not
   automatically multiply the score by a number below or above one.
3. **Length calibration.** Long and short words are judged relative to
   comparable entries rather than directly by raw frequency.
4. **Quality sensitivity.** Replacing an entry with a more recognizable
   comparable entry improves the score.
5. **Coherence sensitivity.** A natural phrase can beat a combination of
   individually familiar but unrelated words.

There is no assumption-free scalar score satisfying all of these goals.
Comparing candidates with different word counts necessarily introduces a
model of what makes one candidate better. The strictly neutral presentation
is to retain separate rankings by word count or another segmentation shape.

## A calibrated lexical-quality score

One possible non-joint-probability model treats frequency as evidence of
lexical quality rather than as the probability of independently drawing the
whole answer.

First render and tokenize the answer without retaining index-entry
boundaries. For a rendered word $w$ consuming $l$ letters, define a
length-conditioned quality:

$$
q(w) = \operatorname{percentile}(\log c(w)\mid\text{letter length}=l).
$$

The percentile can remain in $[0,1]$, be centered in $[-1,1]$, or be
transformed to a normal score. Conditioning on length is important: raw
counts for short words are normally much larger than raw counts for long
words.

For a candidate containing rendered words $w_1,\ldots,w_m$, use a
character-weighted average:

$$
Q_{\text{lexical}} = \frac{\sum_i l_i q(w_i)}{\sum_i l_i}.
$$

This gives every consumed letter equal influence rather than giving every
selected entry an equal multiplicative cost. It has several desirable
properties:

- a median-quality ten-letter word and two median-quality five-letter words
  receive approximately the same score;
- splitting an index entry does not automatically reduce the score;
- common short words do not win solely because their raw counts dwarf the
  counts of long words; and
- adding more segments does not inherently increase or decrease the score.

The character weighting is a modeling choice, but it expresses a concrete
notion of refinement neutrality: replacing a region with equally well-ranked
smaller regions should leave its contribution roughly unchanged.

## Add coherence separately

Lexical quality alone is likely to promote word salad made from individually
familiar words. Natural-language coherence should be an explicit, separately
normalized signal rather than an accidental consequence of index-entry
boundaries.

For adjacent rendered words, define an association quality such as a
length- or frequency-conditioned percentile of bigram frequency, pointwise
mutual information, or another phrase statistic (but see
[association-is-not-interestingness.md](association-is-not-interestingness.md):
co-occurrence statistics rank function-word bigrams like `they were` above
content phrases, so they serve as a floor on pair quality rather than as the
ordering term):

$$
Q_{\text{coherence}} = \operatorname{average}_i q_{\text{pair}}(w_i,w_{i+1})
$$

Combine lexical and coherence quality with an explicit weight:

$$
Q = \alpha Q_{\text{lexical}}+(1-\alpha)Q_{\text{coherence}}
$$

Both components are averages rather than products. More rendered words
therefore do not cause exponential score decay. Indexed phrase counts can
provide evidence that adjacent words belong together without granting an
extra benefit merely because those words were stored as one index entry.

The one-word case has no adjacency evidence. It can use the lexical component
alone, or a defined neutral coherence value if a common scalar range is
required.

## Strong statistical neutrality

Equal expected scores do not guarantee neutral top results. Candidates with
different word counts or word-length patterns can have different score
variances. A group with higher variance will contribute more extreme results
even when its mean is identical.

A stronger statistical definition converts the raw quality into a percentile
conditional on the candidate's shape:

$$
S = F_{\text{shape}}(Q)
$$

where $F_{\text{shape}}$ is the baseline cumulative distribution of $Q$
for candidates with the same shape. A shape might be the word count, the
ordered word-length pattern, or a coarser grouping when data is sparse. For
example, a `3+5+4` candidate would be compared with other `3+5+4` candidates.

If the calibration is accurate, scores are uniformly distributed under the
baseline for every shape. A score of $0.99$ then means "top one percent for
this shape" regardless of the number or lengths of its words. This is
probably the closest scalar interpretation of segmentation neutrality, but
it depends on a defensible baseline population and enough observations to
estimate each conditional distribution.

## Simpler alternatives

### Geometric mean of entry probabilities

$$
G = exp(\frac{1}{k}\sum_i \log p(w_i))
$$

This removes exponential decay with the number of selected entries, but it
still depends on index segmentation and favors common short fragments unless
the entry probabilities are calibrated for length. It also gives each entry
equal influence regardless of how many letters it consumes.

### Per-character joint log probability

$$
\frac{1}{L}\sum_i \log p(w_i)
$$

This looks length-normalized, but all complete anagrams have the same total
letter count $L$. Dividing by $L$ therefore does not change the current
joint-probability ranking and does not remove its segmentation tendency.

### Products of raw counts

$$
\prod_i c(w_i)
$$

This removes the corpus-total factor but strongly favors candidates with many
short entries. Appending any entry with a count above one improves the score,
which also invalidates the monotone-decreasing score assumptions used for DFS
pruning.

### Separate rankings

Maintain separate top lists for one-word, two-word, three-word, and other
answers, then present them as buckets or interleave them round-robin. This is
the only transparent option that does not assert a scalar exchange rate
between different word counts. Bucketing by rendered word count is generally
more meaningful than bucketing by selected index-entry count.

## Suggested experiment

A practical first experiment would be:

1. Canonicalize and deduplicate rendered answers so index segmentation cannot
   affect their scores.
2. Tokenize rendered words independently of selected index entries.
3. Compute length-conditioned frequency percentiles for individual words.
4. Rank by the character-weighted mean lexical quality.
5. Initially report separate rendered-word-count buckets to expose the
   behavior without hiding it behind a cross-bucket scalar.
6. Inspect the results for word salad and, if necessary, add a separately
   normalized adjacency-quality component.
7. If one interleaved scalar ranking is needed, calibrate the combined score
   to a percentile conditional on word count or word-length pattern.

This model would use corpus statistics without interpreting the answer as a
joint independent draw of index entries. More importantly, it would make the
desired invariances explicit, so any remaining preference for one kind of
answer would come from a stated quality model rather than from the mechanics
of restarting the index search.
