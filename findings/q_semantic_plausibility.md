# Semantic plausibility signal

> **No longer current (2026-08-04):** The $G^2$ replacement described below
> fixes the rare-word end but fails at the common end, where it ranks
> function-word bigrams above every content phrase. It is a usable floor on
> pair quality, not the ordering term this document treats it as. See
> [association-is-not-interestingness.md](association-is-not-interestingness.md).

The initial frequency-conditioned shrunken-PMI experiment was rejected. It
turned lack of evidence against unattested continuations of rare words into a
high input-local percentile. That is not affirmative pair evidence.

The replacement pair signal is the signed square root of the likelihood-ratio
$G^2$ statistic from the approximate contingency table:

| | right is $y$ | right is not $y$ |
|---|---:|---:|
| left is $x$ | $c_{xy}$ | $c_x-c_{xy}$ |
| left is not $x$ | $c_y-c_{xy}$ | $N-c_x-c_y+c_{xy}$ |

The independence expectation for the pair cell is:

$$
E[c_{xy}]=\frac{c_xc_y}{N}.
$$

The sign is positive when the observed pair count exceeds that expectation
and negative when it falls below it. The magnitude measures evidence against
independence using the observed pair count and both word marginals. There is
no smoothing strength or input-local pair calibration.

For an answer, the weakest selected boundary is primary and the mean is
secondary. Exact best-order selection uses the same lexicographic objective,
so one spectacular pair cannot conceal an unattested boundary.

This remains a local bigram signal rather than a complete answer-quality
model. Lexical quality is still a separate required component for judging the
individual words.
