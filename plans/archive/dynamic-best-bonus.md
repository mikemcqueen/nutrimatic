# Plan: make the BEST bonus diminish within an exact-segment result

## Status

Design only. No production implementation has been authorized or started.

## Outcome

For `dfs-anagrams -g N`, replace the fixed bonus on each BEST-marked segment
with a diminishing sequence. The first BEST-marked segment receives exponent
`N`, and each additional BEST-marked segment receives one less:

```text
PB_i(N) = N - i + 1
```

If a result contains `k` BEST-marked segments, its cumulative BEST exponent is:

```text
B(N, k) = k * N - k * (k - 1) / 2
```

For example, a four-segment result uses this sequence:

| BEST-marked segments | Marginal exponents | Cumulative exponent |
|---:|---|---:|
| 0 | none | 0 |
| 1 | 4 | 4 |
| 2 | 4, 3 | 7 |
| 3 | 4, 3, 2 | 9 |
| 4 | 4, 3, 2, 1 | 10 |

The exponent continues to use `DFS_PAIR_BONUS_BASE`, so `B(N, k)` contributes
this multiplier:

```text
pow(DFS_PAIR_BONUS_BASE, B(N, k))
```

This leaves the first BEST preference tied directly to the requested result
size while reducing the incentive to fill one result with several BEST
segments.

`query-index --score` has an exact sequence even though it has no `-g` option.
It infers `N` from the number of comma-separated entries and applies the same
formula, preserving its contract of reporting the score `dfs-anagrams` gives
that exact sequence.

## Settled semantics

### Count marked segments once

`k` is the number of result segments that actually receive at least one BEST
source. It is not the number of BEST source applications.

A segment with both direct BEST membership and a selected BEST solo-word edge
therefore contributes one to `k`, not two. This keeps `0 <= k <= N` for an
exactly `N`-segment result and makes the score agree with the result-level `B`
annotation exposed by `--show-bonus`.

The occurrence order is immaterial. Only `N` and final `k` affect the score,
so permutations of the same segments receive the same cumulative BEST term.

### Keep the final marginal bonus at 1.0

The final possible BEST-marked segment receives exponent `1.0`. Do not floor
it at `DFS_YES_PAIR_BONUS` or the old fixed BEST value.

Consequently, for `N = 1`, or for the final marginal decision in a larger
result, a BEST source can be worth less than the fixed YES exponent `1.05`.
That is intentional for this implementation. Do not change YES, source
precedence, or duplicate-source loading to compensate.

### Preserve fixed scoring where N is unresolved

This change applies dynamic scoring where the complete result size is known:

- `dfs-anagrams -g N` obtains `N` directly from the option; and
- `query-index --score` obtains `N` from the parsed sequence length.

An ordinary `dfs-anagrams` invocation without `-g` can emit results with
different segment counts. Keep its current fixed BEST behavior for this
implementation. Ordinary `query-index` listing and its
`--require-completable` search also retain fixed behavior because they do not
score one exact multi-segment result.

The desired no-`-g` policy is explicitly deferred in `docs/todo`. Do not infer
one while implementing this plan.

### Keep a first-class fixed policy

Retain the fixed calculation as an explicit score-model policy rather than
scattering compatibility conditionals through DFS and query code. It remains
the policy for the deferred contexts above and gives unit tests a direct way
to establish that existing fixed scoring is unchanged.

Do not add a public CLI switch between fixed and diminishing modes in this
implementation.

## Why this is a result-level correction

The current model assigns an independent score to each member. Phase 1 sorts
members by that score, phase 2 adds the best member score for each selected
class, projected and certificate tables cache bounds derived from those local
scores, and phase 3 lazily expands class solutions under the same ordering.

The proposed marginal exponent depends on how many other BEST-marked segments
the completed result contains. Making `pair_log_bonus(BEST)` directly depend
on an occurrence counter would therefore make class order and cached remainder
bounds depend on path state that they do not currently store.

Instead, retain a separable optimistic score during extraction and search,
then apply one non-positive correction after the concrete spelling and its
solo-word matching are known. This follows the existing upper-score design:
phase 3 already corrects independent solo-word upper bonuses after resolving
the globally compatible matching.

## Score policy

### Representation

Add a small value policy in `source/dfs-score.h`, with constructors or named
factories for these two modes:

```text
fixed(exponent)
descending(exact_segments)
```

The policy exposes separate concepts for:

- the per-application BEST exponent used in local upper scores; and
- the exact cumulative exponent for a final number of BEST-marked segments.

`DfsScoreModel` owns the policy. Keep the existing constructor source
compatible by defaulting its new trailing policy argument to fixed
`DFS_BEST_PAIR_BONUS` behavior. Production call sites that know `N` pass the
descending policy explicitly.

Do not represent the mode with a sentinel `double`. An explicit enum or value
type makes fixed and descending behavior reviewable and prevents an exact
segment count from being confused with a bonus magnitude.

### Optimistic local BEST exponent

For a descending policy, every possible BEST application in a phase-1 member
upper score uses:

```text
U(N) = max(N, DFS_YES_PAIR_BONUS)
```

Usually `U(N) == N`. The maximum matters at `N = 1`: pair-source metadata
currently retains the highest-precedence kind, not all numeric alternatives.
A profile containing both YES and BEST possibilities can therefore retain the
BEST kind even though the exact BEST exponent is `1.0` and YES is `1.05`.
Using at least `1.05` keeps the local value a genuine upper bound without
changing the exact BEST result score.

If another fixed tier later exceeds YES, compute the maximum across every
non-BEST fixed tier rather than adding another special case.

The fixed policy continues to use its configured exponent for both local and
exact scoring.

### Exact correction without solo words

Suppose a concrete spelling's optimistic score includes `a` local BEST
applications, and those applications occur on `k` distinct result segments.
For the descending policy, add this correction in log space:

```text
(B(N, k) - a * U(N)) * log(DFS_PAIR_BONUS_BASE)
```

Normally `a == k`. They differ when one segment has both direct BEST
membership and a selected BEST solo edge. The formula both applies diminishing
returns and removes the duplicate application so the segment counts once.

The correction is always non-positive: `B(N, k) <= k * N`, `k <= a`, and
`N <= U(N)`. Preserve that invariant with assertions and conservative rounding
in the same style as the existing solo correction.

## Exact solo-word optimization

The selected solo matching cannot be chosen under the optimistic fixed value
and corrected afterward. Diminishing BEST value makes the matching objective
non-additive: a later BEST edge can be worth less than a competing YES edge,
and a BEST edge on a segment already marked BEST adds no additional BEST
exponent at all.

Keep the existing scalar and fixed-policy matching paths unchanged. Add a
descending-policy exact path whose state records:

- the 16-bit mask of external solo words already used; and
- the number of distinct BEST-marked result segments accumulated so far.

Process the selected single-word profile segments one at a time. From each
state, consider leaving the profile unmatched or assigning each available solo
word in its `word_mask`:

- every selected edge receives the existing word bonus;
- LEGACY, SEED, and YES pair edges receive their existing additive bonus;
- a BEST edge increments the BEST-segment count only when that result segment
  did not already have direct BEST membership; and
- a BEST edge adds no local BEST amount to the dynamic-programming score.

Initialize the BEST count with all directly BEST-marked result segments,
including segments with no solo profile. After processing every profile,
evaluate each surviving state by adding `B(N, k)` and select the greatest exact
score. Retain deterministic tie-breaking and the current preference for a pair
edge when score is otherwise equal.

At most 16 external solo words are supported, so the mask has at most 65,536
values and the BEST-count dimension has at most `N + 1` values. Preserve the
existing disjoint-profile fast path only if it computes the same global
diminishing objective; independent per-profile choices are no longer exact in
general.

Return both the selected solo indexes and enough information to compute the
correction from the optimistic member score. Do not recover the BEST count
from rendered `--show-bonus` strings.

## Implementation steps

### 1. Add the score policy and arithmetic

In `source/dfs-score.h` and `source/dfs-score.cpp`:

- introduce the fixed/descending BEST policy value;
- centralize `B(N, k)` with checked or floating-point-safe arithmetic;
- expose the local upper BEST log bonus separately from the exact cumulative
  BEST log bonus;
- keep `pair_log_bonus(DFS_PAIR_BONUS_BEST)` as the local upper value used by
  member sorting and bounds; and
- add assertions for `N > 0`, `k <= N`, finite values, and non-positive exact
  corrections.

Use the closed form for the cumulative exponent rather than assigning
occurrence numbers to segments. That makes the result permutation invariant.

### 2. Propagate one policy through phase 1, phase 2, and output

In `source/dfs-anagrams.cpp`:

- choose `descending(args.num_segments)` when `args.num_segments > 0`;
- otherwise choose fixed `DFS_BEST_PAIR_BONUS` behavior;
- construct the phase-1/output `DfsScoreModel` with that policy; and
- pass the identical policy into `DfsAnagramSearch`.

Extend `DfsAnagramSearch` and its prepared `DfsSearchData` only as needed to
copy that policy into the model it already owns. Avoid adding BEST counts to
phase-2 cache keys or worker tasks: all phase-2 values remain optimistic local
upper scores.

The following existing mechanisms should continue to consume
`member_upper_log_score()` without bespoke dynamic logic:

- `DfsClassList` member ordering;
- `best_member_upper_log_scores`;
- projected score actions and cached bounds;
- length-certificate bounds; and
- phase-3 Cartesian expansion ordering.

Their values become looser when several BEST segments occur, but remain safe
because exact scores can only decrease from the optimistic value.

### 3. Make the concrete spelling correction exact

In `source/dfs-output.cpp` and `source/dfs-output.h`:

- record whether each selected concrete member has direct BEST membership;
- keep profile-to-segment alignment for selected single-word members;
- invoke the fixed or descending exact solo matcher as selected by the model;
- count the union of direct and actually selected solo BEST markers once per
  result segment; and
- apply the combined solo-conflict and dynamic-BEST correction before
  `DfsTopN::offer()`.

The candidate queue continues to hold optimistic scores. Its early exits and
the published global floor remain correct only if the final correction is
never positive; retain and extend the existing assertions for that invariant.

When `--show-bonus` is active, continue to render one `B` marker for any
segment in the union. The marker does not expose an occurrence ordinal because
the score is defined by cumulative `k`, not result-text order.

### 4. Keep query-index exact scoring aligned

In `source/query-index.cpp`:

- parse the exact sequence before constructing its score model, as today;
- use `descending(score_entries.size())` in `--score` mode;
- feed its direct flags, profiles, and profile-to-entry alignment through the
  same exact result-correction helper as DFS output; and
- leave the ordinary listing model fixed.

Do not duplicate the cumulative formula or solo matching logic in
`query-index.cpp`. A sequence printed by `dfs_spelling_entry_list()` must
receive the same score from `query-index --score`, including direct/solo BEST
overlap and the `N = 1` BEST-versus-YES case.

### 5. Update help without adding an option

In both CLI help texts, stop describing BEST as an unconditional fixed tier in
contexts where descending scoring applies.

`dfs-anagrams` should state that with `-g N`, BEST-marked segments receive
descending exponents from `N` through `1`, counted once per segment, while
invocations without `-g` retain the current fixed value pending a separate
decision.

`query-index` should distinguish exact `--score` sequences, whose entry count
supplies `N`, from ordinary one-entry listing, which retains fixed behavior.
SEED and YES remain fixed at their existing values.

## Minimal validation

Keep tests focused on score arithmetic, ordering safety, and agreement between
the two CLIs.

### Score-model and output tests

Extend the smallest existing C++ score/output fixture to establish:

1. Fixed policy produces the existing BEST score unchanged.
2. Descending `N = 4` produces cumulative exponents `0, 4, 7, 9, 10` for
   `k = 0..4`.
3. The correction is non-positive, including the `N = 1` upper value of
   `1.05` and exact value of `1.0`.
4. Two BEST applications on one segment count once.
5. Phase-3 expansion can select a lower-upper-score spelling whose corrected
   score belongs in the top N.

### Solo-word tests

Extend `source/test-dfs-solo-words.cpp` with compact graphs covering:

1. A selected BEST solo edge on an otherwise ordinary segment increments `k`.
2. Direct BEST plus selected solo BEST on the same segment increments `k` only
   once.
3. A late YES edge at `1.05` defeats a marginal BEST edge at `1.0` when the
   matching constraints make them alternatives.
4. Fixed-policy matching retains its current result.

Avoid a broad mask-size or performance matrix.

### CLI smoke tests

Extend `source/test-dfs-cli.sh` and `source/test-query-index.sh` with one small
fixture that verifies:

1. `dfs-anagrams -g 4` scores one through four BEST-marked segments with the
   cumulative exponents above.
2. A no-`-g` DFS invocation retains its fixed score.
3. `query-index --score` infers `N` and exactly matches the corresponding DFS
   result score.
4. An `N = 1` BEST entry receives exponent `1.0`, without changing YES's
   `1.05` exponent.
5. Existing output without BEST inputs is byte-for-byte unchanged.

Use ratios or log-space helpers where the displayed score would overflow or
lose useful precision; do not weaken existing output-format assertions.

### Focused commands

```bash
source ./setup.sh
source .env/bin/activate
conan build .
source build/dep-info/conanbuild.sh
meson compile -C build
meson test -C build dfs-output dfs-solo-words dfs-cli query-index-cli \
  --print-errorlogs
git diff --check
```

No timing measurement is required by the scoring change. If implementation
work introduces a timing check, first follow `AGENTS.md` and inspect the host
process table for both `query-index` and `dfs-anagrams` outside the sandbox PID
namespace.

Review the complete implementation diff with `/review` before committing, as
required by `AGENTS.md`.

## Expected implementation files

The implementation should normally be limited to:

- `source/dfs-score.h`;
- `source/dfs-score.cpp`;
- `source/dfs-search.h`;
- `source/dfs-search.cpp`;
- `source/dfs-output.h`;
- `source/dfs-output.cpp`;
- `source/dfs-solo-words.h`;
- `source/dfs-solo-words.cpp`;
- `source/dfs-anagrams.cpp`;
- `source/query-index.cpp`;
- focused existing C++ tests; and
- `source/test-dfs-cli.sh` and `source/test-query-index.sh`.

The worktree already contains concurrent edits in several of these files.
Implementation must preserve them, and any eventual commit must stage only the
approved feature files and hunks.

## Explicitly out of scope

- Choosing dynamic semantics for `dfs-anagrams` without `-g`.
- Changing the final marginal BEST exponent from `1.0`.
- Changing `DFS_YES_PAIR_BONUS`, SEED, or legacy pair bonuses.
- Changing weighted-source precedence or retaining multiple direct source
  kinds for one entry.
- Adding a CLI selector for the fixed policy.
- Changing extraction, workflow pair discovery, or exclusion behavior.
- Changing the ordinary DFS result format or adding occurrence ordinals to
  `--show-bonus`.
- Broad search-performance work solely to recover pruning lost to the safe
  optimistic bound.
