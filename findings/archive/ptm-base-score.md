# `--ptm` on the base count

`dfs-anagrams --ptm` replaces the base term each index entry contributes,
before any bonus, with the value it would carry if the available counts had a
normal upper tail instead of an exponential one. The map is the one in
parametric-tail-map.md; what this records is where it attaches and what that
costs.

## The base is the whole score's shape

A segment contributes `log(count)`, a boundary subtracts a constant, and each
bonus adds a term. Corpus counts are Zipf, so the `log(count)` term has the
exponential upper tail that finding describes: the top of the list is not a
handful of extraordinary entries but the ordinary top of a Zipf list, and one
very frequent entry contributes enough to carry a result past better-balanced
ones on its own. Compressing that tail is the point of the option.

## It maps back to count units, not to deviations

The obvious move is to substitute the mapped deviation itself, and it does not
work. `segment_boundary_log_score_` is `-log(P) - log(corpus_total)`, about
`-21` at the defaults, and a word bonus is `N * log(1e6)`, about `13.8`. Those
are log-count-sized numbers on purpose: at `--word-bonus 1` a multi-word entry
earns back exactly the boundary penalty it costs. Against a deviation ranging
about `[-3, 4]`, one boundary penalty would bury every multi-segment answer and
one word bonus would be worth four standard deviations.

So the base becomes `mean + deviation(log count) * sigma`, over the mean and
standard deviation of the same batch: the log count carrying that mapped
deviation, rather than the deviation. This is the log of the column
`query-index --ptm` prints beside the mapped deviation, and it is what keeps
the boundary penalty and every bonus commensurate with the term they are added
to. Nothing else in the model is retuned.

Summed deviations would also have compared badly across segment counts, since
k of them spread as `sqrt(k)` while the boundary penalty is calibrated against
a sum of log counts.

## The fit attaches between phase 1's member sort and phase 2

The map needs a population and the search has none: scores are computed while
the search runs, long before a result set exists. Phase 1 supplies it. By the
end of `DfsClassList`'s construction every member the bag can reach is packed
with its count, which is exactly the set of base terms phase 2 will ever read,
so `member_log_counts()` is the batch and it is available before any of it has
been scored in anger.

The remap is installed on the model after that, and the members are then
re-sorted. The re-sort is not bookkeeping. `dfs-search.cpp` prunes against one
optimistic bound per class and takes member 0 as that bound, which holds only
because the class list ordered members under the same model. A monotone change
to the base preserves the order of two members' base terms but not their order
under `member_upper_log_score`, which adds bonuses the map does not touch:
compressing the base can move an unbonused frequent entry below a bonused rare
one. Re-sorting under the final model restores member 0 as the class's best and
with it the bound phase 2 relies on. `MemberOrder` is a total order, so the
result is the order a single sort under the final model would have produced.

Phase 2 must be handed the same recalibration, and this is the trap. It does
not borrow the prepared model: `DfsAnagramSearch` takes the penalty, corpus
total and bonus scalars and constructs its own `DfsScoreModel`, while
`DfsTopN` borrows the caller's. A result's score is assembled from both, as a
representative built from the search's per-class bounds plus a per-segment
difference taken from the output's model, so a remap installed on only one of
them does not fall back to plain scoring: it prints a mixture, exact for a
one-segment result and off by a constant for the rest. The constructor takes
the remap so both models carry it, and its bounds stay optimistic for the
same reason its comment already gives.

Re-sorting is also why the fit does not need a second phase-1 pass: it costs
one sort of the member arena and no further trie walk. The dedup step is not
repeated, since one spelling per trie path means it never fires.

## What it costs

The base term stops being a property of the index alone. The fit covers the
entries this bag reaches, so the same entry contributes differently under a
different bag, and scores are comparable only within one run. That is the same
batch-relativity the deviation column has, moved somewhere it is load-bearing
rather than displayed, which is why the option is off by default and why the
diagnostic line states the fitted rate, mean and deviation.

A batch with no fittable spread is an error rather than a silent fallback. A
run that asked for a recalibrated base and quietly did not get one would print
scores indistinguishable from ordinary ones.

`rerank-anagrams` and `query-index` pass the option as false and are unchanged.
A result file scored under `--ptm` is not reproducible by a reranking run that
does not fit the same batch.
