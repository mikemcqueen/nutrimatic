# Plan: bag-stats --ss pf

## Outcome

```
bag-stats [-u LETTERS] [-s N | --no-nopairs] [--min-wb-len N] --ss pf
          [--exact] [--threads N] LETTERS PAIRS DICT
pfilter ... [--exact] [--no-nopairs] [--no-dict] ...
```

For each DICT word that fits LETTERS (less -u, at least --min-wb-len
letters), take the remainder R = LETTERS less the word and count the PAIRS
rows pfilter would keep with `-l R --no-nopairs --no-dict -x 0` (plus
`--exact` when given): rows whose letters fit within R, or with --exact
equal R. Print one summary row.

## pfilter

- `--exact`: keep only pairs whose letters are exactly the -l letters (less
  -u). Needs -l.
- `--no-nopairs`: skip the global and -s no.pairs lists. -y still drops the
  yes lists. No "--sentence not supplied" alert when neither list is read.
- `--no-dict` already exists.

## bag-stats pf mode

- `--ss pf` is allowed only alone. `--exact` goes only with it.
- PAIRS drops the global no.pairs and -s N's, as in every mode, through
  load_pair_bags(); pfilter itself does no no.pairs filtering. No DICT
  check, and pfilter's letter maximum is 0, no limit. Rows are deduped as `a,b` = `b,a`, as
  bag-stats does elsewhere.
- Word bags come from load_word_bags() with spellings, so each bag knows its
  DICT words. Anagrams leave the same R, so each bag is counted once and
  weighted by its spellings for the word columns.
- Per word bag: pair rows and pair bags that fit R (or equal R with
  --exact). Fit mode tests every pair bag with pairs_fits(), word bags split
  across --threads; exact mode looks R up in a hash of pair bags.
- `--no-nopairs`, in every mode, keeps the no.pairs; -s doesn't go with
  it.
- Rejected with pf: --pp, -n, SORT options, --score and its options,
  --best-pairs, --top-segments, the prior options. bag-stats-cuda rejects
  pf.

## Output

Two lines, the column names and the row:

```
WORDS,WORDBAGS,W_WITH_PAIRS,WB_WITH_PAIRS,W_WITH_MIN_PAIRS,WB_WITH_MIN_PAIRS,W_SOLO,WB_SOLO,TOTAL_PAIRS,TOTAL_PAIR_BAGS,MAX_PAIRS,MAX_PAIR_BAGS
```

- W_* count DICT words, WB_* word bags.
- WITH_PAIRS: R fits at least one pair row; WITH_MIN_PAIRS: at least
  BAG_STATS_MIN_PAIRS rows.
- SOLO: the word uses all of LETTERS.
- TOTAL_*: summed over word bags; MAX_*: the most for one word bag.
  *_PAIRS count rows, *_PAIR_BAGS distinct pair letter bags.

## Tests

One pfilter smoke test for --exact and --no-nopairs; one bag-stats pf
smoke test each for fit and --exact.
