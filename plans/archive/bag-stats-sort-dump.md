# Plan: every bag-stats ranking from a dump

## Outcome

A bag-stats `-n` run is a dump that bag-stats-sort ranks every way bag-stats
can, so trying other SORT options, `--by-pick` or priors doesn't repeat the
walk:

```
bag-stats -n N SORT... [--by-length | --by-pick ...] [-u LETTERS] [-s N]
          [--min-wb-len N] [--ss STEPS] [--pp FILE] [--threads N]
          LETTERS PAIRS DICT > dump
bag-stats-sort [-s N] [-n N] SORT... [--by-length | --by-pick]
               [--score W1,W2... [--score-detail]]
               [--best-pairs appear|lead]
               [--prior FILE | [--p1 INDEX] [--clues CLUEDATA]
                [--prior-weights SPEC]]
               [--top-segments] [--pb-len N] [FILE|-]
```

Each option means what it means in bag-stats; `-s` overrides the dump's
sentence. bag-stats-sort ranks the dump's rows only: percentiles,
`--by-length` groups, `--by-pick`'s Q and `--score`'s "rank R of N" are
taken among them, after the refresh below. On a full dump (no `-n`) whose
files are unchanged it prints what bag-stats prints with the same options.
Without `-n` or `--score`, bag-stats-sort prints every row, sorted, as
today. `--pb-len` drops rows before ranking, as today.

There is no dump switch: every bag-stats run that writes a header writes a
dump, as today.

## Format: version 4

`bag-stats 4`. Version 3's header plus one line after `pp`:

```
sentence N
```

`-s`'s sentence; `sentence` alone when bag-stats ran without `-s`. The rows
are version 3's, after the empty line.

The `pp`, `pairs` and `dict` lines hold absolute paths, `-` for standard
input.

bag-stats-sort reads only version 4 and writes version 4.

## Bags and the refresh

Every bag-stats-sort run rebuilds the pick bags from the files the header
names, as bag-stats built them: PAIRS and the `pp` file, each pair kept when
it fits the header's letters and isn't one of the global or sentence N's
classified NO pairs (`load_no_pairs()` under the workflow root), N from `-s`
or else the header's `sentence` line; the global NO pairs alone when neither
names one, with no alert. A bag's prior is the best of its member pairs'
priors, from `--prior` or `--p1`/`--clues`/`--prior-weights`, and P its
percentile among its list's bags, as in bag-stats. When the header's `pairs` or `pp` is `-`,
bag-stats-sort fails: "the dump's PAIRS was standard input; can't rebuild its
bags" (`--pp` for the pp file).

Then it refreshes each row before ranking. Each name in its PAIR, split on
` + `, is a pick of the ppb list for the first ppb-count positions and of the
pb list after, found by its letters, not its name:

- When a pick's bag isn't in its rebuilt list, the row is dropped. After
  loading, bag-stats-sort prints to standard error `bag-stats-sort: dropped
  K of N rows whose picks have no pairs left`, nothing when K is 0.
- Otherwise each list's picks are put in increasing list order, as the walk
  makes them, and PAIR and PAIRS are rebuilt from the bags: their current
  names and member counts. The counts columns are kept; they depend only on
  the letters left.

Rows that would use bags added to PAIRS since the dump aren't in it, and a
`-n` dump keeps its cut.

## Shared code: `source/bag-stats-rank.{h,cpp}`

Moved out of `bag-stats.cpp`, unchanged in behavior:

- `PairBag`, `load_pair_bags()`, `load_priors()`, and building the
  `PairPrior` from `--prior` or `--p1`/`--clues`/`--prior-weights`.
- `Distribution`, `Ranker`, `PickRanker`, `pick_ranks()`.
- `RankedRow`, `ranks_before()`, `ScoreRank`.
- Printing: `append_row()` (`--top-segments`), `--score`'s rank line,
  `print_score_detail()`, and the `--best-pairs` seen-set printer.
- `Search`, `parse_search()`, `search_text()`, `BestPairs`.
- The ranking options' `--help` lines.

bag-stats keeps `count_sort_values()`, `top_rows()` and `score_rank()`, its
threaded walk passes, as they are. bag-stats-sort ranks its loaded rows in
its own single-threaded loops over the shared rankers: `add()` each row,
`finish()`, `key()` each row, sort with `ranks_before()`, `unit` 0 and `seq`
the row's place in the file, so ties fall in file order, which is rank order
for a `-n` dump and walk order for a full one.

bag-stats-sort's own `sort_keys()`, `ranks()` and `length_ranks()` go;
`Ranker` sorts in their place.

## bag-stats-sort

- Reads the header, builds the pick bags and refreshes the rows as above.
- Option checks: SORT options always; `-n` optional, every row printed
  without it; `--by-length`, `--by-pick` and the prior options with or
  without `-n`. The rest as in bag-stats: `--by-pick` not with
  `--by-length` and with two or more ppb and pb steps in the header's `ss`;
  the prior options only with `--by-pick`, `--prior` not with `--p1` or
  `--clues`, `--prior-weights` only with `--p1` or `--clues`; `--score` one
  pick per ppb and pb step, not with `-n` or `--top-segments`;
  `--score-detail` only with `--score` and `--by-pick`; `--best-pairs` only
  with `-n`, not with `--top-segments`, `lead` only with `--by-pick`; at most
  one of the dump and `--prior` is `-`.
- Keeps each row as its `BagStatsRow` and its picks in one flat `uint32_t`
  array, a fixed count per row, numbered as `RowWalker::picks()` numbers
  them.
- `--score` finds its picks as bag-stats's `score_picks()` does, failing as
  it does when a pick's bag isn't in its rebuilt list, and as bag-stats does
  when the picks don't fit the letters together. When no kept row has
  those picks, it says so on stderr and exits 1.
- Writes the header as bag-stats does: with rows, `-n` or not; none with
  `--score`, `--top-segments` or `--best-pairs`. It is the dump's, its
  `sentence` line the sentence used.
- Single threaded.

## bag-stats

- Writes version 4: the `sentence` line, and absolute paths.
- Its bag loading and ranking call the shared code; its walk, tables and
  threading are unchanged.

## Gate

On the kodiaks S7 run, with `-s 1`: a `-n 1000000 --min-wb-frac --mp` dump's
bag-stats wall time and peak RSS, and on it bag-stats-sort's load time, peak
RSS and `-n 1000 --min-wb-frac --min-wbwp --mp --by-pick --p1 ...
--clues ...` wall time. Report the numbers.

## Files

- `source/bag-stats-rank.{h,cpp}`: new, as above.
- `source/bag-stats-impl.{h,cpp}`: version 4, the `sentence` line.
- `source/bag-stats.cpp`: version 4, bag loading and ranking moved out.
- `source/bag-stats-sort.cpp`: the options, bag rebuilding, the refresh.
- `source/meson.build`: `bag-stats-rank.cpp` in both programs; bag-stats-sort
  links the pairs and pair-evidence libraries.
- `source/test-bag-stats.sh`, `source/test-bag-stats-sort.sh`: version 4.
- `plans/bag-stats-search-sequence.md`: points here for version 4.

## Tests

Smoke: on a small `pb,pb,wb,p` fixture with a `--prior` file, bag-stats-sort
on a full dump prints the same as bag-stats for `-n 5 --by-pick`, `--score
... --score-detail --by-pick` and `-n 5 --by-pick --best-pairs lead`. After
a NO pair is added for one member of a two-member bag and for the only member
of another, bag-stats-sort drops the second bag's rows, reports the count,
and prints the first bag's rows with PAIRS halved.
