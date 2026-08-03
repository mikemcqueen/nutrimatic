# Rare-word defect in the coherence experiment

The current `measure-coherence` ranking is not an answer-quality ranking. It
omits lexical quality almost entirely, and its coherence statistic has a
separate rare-event defect that actively promotes rare words, misspellings,
and corpus artifacts.

These are two distinct problems. Adding a lexical term is necessary, but it
does not by itself make the present coherence term trustworthy.

## The DFS score is effectively ignored

`measure-coherence` reads and retains the score produced by `dfs-anagrams`.
When sorting by coherence, however, it compares candidates in this order:

1. shape percentile;
2. minimum pair percentile;
3. legacy DFS score;
4. reconstructed text; and
5. input rank.

The legacy score is therefore only a late tie-breaker. Almost every candidate
is ranked as though its lexical quality did not exist.

The intended final model in `findings/rethinking-normalization.md` was:

$$
Q = \alpha Q_{\text{lexical}} + (1-\alpha)Q_{\text{coherence}}.
$$

The standalone experiment deliberately excluded that combination. It
therefore does not test whether coherence improves the DFS ranking; it tests
whether coherence alone produces a useful ranking. Those are not equivalent
experiments.

## The rare-event defect

The more serious problem is that the current smoothed association retains the
classic rare-PMI pathology. It scores an ordered pair as:

$$
a(x,y) = \log
\frac{c_{xy} + \mu p_y}{(c_x + \mu)p_y},
$$

where $c_x$ is the approximate count of the left word, $c_{xy}$ is the
approximate ordered-pair count, and $p_y=c_y/N$ is the background probability
of the right word.

Consider two rare strings that each occur once and occur together once:

$$
c_x=c_y=c_{xy}=1, \qquad p_y=1/N.
$$

Their association is:

$$
a(x,y)
= \log\frac{1+\mu/N}{(1+\mu)/N}
\approx \log(N/\mu).
$$

For a large corpus this can be enormous despite being supported by exactly one
observation.

The smoothing does mix the observed conditional distribution with $\mu$
background contexts, but the background pseudo-count for an extremely rare
right word is only $\mu p_y$. When $\mu p_y \ll 1$, one occurrence still
overwhelms the prior. The claim that this construction generally pulls
low-count observations toward zero is therefore false in precisely the regime
that produces the bad results.

This explains why a recurring typo, rare proper-name fragment, corpus
corruption, or other accidental pairing can outrank ordinary English. The
statistic rewards exclusivity relative to marginal frequency; it cannot tell
whether either string is a legitimate or desirable word.

## The later stages amplify the defect

The rest of the experiment does not repair this behavior:

- Pair calibration can map a one-observation rare coincidence to a top
  percentile among pairs with similarly tiny expected counts.
- `--order best` searches all directed orderings for the path containing the
  strongest pair values, deliberately selecting extreme coincidences.
- Averaging local bigram values cannot establish global sentence coherence.
- Shape calibration only ranks a candidate against other candidates with the
  same word count. It equalizes a distribution; it does not make the
  underlying value a measure of quality.

The current statistic is consequently better described as **index-local
exclusivity of adjacent strings** than as natural-language coherence.
Exclusivity may contain coherence information in a suitably constrained
model, but it is not independently an answer-quality measure in this candidate
population.

## Consequences for the experiment

The observed promotion of rare and misspelled words is not adequately
explained by saying that lexical quality was omitted. It is evidence that the
present coherence component itself fails the experiment's acceptance
criterion. It should not yet be trusted as one term in a combined score.

A credible next experiment needs both of the following changes.

### Restore lexical quality

As an expedient experiment, normalize the existing DFS score or input rank to
a scale comparable with coherence and sweep $\alpha$. A longer-term model
should calculate rendered-word lexical quality directly, for example from
length-conditioned word-frequency percentiles, so it does not inherit DFS's
index-entry segmentation bias.

### Make pair quality evidence-aware

Replace or qualify the current association so that one rare occurrence cannot
win automatically. Possible directions include an evidence-aware posterior
lower bound, a likelihood-ratio or significance statistic, explicit
reliability shrinkage, or at minimum direct sensitivity to repeated pair
observations. The choice needs to be evaluated against real false positives,
not selected only from its mathematical interpretation.

After both components are on comparable scales, combine them first:

$$
Q_\alpha = \alpha Q_{\text{lexical}}
         + (1-\alpha)Q_{\text{coherence}},
$$

and apply final shape calibration to the combined value. Sweeping $\alpha$
then tests the useful question: whether a modest coherence term improves a
strong lexical baseline.
