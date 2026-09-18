# BEST-aware phase-2 score bounds

The current phase-2 score bound deliberately overcredits every possible BEST
segment. For exact `-g N`, each such segment receives the largest marginal
BEST exponent, `N`. Concrete expansion later replaces the provisional total
with the exact descending total:

```text
B(N, k) = k*N - k*(k - 1)/2
```

This preserves correctness because the completion-time correction is always
non-positive, but it can make the phase-2 bound substantially looser than
necessary.

## Optimization idea

Track enough BEST state during the phase-2 walk and score-bound calculation to
apply some of the inevitable descending-BEST correction at the early-prune
check.

A partial search path could carry:

```text
cumulative provisional score
selected segment count
provisional BEST count or exponent
```

A cached completion bound could similarly describe:

```text
completion provisional score
completion segment count
completion provisional BEST count or exponent
```

Combining the prefix and completion would reveal the total segment and BEST
counts. Phase 2 could then replace the provisional BEST exponent with the
exact descending exponent before comparing the combined upper score with the
top-N floor.

The direction is sound: applying an unavoidable part of the eventual
non-positive correction earlier can produce a lower, tighter upper bound.

## Why one annotated cached winner is insufficient

The score-bound cache currently retains the maximum provisional score for a
remaining letter state. Attaching segment and BEST counts to only that winning
completion is not safe, because the descending-BEST correction can change
which completion has the greatest score.

For a simplified `-g5` example, measure log scores in units of `ln(1e6)`:

```text
completion A: provisional score 100, three BEST segments
completion B: provisional score  99, zero BEST segments
```

The scalar recurrence selects A because `100 > 99`. After applying the exact
BEST correction:

```text
A correction = (5 + 4 + 3) - (5 + 5 + 5) = -3
A adjusted score = 97

B correction = 0
B adjusted score = 99
```

B is now the best completion. Correcting only A would return 97 even though a
completion scoring 99 exists. That value would not be an admissible upper
bound and could cause incorrect pruning.

In general, correction does not commute with maximization:

```text
correct(max provisional completion)
    !=
max(correct(each possible completion))
```

The completion that wins after correction can also depend on the prefix's
existing BEST count. A cache entry selected without that prefix state cannot
assume that one completion remains dominant for every caller.

## Required frontier

A safe form of the optimization needs to retain the score/count tradeoffs for
each remaining letter state. Conceptually:

```text
U[remaining letters][tail segments][tail BEST count]
    = greatest provisional tail score for that state
```

Given a prefix with depth `p`, provisional BEST count `kp`, and provisional
score `S`, the pruning bound would be the maximum over the cached frontier:

```text
max over (d, k):
    S
    + U[remaining letters][d][k]
    + exact-BEST-correction(p + d, kp + k)
```

For exact `-g N`, the total segment count is fixed, so the segment-count
dimension may be removable or reducible to the number of segments still owed.
For variable-count `-g0`, both segment count and BEST count affect the exact
bonus and must remain represented.

## Phase-1 class-member complication

Phase 2 searches letter classes, not concrete spellings. Its representative
score uses member 0, the member with the greatest provisional local score.

Once a BEST correction is applied during phase 2, member 0 need not continue
to dominate the other members. For example, member 0 may win provisionally
because it receives a large BEST upper bonus, while a slightly lower non-BEST
member can win after diminishing BEST corrections are applied.

Consequently, a flag saying only "BEST applied to member 0" is insufficient.
Each class may need its own frontier containing the best provisional member
score for each relevant BEST-count state. The selected partial path may then
also need a score frontier rather than one scalar representative score.

## Solo-word complication

Solo-derived bonuses are provisional for another reason: different segments
can compete for the same external solo word. A local member upper score also
records only the strongest available solo pair tier.

Exact matching may prefer a non-BEST alternative. In particular, the final
marginal BEST exponent is `1.0`, while the YES exponent is `1.05`. Treating a
possible solo BEST edge as a mandatory BEST segment and applying a correction
for it could therefore lower the bound below an achievable YES-scored result.

A safe frontier must preserve relevant BEST and non-BEST alternatives. It may
continue to ignore solo-word reuse and thereby relax the matching constraint,
but it cannot discard an alternative bonus state that might become optimal
after the global BEST correction.

## Projected-bound complication

The projected score-bound calculation merges classes with the same projected
letter delta and retains only the highest-scoring representative. With a
BEST-aware frontier, this quotient must instead retain the highest score for
each relevant segment/BEST state. A representative that dominates in raw
provisional score need not dominate after correction.

## Cost and possible implementations

The current projected cache stores one `float` per letter state. A dense
frontier over segment count and BEST count is triangular in the maximum result
depth and would require approximately `O(M^2)` values per letter state. That is
likely too expensive for large projected caches.

Possible directions include:

- a dense BEST-count frontier for exact `-g N`, where total depth is fixed;
- sparse Pareto frontiers that discard states dominated in both provisional
  score and correction-relevant counts;
- a smaller secondary BEST-aware cache used only where BEST overcredit is
  large enough to affect pruning;
- a conservative correction based only on BEST counts guaranteed across all
  members and bonus alternatives, although that guarantee may often be zero;
- incorporating exact-depth information into the bound independently, which
  may improve pruning even without BEST-aware corrections.

Any implementation must continue to round bounds upward and must be validated
against exhaustive `-n0` results. The essential invariant remains:

```text
stored bound >= score of every concrete completion
```

The optimization is therefore feasible in principle, but requires retaining
a frontier of correction-relevant alternatives. Annotating only the current
scalar winner with segment and BEST metadata is not sufficient.
