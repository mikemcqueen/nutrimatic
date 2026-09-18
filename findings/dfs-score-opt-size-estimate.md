# BEST-aware phase-2 score-bound memory estimate

This estimates the additional memory required to implement
`findings/dfs-score-optimization-idea-2.md` using a dense score frontier indexed
by exact BEST-segment count.

## Summary

For `-g6`, preserving the current projection depth on the recorded reference
workload would require approximately **171 MiB of additional peak memory**.

Keeping the existing default `-C 64` cache limit would instead increase the
estimated process footprint by approximately **25 MiB**, but the automatic
projection would fall from depth 15 to depth 14. Preserving depth 15 for this
workload would require at least `-C 189`.

The score-bound cache is the dominant cost. Per-class and per-action state adds
about 10 MiB on the reference workload when projection depth is held constant.
Worker and prefix state is negligible by comparison.

## Cache representation

The proposed cache is:

```text
V[L][k] = best provisional completion score for letter state L
          with exactly k BEST segments
```

The BEST count is implicit in the array index. The cache does not need to
store a segment count, pessimistic score, or cumulative exponent alongside
each value. This assumes the current depth-relaxed recurrence remains in use:
the cache admits completions of any tail depth, while `-g N` is enforced by
the phase-2 walk.

The current cache stores one upward-rounded `float` per projected letter
state. A direct dense implementation would store `N+1` floats per state:

```text
current cache       = 4S bytes
new cache           = 4(N+1)S bytes
additional cache    = 4NS bytes
```

Here `S` is the projected state count and `N` is the exact segment count.
The estimate assumes each frontier value remains a 4-byte upward-rounded
`float`, as in `ScoreBounds` today. Using `double` values would double all
cache figures below.

## Reference-workload estimate

The recorded depth-15 reference workload has:

```text
projected states:   7,050,240
concrete classes:     490,329
projected actions:    151,440
wildcard span:             17
```

The current scalar table occupies 26.895 MiB. At the same state count:

| Exact count | New cache | Cache increase | Estimated total increase |
|---:|---:|---:|---:|
| `-g4` | 134.473 MiB | 107.578 MiB | about 117.4 MiB |
| `-g5` | 161.367 MiB | 134.473 MiB | about 144.3 MiB |
| `-g6` | 188.262 MiB | 161.367 MiB | about 171.2 MiB |

The last column includes the straightforward per-class and per-action changes
described below. It excludes allocator-scale noise and negligible worker
state.

## Per-class score frontiers

Each phase-1 class must preserve its best local member score for both
`delta_k=0` and `delta_k=1`, rather than preserving only member 0's scalar
score.

A straightforward representation replaces one `double` with two doubles,
adding 8 bytes per class. The current ownership duplicates the vector when
`DfsAnagramSearch::prepare_phase_two()` copies it into `DfsSearchData`, so the
peak increment is:

```text
additional class bytes = 16 * concrete_class_count
```

For 490,329 classes, this is 7.482 MiB. Removing the existing copy as part of
the implementation would reduce the increment to 3.741 MiB.

This is workload-sensitive. A 5,080,410-class workload, such as the recorded
47-letter `-m2` case, would add about 77.5 MiB under the duplicated-vector
layout.

## Projected actions

The projected quotient must retain the best local score for each of
`delta_k=0` and `delta_k=1` for a projected letter delta. Classes with the
same delta share fit metadata, so both alternatives can remain in one action.

`ProjectedAction` is currently 48 bytes, including alignment padding. Adding a
second partial score and its rounding-error base is expected to make it 64
bytes. The exact-support sidecar remains 8 bytes per action, so the steady
increment is approximately:

```text
additional action bytes = 16 * projected_action_count
```

At 151,440 actions, this adds 2.311 MiB. While projected actions are being
built, both the sortable temporary and final action vector may be live, making
the transient increment about 4.622 MiB. The score-bound table has not yet
been allocated at that point, so this transient should not determine the
overall peak when projection depth is preserved.

## Prefix and worker state

The selected class path must carry its best prefix score for each reachable
BEST count. A recursive implementation needs at most a triangular stack of
prefix values:

```text
(N+1)(N+2)/2 doubles per worker
```

For `N=6`, this is 28 doubles, or 224 bytes per worker. Split tasks may need a
small fixed frontier too; even 1,280 queued tasks carrying seven doubles each
would add only about 70 KiB.

Bottom-up preprocessing scratch expands its `best` and rounding-error vectors
by `N+1`. With wildcard span 17, `N=6`, and 20 workers, the incremental scratch
is roughly 32 KiB. Root frontiers and top-down recursion state are similarly
small.

## Keeping the default cache limit

The default projected-score cache limit is 64 MiB. The projection-depth
selection must be changed to use a value width of `4(N+1)` bytes; if it
continues to assume four bytes, it will select a table that later fails the
cache-size check.

For the reference bag:

```text
depth 15: 7,050,240 states
depth 14: 1,741,824 states
```

The current scalar depth-15 table uses 26.895 MiB. A depth-15 frontier no
longer fits in 64 MiB for `-g4`, `-g5`, or `-g6`, while depth 14 does:

| Exact count | Depth-14 cache | Increase over current depth-15 cache |
|---:|---:|---:|
| `-g4` | 33.223 MiB | 6.328 MiB |
| `-g5` | 39.867 MiB | 12.973 MiB |
| `-g6` | 46.512 MiB | 19.617 MiB |

Depth 14 has 85,240 projected actions instead of 151,440. Accounting for the
larger action record but smaller action count, plus the 7.482 MiB class-score
increment, gives approximate total process-footprint increases of:

| Exact count | Estimated increase under `-C 64` |
|---:|---:|
| `-g4` | about 12 MiB |
| `-g5` | about 18 MiB |
| `-g6` | about 25 MiB |

These figures compare the proposed automatic depth-14 layout with the current
automatic depth-15 layout. They do not mean the frontier itself is cheap: the
fixed memory cap is being honored partly by accepting a weaker projection.

## Exact-depth alternative

If the cache also enforces the exact number of remaining segments, it needs a
frontier indexed by both tail depth and BEST count:

```text
V[L][d][k]
```

Retaining every `0 <= k <= d <= N` requires a triangular number of values per
state:

```text
values per state = (N+1)(N+2)/2
```

At the same 7,050,240-state reference depth:

| Exact count | Values/state | Total cache | Cache increase |
|---:|---:|---:|---:|
| `-g4` | 15 | 403.418 MiB | 376.523 MiB |
| `-g5` | 21 | 564.785 MiB | 537.891 MiB |
| `-g6` | 28 | 753.047 MiB | 726.152 MiB |

This dimension is not required for the implementation described in
`dfs-score-optimization-idea-2.md`; retaining the current depth relaxation is
the substantially smaller first implementation.

## Sparse dominance compression

The finding observes that a state need not retain higher-`k` alternatives
that are dominated by its provisional winner. A variable-length frontier
could therefore use less than `N+1` values on average.

That saving cannot be estimated from current counters. It depends on the
distribution of winning BEST counts and surviving lower-count alternatives
over all cached states. Variable-length storage would also require offsets or
metadata per state and would complicate bottom-up contiguous access.

The dense `N+1` representation is consequently the defensible capacity
estimate. A sparse representation should be considered only after
instrumenting a dense prototype to measure the number of live buckets per
state.

## Conclusion

The practical choices for `-g6` on the reference workload are:

- preserve depth 15: approximately 171 MiB additional peak memory and a score
  cache of 188.3 MiB;
- preserve the default `-C 64` cap: approximately 25 MiB additional process
  memory and reduce the projection to depth 14; or
- pursue sparse frontier storage after measuring live-bucket distributions,
  with no reliable memory estimate until that data exists.

The first implementation should retain 4-byte cache values, omit the depth
dimension, make projection selection aware of the frontier width, and expose
the actual bytes per state in the existing preflight diagnostics.
