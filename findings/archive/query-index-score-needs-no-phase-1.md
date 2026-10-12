# `query-index --score` Needs No Phase 1

`--score` answers one question: what score would `dfs-anagrams` give this
exact sequence of index entries? The caller already names the entries, so
nothing needs to be enumerated to find them. This records why the class list
is not merely avoidable but irrelevant to the arithmetic, and which two
couplings make a bag-free path easy to re-break.

## The member-0 terms cancel

Phase 3 scores a spelling as a class-path representative plus per-member
deltas. `dfs_build_spelling` computes

```text
representative + Σ(chosen − member0) + correction
```

where `representative = Σ member0` accumulated in path order. The member-0
terms appear once with each sign, so the value reduces to

```text
Σ member_upper_log_score(count, multi_word, flags) + correction
```

plus the `(k−1)` segment boundaries `append_log_score` contributes. The class
path is a calling convention for reaching `member_upper_log_score`, not an
input to it. Every remaining term has a direct source:

| Term | Direct source |
|---|---|
| `count` | `IndexReader::aggregate_entry_count` |
| `multi_word` | an interior space in the entry text |
| pair `score_flags` | the `pairs` / `weighted_pairs` maps, keyed on entry text |
| solo edges | `DfsSoloWords::resolve()` |
| boundary penalty | `DfsScoreModel::append_log_score` |
| descending BEST | `DfsBestBonusPolicy::descending(entry count)` |
| solo correction | `dfs_exact_result_matching` |

The equivalence is mathematical, not bitwise. Phase 3 groups the sum as a
representative plus deltas; the bag-free path accumulates the selected members
directly. The two groupings are algebraically equal but can round differently,
which is why the regression tests compare the formatted `%#.7g` column and
`assert_close` allows 6e-4 relative error rather than demanding identity.

## `resolve()`, not `lookup()`

`DfsSoloWords::lookup()` asserts `frozen`, and the only production `freeze()`
runs inside `DfsClassList` construction. A bag-free path that called
`lookup()` would abort rather than misscore. `resolve()` is the
non-registering twin and is what score mode must use.

## The solo flags are load-bearing

`dfs_exact_result_matching`'s correction is *relative* to
`solo_local_upper_log_bonus`, which `member_upper_log_score` has already added
via the entry's flags. Gathering the profile for the matching while omitting
`dfs_solo_score_flags(profile)` from the flags makes the correction subtract a
term that was never added. The result is silently too low — no assertion
fires, because the correction is still non-positive.

## Aggregate count, not exact residual

Score mode must read the trailing-space aggregate node, which includes every
longer phrase continuing the entry. That is what phase 1 emits as
`Choice::count`, so it is what `dfs-anagrams` scores. `exact_entry_count` —
the residual excluding continuations — has no production caller; using it here
would make `--score` disagree with the very tool it exists to predict.
