# Plan: `bag-stats --ss`

## Status

Implemented in `source/bag-stats.cpp`, `source/bag-stats-impl.{h,cpp}` and
`source/bag-stats-sort.cpp`.

## Outcome

`bag-stats` takes a search sequence that says how many pairs and words to
take out of the letters before counting the exact pairs left:

```
bag-stats [-u LETTERS] [--min-wb-len N] [--ss STEPS] [--pp FILE]
          [--threads N] [-n N SORT... [--by-length]] LETTERS PAIRS DICT
```

`STEPS` is a comma-separated list of:

| step | picks |
| --- | --- |
| `ppb` | a pair bag from `--pp FILE` (prospective pairs, same format as PAIRS) |
| `pb` | a pair bag from PAIRS |
| `wb` | a word bag from DICT |
| `p` | the exact pairs of what is left (`pairs -m3 -x -c`), as today |

The default is `pb,wb,p`, which counts the same as today. `p` comes exactly
once, last. There is at least one `pb` and at least one `wb`. `--pp` is
given exactly when there is a `ppb`.

Example: `--ss ppb,ppb,pb,wb,wb,p --pp mine.pairs` takes two of my prospective
pairs, one seed pair and two words, then counts the pairs left.

## Rows

Each row is one combination of pair picks: every `ppb` and `pb` pick is named
in the row. The `wb` picks are counted in the row's columns, as the single
word bag is today.

Subtraction doesn't depend on order, so `STEPS` is a multiset:
`pb,wb,pb,wb,p` and `pb,pb,wb,wb,p` print the same rows. The `ss` header line
records the canonical order: the `ppb` steps, then `pb`, then `wb`, then `p`.

- Picks of one kind are unordered: a row is printed once, not once per
  ordering of its picks.
- The pair picks in a row are distinct bags, across `ppb` and `pb` together.
- Word picks may repeat a bag (a word used twice).
- Every combination of pair picks that fits R is a row, including rows whose
  counts are all 0, as today.

Rows come in walk order: the `pb` picks outermost, then the `ppb` picks,
each list in order of first appearance in its file. `PAIR` still names the
`ppb` picks first.

## -n

`-n N` prints only the N rows that `bag-stats-sort` with the same SORT
options (`--wb`, `--wbwp`, `--tp`, `--mp`, `--min-wb`, `--min-wb-frac`) and
`--by-length` would print first, in that order, ties in walk order. `-n` and
the SORT options go together.

A row's rank depends on the other rows only through how many rows have each
sort value (per pair length with `--by-length`). So with `--by-length` or
more than one SORT option, a first walk collects those counts, and a second
walk ranks each row against them and keeps the best N in a heap per thread.
With one SORT option and no `--by-length`, the value is the rank and one
walk does.

## Output

Header version 3. `PAIRBAG` goes; `ss` and `pp` lines are added.

```
bag-stats 3
letters R
min 3
min-wb 5
ss ppb,ppb,pb,wb,wb,p
pp PATH              (`pp` alone without --pp)
pairs PATH
dict PATH
columns PAIR,PAIRS,WORDBAGS,WB_WITH_PAIRS,TOTAL_PAIRS,MAX_PAIRS,WB_SOLO,MIN_WB,MIN_WB_WITH_PAIRS

cat dog + red hat + sun day,1,...
```

| column | meaning |
| --- | --- |
| `PAIR` | the picks' first rows, each as its two words space-separated, joined by ` + `, in canonical step order |
| `PAIRS` | the product of the picks' distinct-row counts: how many row combinations the bag combination stands for |
| `WORDBAGS` | word-bag tuples (one bag per `wb` step) that fit what the pair picks leave |
| `WB_WITH_PAIRS` | of those, how many leave a remainder with PC > 0 |
| `TOTAL_PAIRS` | sum of PC over those tuples |
| `MAX_PAIRS` | largest PC among them |
| `WB_SOLO` | tuples that use up all that is left |
| `MIN_WB` | tuples whose every bag has at least `--min-wb-len` letters |
| `MIN_WB_WITH_PAIRS` | of those, how many have PC >= 2 |

`read_field` needs a value after the space, so the reader accepts a bare
`pp` line as an empty path.

## bag-stats-sort

`--by-length` and `--pb-len` use the number of a-z letters in `PAIR` in place
of `PAIRBAG`'s length: the combined length of the row's pair picks.
Otherwise unchanged; it reads and writes version 3 only.

## Implementation

### Pair picks

1. Load the `ppb` and `pb` bag lists as `load_pair_bags` does, and the word
   bags as `load_word_bags` does.
2. `RowWalker` walks pair picks depth-first, `pb` steps then `ppb` steps.
   Picks of one kind take increasing indices; a pick whose bag equals an
   earlier pick's bag in the row is skipped. Each level subtracts its bag
   from the rest and skips bags that don't fit.
3. Each combination that reaches the last pair step is a row; RK = R - its
   picks. A shared cache reuses the statistics of rows that leave the same
   RK, including across the two ranking passes. Pair names and PAIRS are
   still computed from each row's own picks.

### Row counts

For one RK, `RestCounter` enumerates `wb` tuples depth-first with
non-decreasing indices, first restricting the word-bag list to the bags that
fit RK. At each leaf, WR = what is left: solo, the 6-letter floor, PC, and
MIN_WB when every bag in the tuple has at least `--min-wb-len` letters.

When R has at most 2^28 sub-bags, and either the search has more than one
step (pairs plus prospects) or ranking walks the rows twice, `RestTable`
indexes all of them with a mixed-radix number. Otherwise no two rows leave
the same RK, and the table stays unallocated. Its 32-bit slots refer to up
to 2^25 immutable records, each containing the seven 64-bit remainder
statistics. A release store publishes each record; readers use acquire
loads. Concurrent misses may compute the same statistics, but only one
publishes a record. Once the record capacity is reached, new remainders are
computed without caching. Larger bags and failed cache allocations also use
the uncached path.

The slot array takes at most 1 GiB and the records at most 1.75 GiB. With
the pair count table below, total cache allocations are at most 3.75 GiB,
leaving room within a 5 GB budget for dictionaries, ranking and workers.

### Pair count table

PC comes from `PairCountTable`, one per run, shared by every thread: a
`uint32_t` per sub-bag of R, numbered by `SubBagIndex` (as bad-bags numbers
them), holding PC + 1, or 0 until first asked for. Threads read and write
slots with relaxed `std::atomic_ref`; two threads filling one slot write
the same value. The array comes from `calloc`, so pages no slot of which is
filled take no memory. A PC of `UINT32_MAX` or more isn't stored.

The index covers sub-bags up to the longest leftover a row can ask about: R
less the shortest bag of each pair step's list and the shortest word bag per
word step. When that numbers more than 2^28 sub-bags (1 GiB), the cutoff
drops until it doesn't; longer leftovers are counted every time.

For searches with multiple pair steps and more than 1,024 PAIRS bags, when
all of R's sub-bags fit within the same 2^28 limit, the table is instead
filled before walking rows. Workers enumerate unordered pairs of inner
dictionary groups, add their mixed-radix numbers, and accumulate the number
of distinct word pairs in that slot. Groups paired with themselves
contribute n*(n-1)/2. This table stores PC directly, including zero; counts
that reach UINT32_MAX fall back to the exact counter when queried.

### Pair counting

- With the lazily filled table, each pick also narrows the inner
  dictionary's groups (`PairsExactCounter::Groups`) to those that fit what
  is left, and
  `PairsExactCounter::count(bag, groups)` scans only those.
- With the table filled in advance, rows look up exact pair counts directly
  and do not filter or scan the inner dictionary groups.
- `count` looks a pair up from its shorter word only, or from the
  lower-numbered one when both are as long, so it skips groups longer than
  half the bag.
- The partner lookup is an open-addressing table of 64-bit slots (a hash tag
  and the group number + 1) with a hash whose four word multiplies are
  independent, in place of `std::unordered_map` and `PairsCountsHash`.

### Threads

`--threads N`, default 1, 0 for all hardware threads, capped at that number,
as `bad-bags --threads`. Workers claim first picks (units) from a shared
counter.

- Without `-n`, each unit's rows go to a buffer; the calling thread prints
  the buffers in unit order. Workers don't start a unit more than 16 per
  thread past the last one printed.
- With `-n`, each worker keeps per-thread sort-value counts (first walk) and
  a heap of its best N rows (second walk), merged at the end.

Output is the same for every thread count.

### Files

- `source/bag-stats.cpp`: `--ss`, `--pp`, `--threads`, `-n`, the walk, the
  table and the workers.
- `source/bag-stats-impl.{h,cpp}`: version 3 header with `ss` and `pp`,
  `BagStatsRow` without `pair_bag`, the column list, and the sort options and
  values bag-stats and bag-stats-sort share.
- `source/bag-stats-sort.cpp`: length from `PAIR`'s letters; the shared sort
  options.
- `source/pairs-impl.{h,cpp}`: `PairsExactCounter` groups, `filter`,
  `count(bag, groups)` and its slot table.
- `source/test-bag-stats.sh`: version 3; `ppb` against `-u`, `pb,pb,wb,p`,
  `wb,wb` and `-n` rows checked by hand.
- `source/test-bag-stats-sort.sh`: version 3.
- `plans/bag-stats.md`: points here for the version 3 format.
