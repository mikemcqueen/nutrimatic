# Ranked pairs

Adds a pair-level cut to `rebuild-pairs.py`: the top N thousand pairs of a
step's `pairs.ranked` by count x prior^2, written as `pb3/pp-1sq-5k`. The prior
is defined by a file in `idx/`, so adding terms to `pair-prior` later doesn't
change what an existing cut means.

The cut is one pipeline:

    pfilter --ranked PARENT/pairs.ranked |
      pair-prior --ranked --manifest idx/pp.1sq --as-pairs -n 5000 -

## Ranked format

A ranked row is `COUNT word word`, the format of `pairs.ranked`. COUNT is a
`uint64` and leading spaces are skipped.

`--ranked` is a switch with the same meaning in `pfilter` and `pair-prior`: the
single positional argument is a file of ranked rows, `-` for standard input.

The parser and row reader are shared. `pfilter` and `pair-prior` already read
through `read_rows` and `print_kept_rows` (`row-input.h`) on top of
`parse_pair_row`. Add `parse_ranked_row` beside `parse_pair_row`, using
`parse_number` for COUNT. A bad row reports
`error: WHAT "FILE" line N: expected COUNT word word`, the format of
`parse_pair_row`.

No new reader is added. In `read_rows` and `print_kept_rows`, the
`allow_single_words` bool becomes a `RowFormat` of `pairs`, `pairs_or_words` or
`ranked`. Each caller changes that one argument.

`DfsPairRow` doesn't change. `parse_ranked_row` outputs the count separately.
The `read_rows` visitor takes it as a new first argument, `uint64_t count`,
which is 0 for the other formats, and each visitor's parameter list gains it.
`print_kept_rows`'s `keep` doesn't change.

## pfilter

`--ranked` applies every existing filter to ranked rows and prints each kept row
unchanged, in input order. It can't be combined with `--cv`.

## pair-prior

`--ranked`:

- takes exactly one positional argument
- keeps the existing dedup of unordered pairs, so the first row of a pair wins
- scores each kept row as COUNT x prior
- prints `SCORE word word`, sorted by descending score, ties by pair
- sorts on the unrounded score and prints it rounded to `uint64`, left-justified
  with one space after it, so `top-segments --by-count` can read it
- can't be combined with `--features`

`--as-pairs` requires `--ranked` and prints `word,word` in the same order, with
no score.

`-n N` requires `--ranked` and stops output after the first N rows of that
order. `-n 0` prints all rows.

`--manifest FILE` reads the prior definition from FILE. It can't be combined
with `--prior-weights` or `--features`. It can be used without `--ranked`.

## Manifests

A manifest is a file of `TERM=WEIGHT` lines, plus an optional
`options=squared` line. `options` is reserved. `squared` squares the prior
before any multiplication by COUNT: `COUNT x prior^2`.

An unknown term or option is an error, and so is a negative weight. A term whose
index can't be opened is an error, unlike the default weights, which drop that
term with a warning. An out-of-date index is a warning and the cut goes on.

`--manifest` calls `open_prior` directly, not `open_default_prior`, with the
default index paths. `open_prior` takes a label for its errors, which is
`--prior-weights` for existing callers and the manifest path here, so every
manifest error names the manifest.

Two manifests are created in `idx/`. Both hold the current default terms:

    mean=1
    exact=1
    adjacent=1
    adjacent-short=1
    link=1
    link-short=1
    rare-answer=1

`idx/pp.1` is exactly that. `idx/pp.1sq` adds `options=squared`.

A manifest doesn't change once a cut uses it. A new term set gets a new name.

Versioning the `idx/pp.X` files is settled: they live unversioned in the
gitignored `idx/`, and that is not an issue.

## rebuild-pairs.py

A new cut token, `pp-NAME-Nk`, is accepted wherever `Nk` is, with
`PP_CUT_RE = pp-([0-9a-z]+)-([1-9][0-9]*)k`. `NAME` selects `idx/pp.NAME`, and
`Nk` is how many thousand rows of the tool's output to keep. Its output is the
same `pairs` file a word cut writes, so steps below it work unchanged.

The cut step's command is:

    pfilter NO_PAIRS_FLAG -x0 --ranked PARENT/pairs.ranked |
      pair-prior --ranked --manifest idx/pp.NAME --as-pairs -n N000 -

It depends on the parent's `pairs.ranked`. Its externals are `pfilter`,
`pair-prior`, `idx/pp.NAME`, `idx/p1.idx` and `cluer/data/cluedata`.

### Length cap

Only a `pbN` step at the root of a path accepts the `-xN` suffix. A cut, word or
`pp`, always runs its `pfilter` with `-x0`. `parse_path` rejects `-xN` on a
`pbNwb` step or on any step below the root with a `bad path` error.

### Docs

`plans/rebuild-pairs.md`:

- Paths: a path alternates steps and cuts, and `pp-NAME-Nk` joins the list of
  tokens
- `-xN` is accepted on a root `pbN` only, as in the cap above, and "Both apply
  to that step alone" stays true of it
- Files: `pp-NAME-Nk` writes `pairs`
- Rebuilds: the recorded externals also list a `pp` cut's `idx/pp.NAME`,
  `idx/p1.idx` and `cluer/data/cluedata`

The `rebuild-pairs.py` docstring and the `parse_path` `bad path` message list
`pp-NAME-Nk` beside `Nk`.

## Count justification

`top-segments` prints COUNT left-justified with no padding, except `--elim`,
which stays an aligned table. This makes every new `pairs.ranked`, including the
`pb2wb` ones built through `top-segments --pairs -c`, the same format.

## Tests

Smoke tests only:

- `test-pfilter.sh`: `--ranked` keeps a passing row unchanged and drops a failing
  one
- `test-pair-prior.sh`: `--ranked` order and score, `--as-pairs`, and a manifest
  with `options=squared`
- `test-top-segments.sh`: a padded count is gone from `-c` output
