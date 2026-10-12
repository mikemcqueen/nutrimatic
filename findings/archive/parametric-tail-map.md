# The `--ptm` mapped deviation

The deviation column answers "how far from ordinary is this value" in units of
one standard deviation of the log score. That unit only carries its usual
meaning if the log scores are roughly normal, and they are not. `--ptm` adds a
second column stating the deviation each value would carry in a batch that was.

## The log scores are not normal, and not in the way the column implies

Over `.wf/best/s6/seed.m4.idx2.85.15.pairs` (78119 pairs against the wiki
index), and reproduced on the s3, s7 and s9 lists, the log scores are skewed
right at about 0.6 with an excess kurtosis near 0.09. The near-zero kurtosis is
the useful part: this is not the heavy-in-both-directions shape that number
usually accompanies. Two separate things make it asymmetric.

The left side is censored. Corpus counts stop at 2, and 13.9% of the batch sits
exactly there, so the deviation column never prints a value below `-1.35` and
never reaches `-2` at all. The first, fifth and tenth percentiles are one
number. A batch whose left tail is a single atom has its mean pulled toward that
atom, and every ordinary value is left looking further to the right than it is.

The right side is exponential rather than normal. Plotting `-log(1 - F)` against
the log score is straight over the top half at r = 0.987, which is what Zipf
counts produce. An exponential tail always carries more mass at three and four
deviations than a Gaussian does, and it does here: 0.503% of the batch is beyond
`3` where a normal would put 0.135%, and 0.038% is beyond `4` where a normal
would put 0.003%. Those are the entries that read as extraordinary when they are
merely the ordinary top of a Zipf list.

## The map is an empirical body and a fitted exponential tail

`--ptm` maps each log score through an estimate of the batch's own distribution
and back out through the normal quantile. Above a threshold the survival is
modeled as `p0 * exp(-rate * (y - threshold))`, with `rate` the maximum
likelihood exponential fit over the values above the threshold. At or below it,
the value is placed by its own rank, at `(rank - 3/8) / (n + 1/4)`.

The two halves agree at the threshold by construction: the modeled survival
there is exactly `p0`, which is the fraction of the batch above it, so the map
is monotone across the join and the column never disagrees with the score order.
Ties share one mid-rank and therefore one mapped value, which is the truth about
a batch where an eighth of the rows are the same count.

The tail branch computes `-normal_quantile(survival)` rather than
`normal_quantile(1 - survival)`. The survival of the best value in a large batch
is small enough that `1 - survival` rounds to `1` and the quantile goes
infinite; taking the negative quantile of the small number directly costs
nothing and has no such point.

## The mapped deviation is restated in score units beside it

A deviation is not the scale the reader arrived on. The first column is a score,
and without a bonus it is the segment count straight out of the index, so a
mapped deviation of `1.18` answers a question the reader did not ask in the
units they were reading. The column beside it inverts the deviation column:
`exp(mean + mapped * sigma)`, the score whose distance from the batch mean is
the mapped deviation rather than the value's own.

That makes it directly comparable to the first column, and the pair is the whole
point of the map. A value that scores `80` on a mapped deviation of `1.18` reads
as `58.06`: the count an ordinary batch would have needed for that rank, against
the count this batch actually holds. The gap between the two columns is what the
exponential tail was inflating.

It restates a position and does not rescore anything. The index count is
untouched, the sort order is the first column's, and the arithmetic is the
deviation column's own formula run backwards, so a value whose mapped deviation
is `0.00` prints exactly the geometric mean on the summary line. Printing it
through `displayed_score` and the score formatter, rather than as its own kind
of number, is what keeps it readable against the column it is meant to be
compared with.

## The threshold is the top percent, not the top decile

This is the parameter that decides whether the column is worth printing, and the
decile is wrong. A tenth of this batch reaches down to a log score of 6.0, well
into the body, where the distribution has not become exponential yet. Fitting
there flattens the rate to 0.852 and the map then overcorrects: the largest
value lands at `3.85` when a normal sample of 78119 should reach about `4.36`,
and only 0.047% of the batch clears `3` where a normal puts 0.135%. Trading too
many extreme values for too few is not an improvement.

Fitting above the top percent gives a rate of 1.209 and lands on the target
almost exactly — 0.132% beyond `3` against 0.135%, 2.330 at the 99th percentile
against 2.326, and a largest value of `4.37` against the 4.363 expected. The
remaining gap is in the upper middle of the tail, around the 99.99th percentile,
where the map reaches 3.60 against 3.72. The batch minimum of two tail values
keeps small batches well defined; below about 200 values that minimum is what
sets the threshold, and the column describes a handful of points.

## An exponential, not a generalized Pareto

The usual next move for a tail this shape is a generalized Pareto, whose extra
shape parameter lets the tail decay faster or slower than exponential. Fitted
above the top percent it returns a shape of -0.04, which is nearly the
exponential it generalizes, and it moves nothing worth the cost: 0.134% beyond
`3` against the exponential's 0.133%, on a target of 0.135%. It earns its keep
at the decile threshold, where it repairs some of the damage that threshold does
— but the threshold is the thing to fix, and once it is fixed there is no
second parameter to estimate and no maximum likelihood search to carry.

## Why not rank every value

Mapping every value by rank, with no fitted tail at all, is exact by
construction for a batch with no ties and needs no parameter. It is not
available here. The 13.9% of rows holding count 2 must all receive one value, so
a pure rank map still reports a skew of 0.235 and leaves 0.19% of the batch
beyond `3`. Breaking those ties arbitrarily brings the skew to 0.000 and the
Kolmogorov-Smirnov distance to 0.000, which confirms that the floor block is the
entire residual and that no monotone function of the count can remove it.

The fitted tail also keeps the column meaning something outside its own batch. A
rank map is wholly batch-relative: the same pair scores differently depending on
what it was listed alongside. With the tail fitted, the batch enters only
through the threshold, `p0` and the rate, and the summary line prints the rate
so a reader can see which scale they are reading.

## What withholds the columns

Both columns follow the deviation they sit beside. Any bonus withdraws them, for
the reason in stdin-score-statistics.md: a bonus is not corpus evidence, and a
distribution fitted across a mixture describes the options rather than the
index. Zero-scoring values print `-` in both, since a value in neither index
orientation has no position in the distribution to map, and so no score to state
that position in. Fitting also needs three finite
scores and at least one below the threshold, without which there is no tail to
separate from a body.
