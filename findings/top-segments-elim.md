# What `top-segments --elim` Measures

## Current metric

For a candidate segment or word `s`, the current `--elim` implementation
examines every other exact candidate `t` and tests whether both candidates'
letters fit in the full result bag:

```text
letters(s) + letters(t) <= full bag
```

When they do not fit, `t` contributes one to `s`'s eliminated-item count and
all of `t`'s occurrences to `s`'s eliminated-occurrence total. Exact
candidates remain separate: candidates with identical sorted letter bags are
not merged.

This measures:

> How much exact candidate-occurrence mass is letter-incompatible with
> choosing this candidate?

Equivalently, the item count is the candidate's degree in a theoretical
letter-incompatibility graph, and the occurrence total is the occurrence mass
attached to those incompatible neighbors.

That is a real property, but it is not a count of eliminated result lines.

## Why it is not result elimination

- A result line contains several candidates, so one line can contribute to
  several incompatible candidates' occurrence totals.
- Many incompatible candidates can be concentrated in the same small set of
  result lines.
- Candidates whose letters fit together may nevertheless never coexist in an
  actual DFS result.
- An exact segment may occur more than once on one line, so its occurrence
  count need not equal the number of lines containing it.

The million-row `s7` sample demonstrates the last distinction. It has 28 exact
segments with repeated occurrences within at least one row, producing 1,164
occurrence increments beyond their containing-line counts. One result includes
`tear down` twice.

The current metric's limited useful interpretation is the restrictiveness of
the remaining letter bag after choosing a candidate. It may be a secondary
tie-breaker among candidates already believed to be correct. It should not be
treated as the primary measure of progress toward one correct result.

## Direct line-impact statistics

Let:

- `N` be the number of input rows surviving the current row-level filters.
- `L(s)` be the number of distinct surviving rows containing candidate `s` at
  least once.

Every candidate divides the current result set exactly:

| Decision | Lines eliminated | Lines retained |
|---|---:|---:|
| Reject `s` as bad | `L(s)` | `N - L(s)` |
| Require `s` as correct | `N - L(s)` | `L(s)` |

These support three different ranking goals.

### Find a bad segment to reject

Sort by `L(s)` descending. Rejecting the candidate removes that many current
results. This resembles ordinary `top-segments`, except that it must count a
candidate at most once per result row rather than counting every occurrence.

### Find a correct segment to require

Sort by `N - L(s)` descending, but only among independently plausible
candidates. Without that plausibility judgment, rare noise will always appear
maximally decisive.

### Choose the next uncertain YES/NO question

Sort by:

```text
min(L(s), N - L(s))
```

This maximizes the number of lines guaranteed to disappear regardless of the
answer. Under a uniform prior over the remaining results, the expected number
of eliminated lines is:

```text
2 * L(s) * (N - L(s)) / N
```

Both favor candidates that split the result set near 50/50. The minimum is the
more transparent primary value because it makes no probabilistic claim.

## Other useful values

Two secondary values could help without conflating candidates with result
lines:

- `best-rank`: the first or highest-scoring result containing the candidate.
  This distinguishes candidates represented near the good end of the DFS
  output from tail-only candidates.
- `occurrences - lines`: the number of repeat occurrences within result rows.
  This is primarily diagnostic rather than a ranking metric.

After choosing an anchor, empirical result intersections are more informative
than global incompatibility counts. `select-segment` already reports the
number of rows containing the anchor and then the anchor together with each
possible descendant candidate. Those intersection counts describe the actual
remaining search frontier.

Individual reject impacts are not additive. If two bad candidates occur on
the same rows, adding their line counts double-counts those rows. To rank a
sequence of rejections, rerun the calculation on the surviving result set or
compute each candidate's marginal coverage of the currently live rows.

## Recommended `--elim` semantics

Replace the current primary incompatibility statistics with line-partition
statistics such as:

```text
DECISION_ELIM REQUIRE_ELIM REJECT_ELIM OCCURRENCES SEGMENT
```

where:

```text
DECISION_ELIM = min(L, N - L)
REQUIRE_ELIM  = N - L
REJECT_ELIM   = L
```

The default ordering should use `DECISION_ELIM` descending. It answers which
candidate provides the most useful next decision instead of automatically
favoring either common or rare candidates.

If the letter-incompatibility calculation remains useful, expose it under an
explicit name such as `--incompat`. The eliminated-item total should not be a
primary `--elim` value.

## Optimal line-count implementation

Line impact does not require letter bags or pairwise candidate comparisons.
It can be computed while reading the input:

1. Increment `N` once for each row that survives row-level rejection.
2. Build the selected or projected candidates for that row.
3. Increment each candidate's occurrence count normally.
4. Increment its line count at most once for that row.
5. Derive reject, require, and decision elimination from `N` and the line
   count.
6. Sort the resulting map entries.

A `last_seen_row` stamp in each exact candidate's map entry avoids allocating
a temporary set for every row. If the candidate appears more than once on the
same row, every appearance increments occurrences, but only the first
increments its line count. Word projections apply the same stamp after
splitting the applicable segments.

This costs linear time in the number of candidate instances in the input,
plus output sorting, and uses linear memory in the number of exact candidates.
It naturally supports `--pairs`, `--solo-words`, `--all-words`, and
`--pair-words` without merging candidates that have identical letter bags.

The resulting counts are exact for the rows supplied to `top-segments`. For a
capped DFS output, such as a retained top one million results, they describe
impact on that retained frontier rather than on results omitted by the cap.

## Recommendation

Revise the uncommitted `--elim` implementation to use the line-partition
model before committing it. Preserve the current letter-incompatibility
calculation only if its narrower remaining-bag interpretation proves useful,
and give it a name that does not imply result-line elimination.
