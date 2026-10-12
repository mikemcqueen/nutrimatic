# Phase 2 score bounds in `dfs-anagrams`

The Phase 2 "optimistic score" is an admissible upper bound, not a heuristic
guess. It may be higher than any score that a real completion can attain, but
it must never be lower than the best real completion.

For a maximization search, the required invariant is:

$$
\operatorname{score}(r) \leq U(L)
$$

for every real completion $r$ of the remaining letter state $L$, where $U(L)$
is the stored optimistic bound. Once the finite top-N queue has a weakest
retained score $F$, pruning is safe when:

$$
S + U(L) \leq F
$$

Here $S$ is the score accumulated by the selected class path. If $U(L)$ could
be below the best real completion, this comparison could discard a result that
belongs in the top N. That would be a correctness bug.

## Bound recurrence

Conceptually, Phase 2 evaluates this dynamic program over remaining letter
states:

$$
U(\varnothing) = 0
$$

and

$$
U(L) = \max_{C \in A(L)}
  \left(u(C) + b + U(L-C)\right)
$$

where:

- $A(L)$ is the set of projected class actions that fit state $L$;
- $u(C)$ is the local upper score for class $C$; and
- $b$ is the segment-boundary log score.

The calculation considers every transition represented by the projected state
and takes their maximum. It is not predicting which transition is likely to
lead to a good answer. The recurrence is implemented by
`ScoreBounds::compute_projected_score_bound_top_down()` and the equivalent
bottom-up evaluator in `source/dfs-search-projected.cpp`.

The search combines this remaining-state bound with the score already
accumulated by the current path. `DfsAllSolutionsRunner::should_prune()` in
`source/dfs-all-runner.cpp` performs the final comparison with the top-N floor.

## Why projection raises rather than lowers the bound

An exact cache key containing every remaining letter count can be too large.
Phase 2 therefore keeps a cache-budget-dependent set of rare letter counts
exact and merges all other letters into one wildcard count.

For example, a projected state might retain:

```text
exact:     1 q, 1 z
wildcard:  8 other letters
```

The state no longer remembers exactly which eight other letters remain. The
projected search may consequently admit class combinations that do not fit the
real letter bag. It is a relaxation:

$$
\mathcal{R}(L) \subseteq \mathcal{P}(L)
$$

where $\mathcal{R}(L)$ is the set of real completions and $\mathcal{P}(L)$ is
the larger set of projected completions. Therefore:

$$
\max_{r \in \mathcal{R}(L)} \operatorname{score}(r)
\leq
\max_{p \in \mathcal{P}(L)} \operatorname{upper\_score}(p)
$$

The projection can make the bound loose. For example:

```text
best real completion:       12
projected upper bound:      17
```

That causes extra search work but cannot cause a valid line to be pruned. A
projected value of 10 in this example would violate the invariant.

When multiple real classes consume the same projected letter state,
`ProjectedActions::build()` retains the highest-scoring representative for
that projected delta. Each discarded class would lead to the same projected
child state with a score no higher than the retained representative, so this
quotient operation cannot lower the projected maximum.

## Why a class score is an upper bound

Phase 1 groups spellings with the same letter multiset into a class. The class
members are ordered by `DfsScoreModel::member_upper_log_score()`, and member 0
is the highest-scoring member under that local upper-score model.

The local score deliberately includes bonuses a concrete spelling might be
able to realize. Exact solo matching and descending BEST resolution happen
later, but under the current policy their completion-time corrections are
non-positive. The intended ordering is therefore:

$$
\begin{aligned}
\text{concrete spelling score}
&\leq \text{member local upper score} \\
&\leq \text{class member-0 upper score} \\
&\leq \text{projected completion bound}.
\end{aligned}
$$

`DfsAnagramSearch` reads member 0 for every class when it builds
`best_member_upper_log_scores` in `source/dfs-search.cpp`. Phase 3 also asserts
that the exact concrete spelling score does not exceed the expansion
candidate's upper score:

```cpp
assert(spelling.log_score <= current.upper_log_score);
```

That assertion is in `DfsTopN::emit()` in `source/dfs-output.cpp`.

## Floating-point upper-bound preservation

An exact mathematical upper bound could become slightly too low if an
intermediate floating-point operation rounded downward. The implementation
adds several protections:

1. Each recurrence step calculates an absolute floating-point error envelope.
2. The envelope is added to the computed maximum.
3. A bound stored as `float` is rounded upward with `nextafterf()` when needed.
4. Conversion to the final `double` bound receives additional upward ULP
   padding.
5. The pruning comparison adds a further magnitude-based rounding margin.
6. Score-bound mode is disabled when the platform does not provide the
   expected IEEE binary types and round-to-nearest behavior, or when fast-math
   is enabled.

The arithmetic checks and upward storage are in
`source/dfs-search-projected.cpp`; `round_score_bound_up()` and the final prune
padding are in `source/dfs-search.cpp` and `source/dfs-all-runner.cpp`.

These measures preserve the intended relation after numerical evaluation:

$$
U_{\mathrm{stored}}(L) \geq U_{\mathrm{mathematical}}(L)
\geq \max_{r \in \mathcal{R}(L)} \operatorname{score}(r)
$$

## Interaction with variable-count BEST scoring

Current exact-result corrections only reduce the local upper score. A naive
change to variable-count descending BEST scoring could reverse that relation.
For example:

```text
search-time BEST exponent:       4
completed six-segment exponent:  6
```

The completion step would add score that was absent from the alleged upper
bound. The result could then satisfy:

$$
\operatorname{final\_score}(r) > U(L)
$$

At that point $U(L)$ would no longer be an admissible upper bound, and Phase 2
could prune a branch containing a result that should enter the top N. This is
why variable-count scoring cannot be implemented merely by increasing scores
after completion.

A safe implementation must give the search an exponent at least as high as
any completed result can receive. If $M$ is the maximum possible segment count
and a result completes with $N \leq M$ segments, search can use $M$ locally and
apply the exact $N$-based score as a non-positive completion-time correction.
That preserves admissibility, although the larger upper bound may substantially
weaken pruning.

## Validation and residual risk

The score bound is designed as a correctness-preserving bound, not a ranking
heuristic. An implementation defect or a new scoring term that can improve a
completed result beyond its local upper score could still violate the
invariant.

The focused tests compare bounded searches with exhaustive searches and cover
active versus disabled bounds, fully wildcard-projected states, `float`
storage, threaded calculation, and completion-time descending-BEST
corrections. Relevant fixtures are in `source/test-dfs-search.cpp` and
`source/test-dfs-output.cpp`.

For a direct exhaustive comparison, `-n0` disables the top-N score floor and
therefore score-bound pruning. A finite-`-n` result can be checked against the
corresponding prefix of an enumerate-all run, subject to the cost of producing
the complete result set.
