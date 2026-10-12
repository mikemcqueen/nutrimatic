# Caching phase-2 scores by BEST-segment count

k = number of BEST segments

## Question

> ok so what if we cached up to N+1 (score, segment count, pessimistic BEST
> segment count/cumulative exponent) for -gN, based on BEST segment count. so
> if the top score has k=2, we need to cache the top k=1 if any, and the top
> k=0 if any.

## Response

Yes. For fixed `-g N`, that is the right shape of the solution, provided the
cache stores the best provisional score separately for each exact BEST count.

Define:

```text
V[L][k] =
    greatest provisional completion score for remaining state L
    among completions containing exactly k BEST segments
```

Missing counts contain `-infinity`. Because every completion in one `k` bucket
has the same BEST count, it receives the same prefix-dependent BEST correction.
Maximization and correction therefore commute within that bucket:

```text
max(score + correction(k))
    = max(score) + correction(k)
```

At the prune check, the prefix must likewise retain its best score for each
possible exact BEST count. If `P[j]` is the best prefix score with exactly `j`
BEST segments, the upper bound becomes:

```text
max over j,k where j+k <= N:
    P[j]
    + V[remaining][k]
    + [B(N, j+k) - N*(j+k)] * ln(1e6)
```

This remains an admissible upper bound as long as the cached provisional
scores are themselves upper bounds.

The proposed dominance observation is also correct. If the overall
provisional winner has `k=2`, completions with `k>2` need not be retained:

- their provisional scores are no greater than the winner's; and
- their eventual corrections are more negative because they contain more
  BEST segments.

They therefore cannot overtake the `k=2` winner after correction. The
potentially competitive alternatives are the lower-count buckets:

```text
best provisional score with k=2
best provisional score with k=1
best provisional score with k=0
```

A state can consequently require as many as `N+1` values, while values proved
inferior for every possible prefix can be discarded.

The cache should store the provisional score by exact `k`; it does not need a
separately calculated pessimistic score. The known `k` determines the exact
correction when the cache is queried.

The prefix needs the same per-count representation. Carrying only the
member-0 prefix score and its BEST count would still lose a lower-`k` concrete
member choice that could overtake it after correction.

For exact `-g N`, the final segment count is already known. If the completion
cache continues to admit any tail depth as a relaxation, segment count need
not be stored. If the bound should enforce exactly `N - path.size()` remaining
segments, it must be indexed by both depth and BEST count:

```text
V[L][d][k]
```

Attaching one winning completion's depth to `V[L][k]` is not sufficient. If
that winner has the wrong depth, the best completion with the required depth
and the same `k` has already been discarded.

Each phase-1 class must also preserve the best member score for both `k=0` and
`k=1` when both are possible. A BEST member may have the greatest provisional
score while a lower-scoring non-BEST member becomes competitive after the
BEST correction.

A possible solo-derived BEST match must be represented as an option rather
than mandatory `k=1`. Exact solo matching may select a YES or another non-BEST
edge instead.

The score-bound recurrence would operate on a small array of scores rather
than one scalar:

```text
parent[k + delta_k] =
    max(parent[k + delta_k],
        action_score[delta_k] + child[k])
```

For ordinary direct BEST entries, `delta_k` is zero or one. Entries that can
never win for any prefix BEST count can be removed from the array.

Thus, caching the best provisional score for every relevant exact BEST count
resolves the earlier two-winner counterexample. For fixed `-g N`, these
per-count scores are the information the cache needs. The representation may
be considerably smaller than a general table indexed by both segment count
and BEST count when depth is handled separately or deliberately left relaxed.
