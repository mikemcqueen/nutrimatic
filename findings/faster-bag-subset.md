# bag-subset: faster deep searches

Why `bag-subset -r` slows down about 4x per extra letter of `-l`, and what
would make `-l 7` and deeper cheap. Change 1 is implemented (see Results);
the rest are not.

## Use

The question that led here: which letters, removed from `$S1`, leave the
fewest sentence-1 seed pairs, for each count of letters removed.
`best-subsets.sh` answers it by running the pair pipeline once and then
`bag-subset -r`:

```
seed-pairs -s 1 -m4 | pfilter -s 1 -l $S1 -d $DICT - > s1.pairs
bag-subset -r -s 1 -l K -d $DICT $S1 s1.pairs
```

`pfilter -l` only checks that a pair's letters fit inside the bag, so removing
letters can only drop pairs. One pair list against the full bag is enough, and
every removal after that is counting.

## Measurements

`$S1` is 90 letters, 22 distinct:
`b1 f1 g1 k1 p1 u1 v1 m2 y2 c3 l3 w3 d5 o5 s5 t5 h6 i6 r6 a8 n9 e15`.
The input has 174,435 pairs before `-s 1` and 154,115 after (the base count).
20 hardware threads.

| `-l` | time | subsets printed |
|---|---|---|
| 1 | 0.32s | 22 |
| 2 | 0.34s | 268 |
| 3 | 0.68s | 2,136 |
| 4 | 2.51s | 12,989 |
| 5 | 10.89s | 64,505 |
| 6 | 43.11s | 272,762 |
| 7 | 153.96s | 1,010,603 |

The subset count grows about 4x per level, and so does the time. At `-l 7`
that's about 150µs per subset.

## Where the time goes

1. Grouping by letter counts buys nothing. `main` folds entries with the same
   letter counts into one group (`std::map<Counts, size_t> entries`), but the
   pairs make 172,539 groups out of 174,435 pairs. The search runs over
   essentially every pair.

2. Every subset scans its parent's whole survivor list. `each_subset` walks
   `matches[depth]`, does a branchy compare against one column for each
   entry, and `push_back`s survivors into `matches[depth + 1]`. In reverse
   mode, survivors are most of the list: 70% after the best single letter,
   15% after the best six letters, and far more for the typical subset, which
   removes common letters that kill almost nothing. So each subset touches
   tens of thousands of entries.

3. The deepest level does the same work as the others. Subsets at depth
   `length` fill `matches[depth + 1]` even though nothing ever reads it; only
   the count matters there. That level is about 3/4 of all subsets.

4. One thread. The branches under different first letters share nothing but
   read-only columns.

5. Every subset is kept, sorted and printed. Harmless at 1M rows, but at
   `-l 9` or `-l 10` it's tens of millions of strings to store and sort, for a
   question that only wants the best few for each size.

## Changes

1. Bitsets instead of index lists.
   - Give each pair its own bit: a group with `number` pairs gets `number`
     consecutive bits, so plain popcount gives exact counts without weights.
     At the base count that's about 154k bits, 2,409 64-bit words, 19 KB.
   - Up front, build one bitset for each letter `i` and each count
     `j = 1..bag[i]`. In reverse mode a bit is set when the pair's count of
     letter `i` is at most `bag[i] - j` (it still fits with `j` copies
     removed). In forward mode it's set when the count is at least `j`. That's
     90 bitsets for `$S1` (one for each letter of the bag), about 1.7 MB.
   - A subset's survivors are its parent's bitset ANDed with the bitset for
     the letter just added at its new count. Its count is the popcount of the
     result. Keep one bitset per depth, as `matches` does now.
   - Under `-march=x86-64-v2` that's about 2.4k word ANDs (SSE-vectorized)
     and 2.4k `popcnt`s, with no branches and no stores beyond the child
     bitset: roughly 1µs per subset, against about 150µs now.

2. Count-only leaves. At depth `length`, fuse the AND with the popcount and
   don't store the child. Combined with 1, the 3/4 of subsets at the deepest
   level only read memory.

3. Threads. Split the top-level loop of `each_subset` by first letter. Each
   thread gets its own depth stack of bitsets and its own results; merge
   before sorting. The 22 first-letter branches are uneven (branches under
   early letters are much larger), so hand them out as work items, or split
   one level deeper (first two letters) for about 250 items.

4. Top N per size. A `--top N` option that keeps, for each subset size, a
   bounded heap of the N lowest counts (highest in forward mode) and prints
   only those. Each thread keeps its own heaps; merge at the end. This takes
   output and sort cost out of the picture at any depth, and it's the form
   `best-subsets.sh` wants. With it, the script's awk step reduces to
   formatting.

5. Pruning, with `--top N`. Kills from removing several letters are at most
   the sum of the kills from removing each one on its own, because the pairs
   killed by a combined removal are the union of the pairs each part kills.
   So for a partial subset at depth `d` with count `c`, every finished subset
   of size `k` below it has a count of at least `c` minus the largest kills
   any `k - d` further letters could make on their own, measured from this
   subset. That bound comes from the counts of its children, which the search
   computes anyway. When the bound is worse than the current Nth best for
   every size still reachable, skip the branch. This is what makes `-l 10`
   and beyond cheap, since most branches remove common letters that kill
   little and fall out early.

## Expected effect

At `-l 7`: changes 1 and 2 take it from 154s to a few seconds, and change 3
takes that under a second. The total work at `-l 9` (about 16M subsets) and
`-l 10` (about 64M) is then seconds to a minute on 20 threads, at which point
printing every subset dominates unless `--top` is used. Pruning cuts deeper
runs further by an amount that depends on the bag; for `$S1`, where the best
removals are always the seven single-copy letters plus `mm`, `lll` or `ccc`,
it should be large.

Change 1 is a contained rewrite of `each_subset` and the column setup in
`main`. It also drops `columns` and `numbers` from `SubsetSearch`.

## Results

Change 1 alone, `-r -s 1` on `$S1`, with nothing else running:

| `-l` | before | after | peak RSS before | after |
|---|---|---|---|---|
| 6 | 43.51s | 0.59s | 81 MB | 68 MB |
| 7 | 154.00s | 1.39s | 102 MB | 88 MB |
| 8 | | 3.98s | | 211 MB |

About 110x at `-l 6` and `-l 7`. Output is byte-identical to the old binary
for `-l 1..6` in both modes and for `-l 7` with `-r`. At `-l 8` (3.35M
subsets), storing the results drives memory, which change 4 would remove.

## Review of change 1

Findings from a code review of the change 1 diff, rated by how much each
would speed up forward mode (no `-r`). None of them is a large speedup; the
first is a possible slowdown. The ratings below are estimates from the code,
not measurements.

1. Forward mode may now be slower than before. Without `-r`, a pair survives
   only if it holds every chosen letter, so survivors fall to a handful within
   two or three letters. The old loop walked only the parent's survivor list,
   so its cost per subset shrank with depth. The new loop ANDs and popcounts
   all 2,409 words (19 KB) for every subset no matter how few bits are set.
   The Results table times only `-r`, so forward mode has not been timed old
   against new. Until it is, change 1 is only known to help reverse mode.

2. Leaves still store the child bitset (`each_subset`, the word loop). At
   `depth + 1 == length`, nothing reads `matches[depth + 1]`, but each leaf
   still writes 19 KB to it. This is change 2, not yet done: pass the depth
   into the loop and, at a leaf, add `popcount(parent[w] & mask[w])` without
   the store. About 3/4 of subsets are leaves. Parent, mask and child together
   are about 57 KB, just over L1, so dropping the store saves real memory
   traffic, but the reads remain. Expected gain is modest, around 1.2–1.4x,
   in both modes.

3. `matches` is sized by `-l`, not by the bag. `main` allocates
   `args.length + 1` full bitsets, so `-l 100000` asks for about 1.9 GB before
   searching, though the recursion can't go deeper than the bag's letter count
   (90 for `$S1`). Clamp the length to `std::min(args.length, sum of bag)`
   before building `matches`. No speed effect at sensible `-l`.

4. Mask setup sets bits one at a time. The triple loop in `main` sets each
   group's `[bit, bit + number)` range bit by bit, and redoes it for every
   `(i, j)` that matches. With groups of about one pair this is cheap today,
   but heavily duplicated input makes setup O(lines × 90) bit operations.
   Better: a small `set_range(Bits&, lo, hi)` that fills whole words, and,
   since a mask is monotone in `j`, compute the matching `j` range once per
   group and letter. One-time setup, milliseconds; no speed effect on the
   search.

5. `matches[0]` is filled bit by bit over all ~154k bits, though it's all
   ones with the tail word masked. The `set_range` helper from 4 covers it.
   Code cleanup only.

6. The word loop scans runs of zero words. Entries are laid out in
   `std::map<Counts>` order, lexicographic by letter counts, so in forward
   mode the survivors of the first letter or two form contiguous runs of
   bits. A subset whose parent has survivors in 5% of the words still ANDs
   and popcounts all 2,409. Carrying the parent's nonzero word range
   `[lo, hi)` down with `matches[depth]`, and narrowing it for the child,
   would skip those runs. This is the one item that could matter a lot in
   forward mode, where it brings back most of the old "touch only survivors"
   cost, and the fix for 1 if 1 turns out to be a real slowdown. In reverse
   mode survivors are dense and spread out, so it does little there.

## Checking

1. Output identical to the current tool for `-l 1..7` on `$S1`, both with and
   without `-r`.
2. Old against new timing at `-l 6` and `-l 7`, with no other `bag-subset`,
   `bag-stats`, `query-index` or `dfs-anagrams` running.
3. With `--top`, the rows match the first N per size of the full output.
