# `dfs-anagrams --ptm` on one score model

## Summary

`--ptm` recalibrates the base count term every index entry contributes, before
any bonus, onto the scale the available counts would carry with a normal upper
tail. `findings/ptm-base-score.md` records why it maps back to log-count units
and why it attaches between phase 1's member sort and phase 2.

It currently reaches phase 2 by a `DfsBaseRemap const*` threaded into
`DfsAnagramSearch`, because the search does not use the model it is given: it
takes `segment_penalty`, `corpus_total`, `word_bonus`, `pair_bonus` and
`best_bonus` and constructs a second `DfsScoreModel`, while `DfsTopN` borrows
the caller's. A result's score is assembled from both — a representative from
the search's per-class bounds plus a per-segment difference from the output's
model — so state held by only one of them prints a mixture rather than falling
back to plain scoring.

This removes the duplication. `DfsAnagramSearch` takes the model,
`DfsSearchData` stops carrying one, and `--ptm` needs no plumbing at all: the
remap is part of the model, so it rides along.

There are three `DfsScoreModel` values today: the one `DfsPreparedClassList`
owns, the one `DfsAnagramSearch` holds, and the one `DfsSearchData` holds. The
third is read in exactly one place, dfs-all-runner.cpp:76, for a constant that
already sits beside it in the same struct.

## Interface

`dfs-search.h`:

```cpp
  DfsAnagramSearch(DfsClassList const* classes, std::string const& letters,
                   DfsScoreModel const& score_model,
                   size_t score_cache_bytes = 0,
                   size_t preprocess_threads = 1,
                   size_t search_threads = 1,
                   size_t exact_segments = 0);
```

The five scalars and the `base_remap` pointer are removed. The scalars occur
nowhere in `dfs-search.cpp` outside the model construction, and `base_remap`
exists only to patch the divergence this plan deletes. The trailing arguments
keep their order, so call sites passing cache size and thread counts
positionally are unaffected.

`DfsAnagramSearch` drops its `DfsScoreModel const score_model` member and
retains no model. It keeps `segment_boundary_log_score` and
`best_member_upper_log_scores`, both computed in the constructor from the
supplied model, which is what phase 2 actually reads.

`dfs-search-data.h` drops `DfsScoreModel score_model{1.0, 1, 0.0};`. The dummy
initializer is there because `prepare_phase_two` resets with `*data =
DfsSearchData()`, which requires the struct to be default-constructible; that
same assignment is why the field cannot become a reference. Removing the field
settles both and shrinks a struct that is moved into a runner once per call.

`dfs-search.cpp` loses `make_search_score_model` and the
`data->score_model = score_model;` line in `prepare_phase_two`.

## Behavior

dfs-all-runner.cpp:76 reads the model once:

```cpp
                  : data.score_model.append_log_score(
                        representative_log_score, class_score);
```

It becomes

```cpp
                  : representative_log_score +
                        data.segment_boundary_log_score + class_score;
```

which is what `append_log_score` expands to, in the same association:
`(accumulated + boundary) + segment`. Scores stay bit-identical.
dfs-search-projected.cpp:412 already forms its bound from
`data.segment_boundary_log_score` this way, so the two runners come to agree
rather than diverge.

No score changes anywhere. The model the search prunes with is the model the
class list ordered its members under, which is what the constructor's existing
comment already claims.

## Call sites

Twenty-seven constructions.

- `dfs-anagrams.cpp:432` passes `*prepared.model` and drops the five scalars,
  `best_bonus` and `prepared.base_remap.get()`. `dfs_best_bonus_policy` is
  still called by `prepare_dfs_scoring_inputs` to build the model; the local
  `best_bonus` at dfs-anagrams.cpp:430 goes away with the argument. Dropping
  the scalars also removes this file's last reader of the raw
  `args.common.pair_bonus`: `prepare_dfs_class_list` at dfs-anagrams.cpp:392
  reaches the field only through `prepare_dfs_scoring_inputs`, which ignores
  the raw value when `pair_file` is NULL. So `dfs-anagrams.cpp:371`,
  `if (args.common.pair_file == NULL) args.common.pair_bonus = 0.0;`, becomes
  a dead assignment and goes with them.
- `query-index.cpp:990` passes `model`, already bound at query-index.cpp:978 as
  `DfsScoreModel const& model = *prepared.model;`.
- `test-dfs-output.cpp:571` passes `model`, already constructed above it for
  its `DfsTopN`.
- `test-dfs-search.cpp` holds the remaining 24, in three scopes. Twenty-two of
  them already have a model in scope and simply pass it: the one at
  test-dfs-search.cpp:113 covers the twenty constructions from line 116 to line
  531, and the one at test-dfs-search.cpp:566 covers lines 569 and 578. Those
  are the two the file already constructs, so nothing new is declared beside
  them. `validate_14_letters()` has no model and gains exactly one
  `DfsScoreModel const model(DFS_DEFAULT_SEGMENT_PENALTY, reader.count(), 0.0);`
  for both of its searches, which share one `reader` and differ only in class
  list.

## `--ptm` after the change

`dfs-class-list-build.{h,cpp}` keep the `ptm` parameter, the fit over
`member_log_counts()`, the install onto the prepared model and the
`resort_members` that follows it. `DfsPreparedClassList::base_remap` stays: it
owns the fitted map the model borrows. `dfs-score.{h,cpp}`, `tail-map.{h,cpp}`
and the `--ptm` option itself are untouched. The only `--ptm` code this plan
deletes is the constructor argument.

## Build

`meson.build` does not change. `tail-map.cpp` is already in
`dfs_class_list_lib` and no source is added or removed.

## Validation

```bash
source ./setup.sh
source .env/bin/activate
source build/dep-info/conanbuild.sh
conan build .
meson test -C build
git diff --check
```

All 19 tests pass, `dfs-search` and `dfs-output` among them, and the `--ptm`
smoke test in `test-dfs-cli.sh` with them.

The refactor changes no score, so the output is compared directly against the
current build. Against a `make-dfs-test-index` index, with and without `--ptm`:

```bash
dfs-anagrams -i test.index abcd -m 2 -n 6 --word-bonus 0
dfs-anagrams -i test.index abcd -m 2 -n 6 --word-bonus 0 --ptm
dfs-anagrams -i test.index abcdghij -m 2 -n 6 --word-bonus 0 --ptm
```

must print byte-identical stdout before and after. The `--ptm` run scores
`ab cd` at `60.33883` against a plain `70.00000`, and its `abcdghij` run orders
`ba,cd` above `ab,dc`.

## Not in scope

- Fitting the map over the whole index instead of the bag's class list, which
  would make the base a property of the index and end the batch-relativity
  recorded in `findings/ptm-base-score.md` and `docs/todo`.
- `--ptm` for `rerank-anagrams` or for `query-index` scoring.
- `DfsTopN`, which already borrows the caller's model and needs no change.
