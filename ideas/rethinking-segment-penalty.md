# Rethinking the segment penalty

`dfs-anagrams` currently favors solutions made from fewer selected index
entries. Longer entries consume more letters at once and therefore require
fewer restarts, so this appears as a preference for long "words" even though
the scored units are actually index entries and may contain spaces.

Removing the extra restart penalty is mathematically defensible, provided the
corpus-size normalization remains.

## Current score

For selected index entries $w_1,\ldots,w_k$, the current score is

$$
S=\prod_i c(w_i)\left(\frac{r}{T}\right)^{k-1}
  =T\left(\prod_i p(w_i)\right)r^{k-1}.
$$

where:

- $c(w)$ is an entry's corpus count;
- $T$ is the corpus total;
- $p(w)=c(w)/T$; and
- $r=10^{-6}$.

Every additional selected entry therefore contributes both:

1. its ordinary corpus probability $p(w)$; and
2. an additional one-in-a-million penalty.

The second factor is the strong preference for fewer, and consequently usually
longer, entries. It is a heuristic rather than something required by
probability theory.

## Remove only the extra penalty

Setting $r=1$ gives

$$
S=T\prod_i p(w_i).
$$

This treats the selected entries as independent corpus observations. It
removes the arbitrary per-entry penalty while retaining meaningful frequency
normalization.

The entire $1/T$ factor must not be removed. Scoring with
$\prod_i c(w_i)$ would compare quantities with different dimensions and
would create an enormous bias toward solutions containing many short entries.

Some examples from `wiki-merged.5.index` illustrate the difference:

| Anagram | Current score | Score with $r=1$ |
|---|---:|---:|
| `therapist` | 4676 | 4676 |
| `the,artist` | 0.00009182 | 91.82 |
| `schoolmaster` | 2017 | 2017 |
| `the,classroom` | 0.000005114 | 5.114 |

`therapist` would still beat independently selecting `the` and `artist`, but
by about 51 times rather than 51 million times. The remaining preference comes
from the corpus frequencies rather than the restart constant.

## Indexed phrases

There is a useful probabilistic interpretation for contiguous indexed phrases.
With $r=1$, an indexed `the classroom` competes against separately selected
`the,classroom` as

$$
P(\text{the classroom})
\quad\text{versus}\quad
P(\text{the})P(\text{classroom}).
$$

In the current index, the contiguous entry scores 2943 while the independently
split form would score 5.114. The indexed phrase still wins strongly because
the words genuinely co-occur. The existing restart penalty inflates that
advantage by another factor of one million.

Equivalently, with $r=1$, deciding between a two-word indexed phrase and two
separately selected entries is an association test: the phrase wins when its
observed joint frequency exceeds the independent-frequency estimate. With the
current $r=10^{-6}$, almost any indexed phrase wins, including one whose
observed frequency is far below the independent estimate.

## Search consequences

Changing $r$ from $10^{-6}$ to 1 does not break the score mathematics.
For any $0<r\le1$, every appended-entry contribution is non-positive in log
space because $c(w)\le T$:

$$
\log r+\log c(w)-\log T\le0.
$$

The accumulated score therefore remains monotone. The score bounds and pruning
algorithms can continue to use the same model.

The likely practical effects are:

- more multi-entry results becoming competitive;
- more combinations of common short entries, including combinations that are
  not natural language because the model assumes independence;
- a potentially substantial change in top-N pruning and runtime, which should
  be measured rather than assumed; and
- a need to redefine `--word-bonus`.

The last point follows from the current implementation:
`multi_word_log_bonus = -word_bonus * log(restart)`. Setting `restart` to 1
makes every `--word-bonus` value produce a zero bonus. A segment-penalty change
should therefore decouple the multi-word-entry bonus from the restart
parameter, or retire that bonus if it no longer serves a useful purpose.

## What neutrality means

There is no scalar score that is completely neutral between a solution
containing one selected entry and one containing three. Comparing them
necessarily expresses some prior preference about segmentation. The restart
multiplier is exactly such a prior.

Also, a linear correction based only on character length cannot solve this for
complete anagrams. All candidates consume the same total number of letters, so
an additive term proportional to entry length sums to the same constant for
every solution. A correction that changes rankings must depend nonlinearly on
entry length or, more directly, on the number of selected entries.

If strict neutrality is desired, the cleanest representation is separate
rankings for one-entry, two-entry, three-entry, and other solutions. Results
could be presented in buckets or interleaved round-robin without asserting an
arbitrary cross-bucket ordering.

Geometric-mean or length-percentile scores are possible alternatives, but they
lose the joint-probability interpretation. They are also liable to overpromote
common short fragments or obscure entries that happen to rank highly within a
sparse length bucket.

## Recommended experiment

Introduce an explicit, user-facing segment penalty $P$:

$$
S=\frac{T\prod_i p(w_i)}{P^{k-1}}.
$$

This gives intuitive settings:

- $P=1$: no explicit penalty;
- $P=100$: each additional entry costs a factor of 100;
- $P=10{,}000$: each additional entry costs a factor of 10,000; and
- $P=1{,}000{,}000$: current behavior.

A CLI option such as `--segment-penalty P` would describe the behavior more
clearly than `restart`. Representative queries should be compared at
$P=1$, $100$, $10{,}000$, and $1{,}000{,}000$, recording:

- the number of selected index entries in retained results;
- subjective result quality;
- top-N cutoff scores;
- nodes searched and bound-pruning behavior; and
- setup and search time.

The most principled initial candidate is $P=1$. It removes the ad hoc
boundary penalty, retains corpus normalization and monotonicity, and lets
indexed phrases win over split entries according to actual corpus association.
If it admits too much combinatorial noise, a modest or empirically learned
segment-count prior can then be introduced explicitly rather than returning
immediately to the current million-fold penalty.
