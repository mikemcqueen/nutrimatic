# Plan: `bag-stats`

## Status

Implemented in `source/bag-stats.cpp`, with the exact-count index in
`source/pairs-impl.{h,cpp}` and a smoke test in `source/test-bag-stats.sh`.

## Outcome

A new tool that ranks the letter bags of candidate pairs by how many complete
three-part solutions they leave open: pair + dictionary word + pair = all of
the letters.

```
bag-stats [-u LETTERS] [--min-wb-len N] LETTERS PAIRS DICT
```

For remaining letters R = LETTERS - used letters:

- PAIRBAGS: the distinct letter bags of the PAIRS rows that fit R.
- WORDBAGS: the distinct letter bags of the DICT words that fit R.
- For each PAIRBAG, PR = R - PAIRBAG. For each WORDBAG that fits PR,
  WR = PR - WORDBAG, and PC is what `pairs -m3 -x -c WR` prints.

One CSV row is printed per PAIRBAG.

## Inputs

- `LETTERS`, `-u/--used-letters`: cleaned and subtracted as `pairs` does
  (`clean_letters`, `subtract_letters`); `-u` may repeat and accumulates.
- `PAIRS`: `word,word` rows read with `read_rows` (`allow_single_words`
  false). A row's bag is the letter counts of both words together. Rows whose
  bag doesn't fit R are skipped.
- `DICT`: one word per line, `-` for stdin. No minimum length; every word
  with at least one letter that fits R contributes its bag. Length filtering
  is done upstream in a pipe.
- PAIRS and DICT can't both be `-`.
- The inner pair counts are `pairs -m3 -x -c WR`, not DICT: words from
  the `pairs` default dictionary `$WFROOT/.wf/dict/words.filtered`
  (`WORKFLOW_DICT_PATH`), minimum length 3, exactly two words, `--exact`.
  The minimum length is hardcoded as a constant in `bag-stats.cpp`, not
  an option.

Letter counts use the `PairsCounts` representation and semantics from
`pairs-impl` (case-insensitive a-z, everything else ignored).

## Output

A text header in the style of the `bad-bags --bitmap` header, then one CSV
row per PAIRBAG in order of its first appearance in PAIRS. The format,
`BAG_STATS_MAGIC` and the write/read functions live in
`source/bag-stats-impl.{h,cpp}`:

```
bag-stats 2
letters R            (sorted)
min 3
min-wb 5
pairs PATH
dict PATH
columns PAIR,PAIRBAG,PAIRS,WORDBAGS,WB_WITH_PAIRS,TOTAL_PAIRS,MAX_PAIRS,WB_SOLO,MIN_WB,MIN_WB_WITH_PAIRS

cat dog,acdgot,2,3,2,9,6,1,1,0
...
```

The version on the first line changes whenever the columns do. Shell
pipelines skip the header with `sed '1,/^$/d'`.

| column | meaning |
| --- | --- |
| `PAIR` | the first PAIRS row with this bag, its two words space-separated |
| `PAIRBAG` | the bag's letters, sorted |
| `PAIRS` | distinct unordered PAIRS rows with this bag (`a,b` and `b,a` are one) |
| `WORDBAGS` | WORDBAGS that fit PR |
| `WB_WITH_PAIRS` | of those, how many have PC > 0 |
| `TOTAL_PAIRS` | sum of PC over those WORDBAGS |
| `MAX_PAIRS` | largest PC among them |
| `WB_SOLO` | WORDBAGS equal to PR, so WR is empty: pair + word uses all of R. Counted in `WORDBAGS`, contributes no PC |
| `MIN_WB` | WORDBAGS of at least `--min-wb-len` letters (default 5, written as the `min-wb` header line) |
| `MIN_WB_WITH_PAIRS` | of those, how many have PC >= 2 (`BAG_STATS_MIN_WB_PAIRS`) |

PC counts words, not bags, exactly as `pairs -m3 -x -c` does: a bag pair whose
sides have 3 and 2 spellings counts 6, and a bag paired with itself counts
n·(n-1)/2 combinations of its n distinct spellings. Anagram-rich remainders
score high on purpose.

The average PC is `TOTAL_PAIRS / WORDBAGS` or
`TOTAL_PAIRS / WB_WITH_PAIRS`, derived downstream.

## Sorting

`bag-stats-sort (--wb | --wbwp | --tp | --mp | --min-wb | --min-wb-frac)
[--by-length] [--pb-len N] [FILE|-]` reads bag-stats output and prints it
back, header unchanged, with the rows sorted by `WORDBAGS`, `WB_WITH_PAIRS`,
`TOTAL_PAIRS`, `MAX_PAIRS`, `MIN_WB` or the MIN_WB fraction, largest first.
The sort options are mutually exclusive.

- `--min-wb-frac` sorts by `MIN_WB_WITH_PAIRS / MIN_WB` (0 when `MIN_WB` is
  0): the fraction of MIN_WB word bags whose remainder has at least 2 exact
  pairs.
- `--by-length` sorts by the sort value's mid-rank percentile among the rows
  whose `PAIRBAG` has the same number of letters, so the best of each length
  comes first. It has no effect with `--pb-len`, which leaves one length.
  See `findings/bag-stats-min-wb-rank.md`.
- `--pb-len N` prints only the rows whose `PAIRBAG` has N letters.
Equal rows keep their order. Each option maps to a column name in the
`columns` line, and the column table `BAG_STATS_COUNTS` in
`bag-stats-impl` maps that name to the row's count.

## Implementation

### Exact pair counting, reusable

Add to `pairs-impl` an exact-count index that is built once and queried for
many bags:

- Built from the default dictionary and a minimum length passed by the
  caller (3 here), keeping words of at least that length that fit R. Words
  are grouped by `PairsCounts`, with distinct spellings per group, the same as `Pairs::add`; only each group's size is
  kept.
- `count(WR)`: for each group `a` that fits WR, `rem = WR - a`. If
  `rem == a`, add n_a·(n_a-1)/2; otherwise look `rem` up and, when it is a
  later group `b`, add n_a·n_b. This is the `options_.exact` branch of
  `Pairs::each_line`, counting instead of emitting.

`pairs` keeps its current code path. The smoke test checks the two
agree.

### Main loop

1. Build the WORDBAGS list (distinct `PairsCounts` fitting R) and the
   PAIRBAGS list (distinct bags fitting R, with distinct-row counts).
2. For each PAIRBAG: compute PR, and for each WORDBAG, test `fits(PR, wb)`,
   compute WR, and get PC.
3. PC is cached in a hash map keyed by WR. Different (PAIRBAG, WORDBAG)
   splits often leave the same WR, so each distinct remainder is counted
   once.
4. A WR with fewer than 6 letters (two 3-letter words) has PC 0 without a lookup.

Single-threaded.

### Files

- `source/bag-stats.cpp`: CLI (`optparse`, `dfs_help_*` style as in
  `pairs.cpp`) and the main loop.
- `source/pairs-impl.{h,cpp}`: the exact-count index (`PairsExactCounter`).
  `fits`, `subtract` and `letter_counts` move from the anonymous namespace
  into the header as `pairs_fits`, `pairs_subtract` and
  `pairs_letter_counts` so the new tool can use them.
- `source/meson.build`: the `bag-stats` executable linked with
  `pairs_lib`, plus a test entry.
- `source/test-bag-stats.sh`: one smoke test. It runs with a temp
  `WFROOT` holding a tiny `.wf/dict/words.filtered`, a small PAIRS file and
  a DICT, checks the expected rows, and checks that one row's
  `TOTAL_PAIRS` equals the sum of `pairs -m3 -x -c` over its WRs.
