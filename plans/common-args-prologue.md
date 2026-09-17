# One prologue for the three `DfsCommonArgs` tools

## Summary

Three helpers resolve `DfsCommonArgs` after option parsing, and their relative
order matters: `finalize_dfs_bonuses` validates raw scalar bonuses and
normalizes the effective legacy pair bonus,
`finalize_dfs_workflow_args` then resolves the workflow defaults and weighted
pair sources, and `collect_workflow_exclude_pair_files` reads what that
resolution named. The recently landed bonus normalization put the first two
stages in the same order in all three tools. The current sequences are:

- `dfs-anagrams.cpp` — bonuses, workflow, collect at adjacent call sites
- `query-index.cpp` — bonuses, workflow, collect at adjacent call sites
- `rerank-anagrams.cpp` — bonuses, workflow, then collect later at :181

The bonus and workflow stages now have one order, but that order is still
copied at every call site and collection remains separate in rerank-anagrams.
The intended invariant is stronger: every tool must finalize scalar bonuses,
resolve workflow pair sources, and collect the resulting exclusions through
one shared sequence before it prepares scoring inputs.

Give the three helpers one caller. `finalize_dfs_common_args` runs them in a
fixed order, the three tools call it once, and the helpers stop being callable
individually. A fourth tool then gets the order by construction rather than by
copying whichever neighbor it read first.

These are the only three callers repo-wide, and the only three constructors of
`DfsCommonArgs` (dfs-anagrams.cpp:219, query-index.cpp:229,
rerank-anagrams.cpp:98), so nothing else has to move.

## Interface

`dfs-cli-args.h` gains one declaration and loses three:

```cpp
// Resolves the common arguments after option parsing: raw scalar bonuses are
// validated and the effective legacy pair bonus is normalized, then the
// workflow defaults and weighted pair sources are resolved, then the
// workflow's exclude-pair files are appended to `exclude_pair_files`.
// A NULL `exclude_pair_files` skips that last step for a caller that excludes
// nothing. The order is fixed here because each step reads what the one
// before it resolved.
bool finalize_dfs_common_args(
    DfsCommonArgs* args, char const* program, char const** index_file,
    std::vector<std::string>* exclude_pair_files);
```

`finalize_dfs_bonuses`, `finalize_dfs_workflow_args` and
`collect_workflow_exclude_pair_files` become file-static in
`dfs-cli-args.cpp`, keeping their bodies and their comments, which move with
them. That is what enforces the order: after this there is no way to call them
apart, so no future tool can sequence them wrongly.

Two header comments name a function that is no longer declared there. The one
on `load_dfs_workflow_target_settings` (dfs-cli-args.h:128) and the one on
`collect_workflow_exclude_pair_files` — the latter departing with its
declaration — say "Call after `finalize_dfs_workflow_args()`". The surviving
comment says `finalize_dfs_common_args()` instead.

`finalize_min_word_length` stays declared and stays at each call site. It is
ordered against the letters a tool has resolved, not against this prologue,
and the three tools source their letters differently.

## Call sites

- `dfs-anagrams.cpp:314-320` becomes one call passing
  `&out->exclude_pair_files`. Its order is already bonuses, workflow, collect,
  so nothing moves.
- `query-index.cpp:315-322` becomes one call passing
  `out->score ? NULL : &out->exclude_pair_files`, which is how the existing
  `!out->score` condition on the collect survives. The bonus normalization
  change already moved `finalize_dfs_bonuses` directly before
  `finalize_dfs_workflow_args`, so the three calls are adjacent and collapse
  into one.
- `rerank-anagrams.cpp:164-168` becomes one call passing
  `&out->exclude_pair_files`, and the trailing
  `collect_workflow_exclude_pair_files` at `:181` goes away. The function then
  ends `return finalize_min_word_length(...);`.

## Behavior

One ordering changes. rerank-anagrams runs the collect last today, after
`load_dfs_workflow_target_settings` (`:170`) and `finalize_min_word_length`
(`:177`); under this change it runs inside the prologue, before both. The
collect reads
`workflow_root` and `target_complete`, both assigned by
`finalize_dfs_workflow_args`, and neither of the two functions it moves ahead
of assigns either field — `load_dfs_workflow_target_settings` writes a
separate `DfsWorkflowTargetSettings`, and `finalize_min_word_length` touches
only the letters and `min_word_len`. So the same paths are collected from the
same resolution.

What that changes is which diagnostic prints when an invocation is invalid
twice over: a rerank-anagrams run with both an unreadable workflow
`no.pairs` and a target whose `mN` minimum is too large for its derived bag
reports the pair-file error after this change and the minimum-length error
today. The exit status is 2 either way.

No other order moves, and the resolved values are unchanged everywhere.

## Tests

`test-dfs-cli.sh:957-961` checks the exact negative-word-bonus diagnostic,
while `test-query-index.sh:778-781` already checks that both negative word and
pair bonuses fail in score mode. Do not add a duplicate query-index case.

The dfs-anagrams and query-index smoke tests already exercise automatic
NO-pair collection, including query-index's score-mode skip. Add one focused
case to `test-rerank-anagrams.sh`: make the complete target's `no.pairs` a
directory, assert that reranking the existing saved candidates exits 2 with
the target-NO "not a regular file" diagnostic, then remove the directory so
the remaining cases keep their current fixture. This directly covers the
rerank call site's newly moved collection without broadening the suite.

## Build

Nothing to change. `dfs-cli-args.cpp` is already a source of
`dfs_class_list_lib` (source/meson.build:95), which all three CLIs link, and no
file is added or removed.

## Validation

```bash
source ./setup.sh
source .env/bin/activate
source build/dep-info/conanbuild.sh
meson compile -C build dfs-anagrams query-index rerank-anagrams
meson test -C build dfs-cli query-index-cli rerank-anagrams --print-errorlogs
git diff --check
```

## Not in scope

- `finalize_min_word_length`, and the letters each tool feeds it. dfs-anagrams
  and query-index take a positional argument; rerank-anagrams derives them
  from `load_dfs_workflow_target_settings`, which must sit between the
  prologue and the minimum-length call.
- The letter-bag quartet — `clean_letters` twice, `subtract_letters`,
  `check_bag_length` — duplicated at dfs-anagrams.cpp:342-350 and
  query-index.cpp:352-358. It is a separate extraction with its own shape, and
  rerank-anagrams has no use for it.
- query-index's `--near` (`:300`) and `--score` (`:328`) early returns, which
  leave `parse_args` before its remaining checks. The prologue fixes the order
  of the steps a tool runs; it does not make every tool run every step.
