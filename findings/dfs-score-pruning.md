# DFS score pruning and variable-count BEST bonuses

`dfs-anagrams` performs branch-and-bound score pruning when `-n` is nonzero.
This is separate from dictionary filtering, pair exclusion, letter-fit tests,
and exact-segment-count rejection. This finding explains where score pruning
happens and why changing `-g0` to apply a descending BEST bonus based on each
completed result's segment count cannot be implemented as a final output-only
rescore when the search retains a finite top N.

## Search pipeline

The relevant pipeline is:

```text
index spellings
    |
    v
phase 1: group spellings with identical letters into classes
    |
    v
phase 2: search partial combinations of letter classes
    |
    v
phase 3: expand completed class combinations into concrete spellings
    |
    v
retain the global top N
```

### Phase 1: spelling classes

Phase 1 extracts every eligible index spelling and groups spellings with the
same letter multiset into one class. For example, one class for `aet` might
contain `eat`, `tea`, and `ate`. Members are ordered under an optimistic score,
with member 0 the class's best-scoring representative.

Phase 2 therefore searches combinations of letter classes rather than every
concrete spelling. See `source/dfs-class-list.h`.

### Phase 2: partial-path score bounds

At an intermediate DFS node, the search has:

```text
selected classes:  [class A, class B]
letters remaining: XYZ...
score so far:      S
```

For the remaining letter state, phase 2 can look up a precomputed optimistic
score bound `R`. This gives the invariant:

```text
score of every completion below this node <= S + R
```

Phase 3 maintains a top-N heap. Once that heap contains N results, its weakest
retained score becomes the floor `F`. Phase 2 may then prune the entire partial
branch when:

```text
S + R <= F
```

The comparison is in `DfsAllSolutionsRunner::should_prune()` in
`source/dfs-all-runner.cpp`. It is called from `walk()` before the search
enumerates the next fitting class.

This is not a claim that the remaining letters cannot form a result. It means
that valid results may exist below the node, but even the most optimistic one
cannot enter the current top N.

Score pruning begins only after `DfsTopN` fills its heap and publishes the
floor. Before that point there is no top-N score with which to reject a branch.

Phase 2 also has a length certificate that can skip groups of possible next
classes. It combines the partial score, the best score for a candidate length,
and an optimistic tail score, then compares that total with the same top-N
floor. This is another score cutoff, not merely a structural letter-fit test.

### Phase 3: concrete spelling expansion

Consuming all input letters completes a class combination, not necessarily a
concrete output line. For a path such as:

```text
[class A, class B, class C]
```

phase 3 lazily explores the Cartesian product of the class members:

```text
A0, B0, C0
A1, B0, C0
A0, B1, C0
...
```

It explores these combinations in descending optimistic-score order. If the
best remaining combination cannot beat the top-N floor, it stops expanding
that class path. These cutoffs are in `DfsTopN::emit()` in
`source/dfs-output.cpp`.

Only when `dfs_build_spelling()` constructs a concrete spelling does the code
resolve exact solo-word matching, count distinct BEST-marked segments, and
apply the exact result-level score correction.

There are consequently two places where score can prevent a potential output
line from being constructed:

1. Before all letters are consumed, phase 2 can prune a partial class path.
2. After all letters are consumed, phase 3 can stop before expanding all
   concrete spellings represented by the completed class path.

## Why an output-only rescore is unsafe

Consider a `-n1` search whose currently retained winner has score 100. Suppose
DFS reaches a partial branch whose fixed-BEST scoring says:

```text
score so far                         70
best possible remaining completion  25
optimistic total                     95
```

Since 95 is no better than the floor of 100, phase 2 prunes the branch.

Now suppose that branch would have produced a six-segment result containing
one BEST-marked segment. Current `-g0` scoring gives that segment exponent 4.
A per-result descending policy would give the first BEST-marked segment
exponent 6. That increases the log score by:

```text
(6 - 4) * ln(1e6) = approximately 27.63
```

The result's new score could therefore be approximately 122.63 and should beat
the current winner. It is nevertheless absent from the output because the
search discarded its branch before a complete line existed. A final rescore
cannot recover a line that was never constructed.

The analogous problem occurs in phase 3 if a completed class combination is
rejected at the start of `emit()`, or if its concrete-member expansion stops
before reaching the spelling that would receive the larger exact bonus.

## Why exact `-g N` descending scoring is safe

With `-g N`, every emitted result has exactly `N` segments. During search,
each possible BEST segment receives a deliberately optimistic local exponent.
After a concrete spelling is built, the exact cumulative exponent for `k`
distinct BEST-marked segments is:

```text
B(N, k) = k*N - k*(k - 1)/2
```

Equivalently, the marginal exponents are `N, N-1, ..., 1`. The exact
result-level adjustment is applied as a non-positive correction to the
optimistic score. The required invariant is therefore preserved:

```text
exact final score <= search-time upper score
```

No result can rise above the upper bound used to prune its branch.

## A safe variable-count design

Descending BEST scoring for `-g0` remains implementable. Let `M` be the maximum
possible number of result segments, bounded by:

```text
M = bag letter count / minimum segment length
```

The search can use `M` as the optimistic exponent for every possible BEST
segment. When a concrete result is complete, its actual segment count `N` is
known and its exact bonus can use `B(N, k)`. Since `N <= M`:

```text
search assumption: M, M, M, ...
exact exponents:    N, N-1, N-2, ...
```

the completion-time correction is always non-positive. This makes phase-2 and
phase-3 pruning correct.

The cost is a weaker bound. When `M` is much larger than the actual segment
count, especially with a small `-m`, BEST-containing branches appear far more
promising than they really are. They survive phase-2 pruning longer, and phase
3 may expand many more concrete spellings before proving they cannot enter the
top N.

A tighter variable-count bound is more involved. The existing score-bound
cache is primarily keyed by the remaining letter bag, but the same remainder
can be reached after different numbers of selected segments. The final `N`
also changes the reward for BEST segments already selected earlier in the
path. Tighter bounds would therefore need additional path-sensitive accounting
for depth and BEST state, or another conservative formulation.

## The `-n0` exception

With `-n0`, `DfsTopN::supports_score_pruning()` returns false. Phase 2 does not
request score bounds for top-N pruning, and phase 3 has no score floor at which
to stop spelling expansion. In that enumerate-all mode, applying per-line
descending scoring after completion and then sorting is conceptually safe:
no candidate was discarded merely because it could not beat a retained score.

For finite `-n`, however, the new scoring policy must participate in the
search's admissible upper bounds. Rescoring only the lines that happened to
survive the old fixed-BEST search can return the wrong top N.
