# Summarizing `query-index --score -`

Stdin scoring ranks a list of candidate values against one another, so the
question a reader has at the top of the list is not "what is this score" but
"how far from ordinary is this value". That is a summary over the whole batch,
which raises three choices this records.

## The summary describes log scores, not displayed scores

`displayed_score` is `exp(log_score)`. Over a real corpus the printed column
runs across many orders of magnitude, so its arithmetic mean is whatever the
single best value happens to be, and every other value sits a hair above zero.
A mean of displayed scores would therefore be a slower way of printing the
maximum.

The log scores are the scale the model actually adds in: a segment contributes
`log(count)`, a boundary subtracts a constant, and each bonus is another
additive term. Differences in that space are the units the score is built from,
so a mean and a standard deviation over log scores describe the batch rather
than its largest member.

The mean is nonetheless printed in score units, as `exp(mean of logs)` — the
geometric mean — formatted exactly like the column beneath it. Printing the log
mean instead puts two scales in one display and invites reading a score of
`5.000000` as above a mean of `2.16`, when `log(5) = 1.61` is below it and the
value's deviation is correctly negative. Exponentiating costs nothing: the
geometric mean is the same statistic, stated in the units the reader is already
looking at, and a value is above it exactly when its deviation is positive.

One deviation is stated on the same line, as the factor `exp(deviation)` rather
than as a score. In log space a deviation is additive, so in score units it
multiplies: a value one deviation above the mean scores the mean times that
factor, and one below scores the mean divided by it. An absolute `+1 sigma`
score would repeat the two-scales mistake in a subtler form, since the interval
is not symmetric in score units — on the batch above, one deviation up is
`43.62` while one down is `1.72`, not `8.65 - (43.62 - 8.65)`. The factor is
the one number true on both sides, and it gives the per-value column a unit: a
value at `1.37` deviations scores `5.04^1.37`, or about nine times, the mean.

The per-value column is `(log_score − mean) / deviation`: how many standard
deviations above or below the batch that value scored. The deviation is the
population form, dividing by the number of scored values, because the batch is
the whole population under discussion and not a sample of a larger one. When
every value ties, the deviation is zero and the column is `0.00` for all of
them, which is the truth about that batch rather than a division to guard.

## Zero scores are excluded

A two-word value in neither index orientation scores zero, whose log score is
`−INFINITY`. Including one would make the mean `−INFINITY` and the deviation
`NaN`, destroying the summary for every other value on account of a value that
was never in the index. Those rows are left out of both statistics and print
`-` in the deviation column, which states that the value has no position in the
distribution instead of inventing a finite floor for it.

## Any bonus withdraws the summary

The statistics answer a question about the corpus: how unusual is this value
among the values supplied. A bonus is not corpus evidence. `--pairs`,
`--seed-pairs`, `--yes-pairs`, `--best-pairs`, `--more-best-pairs`, a workflow
root, and `--solo-words` all add a term to some values and not to others, so a
mean computed across a mixture describes the options rather than the index, and
a value would move relative to its neighbors because it was named in a file.

The test is deliberately on the arithmetic and not on the options. Alongside
each score, `score_entry_sequence` accumulates the same sum with every optional
term dropped, which is `log(count)` per entry plus the segment boundaries, and
reports whether the two differ. Anything the model later adds — a new bonus
tier, a new correction — is covered the day it is added, with no list of
options to keep in step. Options that are supplied but inert are covered in the
other direction: `--pairs FILE --pair-bonus 0` loads the file, multiplies
nothing, differs from the plain sum nowhere, and keeps its statistics. Only
values whose scores were actually moved withhold them.

A single moved value withholds the summary for the whole batch, since the mean
is a statement about the batch and one bonused member is enough to make it a
statement about the bonus.
