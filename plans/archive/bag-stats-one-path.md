# Plan: one bag-stats search path for p and pf

## Outcome

Every `--ss` step adds exactly one thing, and both final steps run through
the same row walk:

```
--ss [ppb,]...[pb,]...[wb,]...p|pf
```

- `ppb` a --pp bag, `pb` a PAIRS bag (at least --min-pb-len letters), `wb`
  one DICT word bag, in any order and any number, as today.
- `p` last: DICT pairs (`pairs -m3 -x -c`) that use exactly what is left.
- `pf` last: PAIRS rows, of any length, that use exactly what is left with
  `--exact`, or that fit in it without.

| `--ss`     | before                           | after                    |
|------------|----------------------------------|--------------------------|
| `pf`       | word + PAIRS row                 | PAIRS row                |
| `wb,pf`    | word + word + PAIRS row          | word + PAIRS row         |
| `pb,wb,pf` | PAIRS bag + word + word + row    | PAIRS bag + word + row   |
| `wb,p`     | rejected                         | word + DICT pair         |
| `pb,wb,p`  | PAIRS bag + word + DICT pair     | unchanged                |

The "at least one pb and one wb" rule goes: any step counts may be 0.

## One path

`count_outer_bags`, `PairFilterInputs`, `PairFilterCounts`,
`PairFilterResult`, `count_pair_filter` and `run_pair_filter` go away.

- **Final step.** RestCounter and CompletionCollector count a word rest's
  pairs through one interface with two implementations: the DICT pairs of
  today (PairCountTable + InnerGroups), and PAIRS rows (a hash of every
  PAIRS bag with --exact, otherwise a scan of the ones that fit). PAIRS'
  bags for pf are loaded unfiltered by --min-pb-len, beside the pb list.
  With pf no PairCountTable, inner dictionary or RestTable sizing for DICT
  pairs is built.
- **No pick steps.** PickWalk with no pair steps has one unit and one row,
  whose rest is the whole bag; RowWalker keeps one fitting list.
- **Rows.** Every search prints the bag-stats header and rows as `p` does
  today: `PAIR,PAIRS,WORDBAGS,WB_WITH_PAIRS,TOTAL_PAIRS,MAX_PAIRS,WB_SOLO,
  WB_WITH_MIN_PAIRS`. With no pick steps the one row's PAIR is empty and
  PAIRS 1. -n, SORT options, --score, priors and bag-stats-sort work on pf
  rows as on p rows; ranking options are rejected without pick steps.

## Listings

- `--completions N` lists the N completions with the most pairs below the
  row: with --score as today, and without pick steps below the one row,
  only those with pairs, or SOLO.
- `--all` is `--completions` of every completion with pairs, or SOLO;
  same places.
- Each completion is its word bags, `w1 + w2` with two wb steps, as
  --completions prints today; key `PC`.
- `pb,wb,pf --all`'s list of PAIRS bags by pairs is now `-n N --tp` rows.
- `--sort TP|UP` goes: one wb step reaches each PAIRS bag once per word
  bag, so UP equals TP.
- The pf summary row (W_ columns, TOTAL_PAIR_BAGS, MAX_PAIR_BAGS, the
  UNIQUE columns) goes.

## Options

- `--exact` goes with a final pf. `wb` steps no longer need it.
- `--progress`, `--cache-size`, `--pair-cache-size` go with pf too.
- bag-stats-cuda rejects pf and searches without pick steps.

## pgui

The bag-stats column runs the same walk: `pf` with neither toggle, `wb,pf`
with [wb], `pb,wb,pf` with [pb] and [wb]; [pb] stays enabled only with
[wb]. [wb] and [pb] no longer need [ex].

- `pf`: the PAIRS rows that fit the letters, or with [ex] use them all,
  each with its bag's pairs.
- `wb,pf`: the row's completions, each spelling with its pairs or SOLO, as
  today.
- `pb,wb,pf`: the rows by TOTAL_PAIRS, each PAIRS bag's rows with its
  pairs, as today.

On the command line, `--ss pf --all` and `--completions N` list the same
PAIRS rows below the one row: with no wb steps a completion is a PAIRS bag
the final pf reaches, printed as its rows.

## Docs and tests

`--help` text for --ss, --completions, --all and --exact. Smoke tests in
test-bag-stats.sh: replace the pf cases with `pf`, `wb,pf` (exact and
fit), `pb,wb,pf -n --tp`, and `wb,p`.
