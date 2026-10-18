# Plan: packed keys and staged lookups for bag-stats pair counts

## Outcome

bag-stats counts a leftover's pairs with letter counts packed into one
64-bit or 128-bit integer instead of 32 bytes, and splits each count into a
branchless fit pass and a separate lookup pass. Output is unchanged; there
are no new options. The target is the general pb,pb case; `--pp` with one
or two prospects is not optimized for.

## Problem

`findings/bag-stats-codex.md`: on a 46-letter pb,pb run, 97.7% of samples
are in `PairsExactCounter::count_with`. It scans up to ~196,000 32-byte
group keys (~6.3 MB) per leftover and, for each that fits, probes
`RemainderGroups`, whose key compare reads a random 32-byte entry of that
list. A scratch prototype of packed keys plus staged lookups ran the inner
count 2.1x faster on sampled rows of two configurations, all counts equal.

## Packing

`PairsPacking`, built from the constructor's bag, gives each letter present
in the bag a field of `bit_width(count) + 1` bits: the count plus a guard
bit at the top. Every dictionary key and every remainder fits inside the
bag, so no field overflows. `bits()` is the total.

Fit test and subtraction, for packed pool P and key K with guard mask G:
`r = (P | G) - K`; K fits when `(r & G) == G`; the remainder is `r & ~G`.
A field's guarded value is at least 2^(w-1) and K's field is below that, so
no borrow crosses fields.

Key types are `uint64_t` and `unsigned __int128`. bag-stats picks once at
startup: 64 when `bits() <= 64`, 128 when `<= 128`, otherwise today's
`PairsExactCounter` path, unchanged. A 46-letter bag needed 61 bits; 128
covers bags well past 100 letters.

## PackedPairsCounter<Key>

In `pairs-impl.h`, explicitly instantiated for both key types in
`pairs-impl.cpp`. Built once from a finished `PairsExactCounter` and shared
const across threads:

- `Groups`: packed `keys` and group `numbers`, shortest first.
  `groups_by_length()` is packed once in the constructor.
- `filter(from, bag, out)`: the packed fit test over `from`.
- `Hash`: 16-tag SSE buckets as today, with each slot's packed key and group
  number in arrays parallel to the tags. A tag match compares the inline key;
  the number is read only on a key match.
- `Scratch`: remainder keys and group positions, sized once to the group
  count.
- `count(bag, groups, hash, scratch)`: the staged count below.

It copies each group's size and length from the `PairsExactCounter`.

## Staged count

1. Binary search the length-sorted groups for `end`, the first group with
   2·length > the bag's length, and `half`, the first with 2·length equal
   to it.
2. For `i` in `[0, end)`: compute the remainder, store it and `i` at
   scratch position `n`, and add the fit bit to `n`. No branches.
3. For each of the `n` entries: at `i >= half` with remainder equal to its
   own key, add size·(size−1)/2; otherwise `find` the remainder and count
   it when `i < half` or `b > a`. The finds are independent, so their
   misses overlap.

This keeps today's same-group and count-once rules.

## bag-stats integration

`InnerGroups`, an interface owned by each `RestCounter`, holds the inner
dictionary groups for each pick step and counts pairs over the last one:

- `filter(step, rest)`: narrow step `step`'s groups from the previous
  step's (or all of them) to those fitting `rest`.
- `hash(step)`: build the remainder hash from step `step`'s groups.
- `count(bag)`: exact pairs in `bag`.

`ExactInnerGroups` wraps today's `Groups` and `RemainderGroups`.
`PackedInnerGroups<Key>` holds `PackedPairsCounter<Key>::Groups` per step,
its `Hash` and its `Scratch`, and packs the query bag once per count. The
call is virtual once per filter, hash or count, against a scan of thousands
of groups, so `RowWalker` and `RestCounter` stay non-templated.

`PairCountTable::count` takes a callable for the exact count on a miss in
place of `groups` and `hashed`. Its prepared-table overflow path keeps
calling `PairsExactCounter::count(bag)`.

## Measurement

Master and branch builds, 20 threads, `--ss pb,pb,wb,p`, no `--pp`, `-n`
capped to a few minutes per run, on `$S1TEST` with `-u tennessee`
(46 letters, 64-bit keys) and without it (55 letters, 128-bit keys). Check `pgrep -x` for query-index,
dfs-anagrams and bag-stats before each timed run. Report wall time and first
pair bags per second; the outputs must be byte-identical.

## Tests

One smoke test: for a small bag, the packed and exact counters agree on
every sub-bag's count, for both key types.
