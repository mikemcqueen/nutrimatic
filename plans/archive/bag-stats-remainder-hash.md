# Plan: a per-remainder group hash for pair counts

## Outcome

bag-stats counts a leftover's pairs against a small hash of only the inner
dictionary groups that fit the row's remainder, instead of the whole
dictionary's, and stops scanning groups once they pass half the leftover's
length. Output is unchanged; there are no new options.

## Problem

Pass 1 of the s1 used-pair search (`--ss ppb,pb,wb,p`, `$S1TEST`, `-u
catdog`) spends 99% of its time in `PairsExactCounter::count` and
`find`. For each remainder R, `RestCounter` counts pairs in R−W for each of
about 1,500 word bags W. Leftovers are 15–27 letters, past
`PairCountTable`'s 13-letter index, so each goes to `PairCountCache` and,
on a miss (about 60%), to `PairsExactCounter::count`. That scans the ~6,700
groups that fit R; for each group g that fits X = R−W it calls `find(X−g)`
on the counter's table of every dictionary group, several MB of tags,
group numbers and keys, so most finds miss cache.

Every X−g is a sub-bag of R, so it is either one of the groups that fit R
or no group at all.

## RemainderGroups

A new class in `pairs-impl.h`, built from a `Groups` (the groups that fit
R) and reused across remainders so its storage is allocated once per
walker:

```cpp
// The groups that fit one remainder, hashed for find().
class RemainderGroups {
 public:
  // Rebuilds from `groups`, reusing storage.
  void build(PairsExactCounter::Groups const& groups);
  // The group number whose letters are `key`, or NO_GROUP.
  uint32_t find(PairsCounts const& key) const;
 private:
  PairsExactCounter::Groups const* groups_;
  std::vector<Tags> tags_;      // 16 tags per bucket, as PairsExactCounter
  std::vector<uint32_t> slots_; // index into groups_
};
```

Same layout as `PairsExactCounter`'s table: 7-bit tags with the high bit
set, 16 per bucket, one SSE compare per bucket, linear probing, sized to a
power of two at least twice the group count. Slots hold the position in
`groups`, so the key compare reads `groups.keys`, contiguous and already
hot from the scan. It uses `exact_counter_hash`, moved into the header so
both tables share it. At ~6,700 groups that's 16 KB of tags and 64 KB of
slots.

`PairsExactCounter::count(bag, groups)` gains a `RemainderGroups const&`
parameter and calls its `find` in place of its own. The scan loop becomes a
private template over the lookup; the plain `count(bag)` overload runs it
over `groups_by_length()` with the full table's `find`.

## Length order

`PairsExactCounter` gains `groups_by_length()`: `groups()` sorted by
letter count, built once after the last `add`. `RowWalker::filter` starts
from it in place of `groups()`; `filter` keeps order, so every filtered
`Groups` is sorted by length too, as is what `count(bag)` scans. In the
scan loop, `if (twice > length) continue;` becomes `break`.

## Wiring

- `RestCounter` owns a `RemainderGroups` and, when `needs_groups()`,
  builds it from `groups` at the start of `count(rest, fitting, groups,
  out)`, once per remainder.
- `PairCountTable::count` takes the `RemainderGroups const&` alongside
  `groups` and passes it to `counter_.count` on both its index and its
  `PairCountCache` paths.
- The prepared-table path doesn't filter groups and is unchanged.

## Verification

- `meson test`: bag-stats and bag-stats-sort pass.
- Same output: a scratch build with `PAIR_COUNT_TABLE_MAX` at 2^16, so the
  cache and count paths run, prints identical rows before and after on
  `-u heroine --ss pb,wb,p $S9 idx/seed.s9.m4.all.85.15.pairs`.
- Speed: the used-pair search with `--pp` holding only `cat,dog`, 90
  seconds at `--progress 30`, with no bag-stats, query-index or
  dfs-anagrams running (`pgrep -x`). The pair cache
  alone counts 14,415 remainders in that time; compare.
