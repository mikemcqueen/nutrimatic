# Plan: one pair file per sentence, seeds filtered on read

## Outcome

Each sentence has one pair file, `$IDX_DIR/pairs.sN`, holding every pair the
wiki index and the cluer index find for the sentence's letters over one broad
dictionary, word lengths 3 to 13. Everything that is a separate file today --
the `dict`/`words` universes, the `m3`/`m4` minimums, the `x10`/`x13`
maximums, the `85.15`/`50` probability bands -- is a filter applied when the
pairs are read:

```
view = pairs.sN
       | both words of length in [min, max]
       | both words in a dictionary (big_dict or sml_dict)
       | max(YES ab, YES ba) >= band, oriented to the larger
```

The band comes from `$IDX_DIR/p1.idx` (make-p1-index), looked up per pair.
A seed then reflects the current dictionaries and the current phase 1
results without being rebuilt: removing a dictionary word takes effect on the
next load, and new phase 1 results take effect after `make-p1-index`. Only
adding words to the dictionary, or changing a sentence's letters, needs a
pair rebuild.

A sentence rebuild writes three files (`idx.2.sN`, `cluer.sN`, `pairs.sN`)
where it wrote 26 (6 wiki-index, 6 cluer, 6 combined, 8 seeds), and runs the
cluer query once where it ran it six times. The existing files in `$IDX_DIR`
are left in place; the new names don't collide with them.

## Why the filters reproduce today's files

Checked on s7:

- `all.s7.m4.x10` is exactly `all.s7.m3.x10` keeping pairs whose words both
  have at least 4 letters.
- `all.s7.m3.x10` is exactly `all.s7.m3.x13` keeping pairs whose words both
  have at most 10 letters.
- `seed.s7.m3.all.85.15.pairs` is a subset of `seed.s7.m3.all.50.pairs`; the
  bands are [.85, 1] and [.5, 1].
- `dict.s7.m3.x10` is `all.s7.m3.x10` keeping pairs whose words are both in
  `dict.m3.x10`, plus 59,233 pairs that each contain one of the 2,113
  `dict.m3.x10` words absent from `words.m3.x10`. Building over the union of
  the two dictionaries removes that difference.

`wf extract p1 yes` keeps a pair when either order is labeled YES within the
band, and writes it in the order with the higher in-band YES. P1Index stores
each order's YES (0 when not YES), so `max(ab, ba) >= band`, written in the
larger order, is the same selection for both bands, whose tops are 1.

## The universe dictionary

`$IDX_DIR/universe` is the sorted union of `$IDX_DIR/words`, `$BIG` and
`$DICT`, keeping words of 3 to 13 letters. The pair rules depend on it
order-only: a removal from `$BIG` or `$DICT` only drops pairs the view's
dictionary filter drops anyway, so it must not start an hours-long cluer
run. After words are added, `make -f rebuild.mk -B universe wiki cluer
combine S=N` (or `rebuild.sh -f universe wiki cluer combine sN`) rebuilds.

The pair rules no longer read `dict.mM.xN` or `words.mM.xN`; those files
stay in `$IDX_DIR`.

## rebuild.mk

`rebuild.mk` is rewritten for the one pair file. Targets, for `S=N`:

- `universe`: `$(IDX_DIR)/universe`, as above.
- `wiki`: `$(IDX_DIR)/idx.2.sN`, from `universe`:
  `query-index '$(letters)' --wf -w2 -n0 --csv -m3 --dict universe`.
- `cluer`: `$(IDX_DIR)/cluer.sN`, from `universe` and `tokens.txt`:
  `comm -12 tokens universe | pairs '$(letters)' -d - -m3 | python
  query_index.py -f - -j --threads 2`.
- `combine`: `$(IDX_DIR)/pairs.sN`, from `cluer.sN`, `idx.2.sN` and, when it
  exists, `$(IDX_DIR)/sN.pairs`: `pcomm` of the three, `sort -u`.
- `all`: `combine`.

A `seed` target writing `$WFROOT/.wf/best/sN/seed.m3.85.pairs` from
`pairs.sN` and `p1.idx` (`seed-pairs --p1 p1.idx --pm .85 pairs.sN`),
the sentence seed `dfs-anagrams -t sN` resolves, is written but commented
out, and `all` doesn't name it. Those seed files are not generated; the
existing links under `$WFROOT/.wf/best/sN` are left as they are.

`DICT`, `MIN` and `MAX` go away, and with them `base_of`, `min_of`, the
range lists, the `pair_rules`/`extract_rules` loops and the `DICT`
validation finding in `findings/rebuild-mk-review.md`. The findings that
still apply carry over: reject an empty sentence number (`S=S`), write each
target to `$@.tmp` and rename it, and check `wf` and `query_index.py` at
parse time. The `wf` check stays because `cluer` is found through the `wf`
link.

`sN.pairs` becomes a prerequisite of `pairs.sN` through `$(wildcard ...)`, so
adding it rebuilds the combined file and nothing upstream.

## rebuild.sh

A new `rebuild.sh` does what `rebuild.mk` does, as a bash script, so the two
can be compared in use:

```
rebuild.sh [-f|--force] [universe|wiki|cluer|combine|all]... sN|SN
```

- The same targets, files, commands and prerequisites as `rebuild.mk`, with
  no `seed` target, commented out or otherwise. No target named means `all`.
- A target builds the targets it depends on first, as make would: `combine`
  builds `wiki` and `cluer`, which build `universe`.
- Up-to-date checks follow `rebuild.mk`: a file is rebuilt when it is
  missing, empty, or older than a normal prerequisite, and `universe` is
  never a reason to rebuild a pair file. `-f` rebuilds every named target
  and what it depends on, as `make -B` does.
- Each output is written to `OUT.tmp` and renamed on success, with the
  same setup and tool checks as `rebuild.mk`. Its helpers follow
  `rebuild-seed.sh` (`die`, `msg`, `up_to_date`, `run_to`) and its
  braces-around-the-body guard.

`rebuild-seed.sh` stays as it is.

## seed-view

A small library, `seed-view.h` / `seed-view.cpp`, holds the filter so pgui
and the CLI agree:

```cpp
// One pair of a pair file with its phase 1 YES, oriented to the larger
// order.
struct SeedPair {
  std::string line;  // "a,b", in the order with the larger YES
  float yes;         // max(YES ab, YES ba); -1 when p1 has no verdict
};

// Reads the word,word lines of `pairs`, looking each up in `p1`, which
// may be null (every yes is then -1). Lines in their file order.
bool read_seed_pairs(std::filesystem::path const& pairs,
                     P1Index const* p1, std::vector<SeedPair>* out,
                     std::string* error);

struct SeedFilter {
  int min_len = 3;                          // each word, inclusive
  int max_len = 13;                         // each word, inclusive
  std::optional<float> min_yes;             // none keeps every pair
  DfsDictionary const* dictionary = nullptr;  // null keeps every word
};

// Whether `pair` passes `filter`.
bool seed_pair_passes(SeedPair const& pair, SeedFilter const& filter);
```

A pair with no verdict passes only when `min_yes` is unset. An unoriented
pair (no verdict, or equal YES both ways) keeps its file order.

## seed-pairs

```
seed-pairs [--p1 INDEX] --pm P [-m N] [-x N] [-d FILE] PAIRS
```

prints the `line` of each pair of PAIRS that passes, in file order. `-m`
and `-x` are per-word lengths (defaults 3 and 13). `--p1` defaults to
`$IDX_DIR/p1.idx`. It is how any CLI use gets a seed:
`dfs-anagrams ... --seed-pairs <(seed-pairs --p1 $IDX_DIR/p1.idx --pm .5 $IDX_DIR/pairs.s1)`,
and what `rebuild.mk`'s commented-out `seed` target runs.

## pgui

The seed field names a pair file, `pairs.sN`. `load_app_state` opens
`seed_directory/p1.idx`. When P1Index::open fails, its error is printed and
every pair has no verdict, so the `seed50*` and `seed85*` views are empty;
the source dropdown offers `all`, `seed50m3`, `seed50m4`, `seed85m3` and
`seed85m4` either way. Enter in the seed field reopens `p1.idx` when it's
newer, keeping the old index when it can't be opened, and rereads the pair
file so new YES values reach the views. `seed_key` is always `seed85m4`:
the first column's source, and what a seed column falls back to. pgui starts on `idx/pairs.s1` rather than
`idx/seed.s1.m4.all.85.15.pairs`.

`AppState` replaces `seed_map`/`seed_files`'s file per key with the pair
file read once into a `std::vector<SeedPair>`, and views of it, each under a
band key and a dictionary key, `sml_dict` or `big_dict`: at most ten. One
`seed_bands` table of `{key, min_yes, min_length}` names the bands: `all`
(`min_yes` unset, m3), `seed50m3`/`seed50m4` (.5) and `seed85m3`/`seed85m4`
(.85); it feeds the source dropdown, the default and the filter. A column's seed is
the view under its source's band key and its D: dictionary, so D: is both
the dictionary a column's command runs with and the one its seed is
filtered by. A view is built the first time a column asks for it, and built
again, under a new `seed_file_version`, when a column asks for it after the
pair file, its dictionary (reloaded by `refresh_dictionaries`) or the `x`
field has changed since it was built. Columns whose source is a seed rerun
when that version changes, as they do when a seed file changes today.
`refresh_seed_files` takes each seed column's band key and D: key and drops
the views none of them name.

The seed line gains an `x` field beside the seed field, the per-word max
length for every view, starting at `SeedFilter`'s 13; Enter in it applies
it. The per-word min comes from the band. The pair file's mtime is checked where seed file mtimes are checked
today, and a newer file is read again.

D: offers `sml_dict` and `big_dict` in every command, and no columns; it
still starts at `sml_dict`, so a seed column's seed starts filtered by it.
bag-stats' second line gains, after `ss:`, a `wb:` dropdown of `sml_dict`,
`big_dict` and the number of each column before this one, naming the words
its `wb` steps pick from; choosing bag-stats sets it to the column before,
as it sets D: today. Column does for a `wb:` column what it does for a D:
column today: waits for it to finish, and reruns when its output changes.
`Column::Key` holds the `wb:` choice and, when it names a dictionary, that
dictionary's version, so choosing another `wb:` or reloading its dictionary
reruns the column.
`has_columns_in_dictionaries` becomes `has_word_bag_source`, and
`LetterToolsParams` gains `word_bag_lines`, the `wb:` dictionary's lines or
column's output; `dictionary` and `dictionary_lines` are always D:'s.
bag-stats' D: thus names the dictionary its `pb` and `pf` pairs, its seed,
are filtered by; its final `p` still counts pairs in `big_dict`. bag-stats'
`uses_dictionary` becomes false: it no longer reads D:'s dictionary, and a
D: reload reaches a seed source through the view's version.

Idea: a view could hold `std::vector<std::reference_wrapper<std::string const>>`
to the passing pairs' `line`s instead of copying each into a `Lines`, so the
views cost a pointer per pair rather than a string. `SeedPair` keeps
the pair as one `line` with a `comma` offset, not two word strings, so a
reference to `line` is the whole pair a column reads. The pair file's vector
is held by `std::shared_ptr`, and each view holds a copy of that pointer
beside its references, so reading a newer pair file replaces the vector
without invalidating the lines of columns that haven't rerun yet.

`get_seed_key`, `get_seed_universe`, `build_seed_name`,
`add_seed_if_exists` and `add_other_seed_files` are deleted. `seed_m` goes;
`column.cpp`'s fallback to `"seed85" + seed_m` becomes `"seed85m4"`, and
`BagStats::default_seed` stays `"seed50m3"`. The sentence for bad bags still
comes from the seed name's `sN` component, which `pairs.sN` has.

## Scripts and docs

`setup.sh`'s `SEED` and `SEED50` become shell functions, `seed` and
`seed50`, running
`seed-pairs --p1 $IDX_DIR/p1.idx --pm .85 -m 4 $IDX_DIR/pairs.s${S}`
(and `.5`) to stdout.

`docs/best-pairs-workflow.md`'s pair-generation steps and `docs/file-types.md`
are rewritten for `universe`, `pairs.sN`, `seed-pairs`, `rebuild.mk` and
`rebuild.sh`.

Nothing in `$IDX_DIR` is deleted.

## Tests

Smoke tests only:

- `test-seed-pairs.sh`: a small pair file and a test p1 index built by
  make-p1-index from a two-line jsonl; checks `--pm`, `-m`/`-x`, `-d`
  and that the output takes the larger YES order.
- rebuild.mk, rebuild.sh and pgui get no automated tests.
