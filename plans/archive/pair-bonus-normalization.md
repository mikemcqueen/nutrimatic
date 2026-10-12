# Finalize DFS bonuses consistently before scoring

## Summary

Before any tool loads scoring inputs or constructs `DfsScoreModel`, all
bonus-related arguments are finalized through one shared sequence. Raw scalar
bonuses are validated first, an inapplicable legacy pair bonus is normalized
next, workflow pair sources are resolved in a fixed position after that, and
every scoring consumer receives the resulting effective bonus configuration.

`--pair-bonus` means nothing without a `--pairs` file. Two places say so
today:

- `query-index.cpp:954`, with
  `if (args.common.pair_file == NULL) args.common.pair_bonus = 0.0;`
  after `parse_args` returns.
- `dfs-class-list-build.cpp:108`,
  `double const pair_bonus = args.pair_file == NULL ? 0.0 : args.pair_bonus;`,
  which re-derives the rule for every caller of `prepare_dfs_scoring_inputs`.

`rerank-anagrams` states the rule nowhere. It consumes only `prepared.model`
and is correct because of the gate at :108 alone.

The three CLIs already call `validate_solo_bonuses` once after option parsing
(dfs-anagrams.cpp:314, query-index.cpp:327, rerank-anagrams.cpp:164). Replace
that validator with `finalize_dfs_bonuses`: first validate the raw bonuses,
then clear `pair_bonus` when `pair_file` is null. Each tool calls it before
`finalize_dfs_workflow_args`, so scalar bonuses are finalized before workflow
pair sources are resolved. The dfs-anagrams and rerank-anagrams calls stay at
their current positions; the query-index call moves to immediately before its
workflow finalization. The gate therefore has one implementation and the
bonus-finalization stages have one order across all three tools.

This placement is safe. `pair_file` is set only while parsing options;
`finalize_dfs_workflow_args` does not assign it. Workflow handling then appends
weighted seed, YES, and BEST sources. Those are separate from the legacy
`--pairs` file and already conflict with it. Finally,
`prepare_dfs_scoring_inputs` loads the resolved sources and constructs the
shared score model consumed by all three tools.

`query-index`'s `--near` branch returns before the existing solo-bonus
validator, then `main()` enters `run_near_query` at query-index.cpp:952 before
the gate at :954. It therefore bypasses both validation and normalization
today and will continue to bypass the combined finalizer. Pair-related options
are rejected as near-incompatible.

## Interface

Rename the shared declaration and definition from:

```cpp
bool validate_solo_bonuses(DfsCommonArgs const& args);
```

to:

```cpp
bool finalize_dfs_bonuses(DfsCommonArgs* args);
```

The implementation keeps the current `--solo-words` checks in their current
order. It must not return early when `solo_words` is empty; after any required
validation succeeds, it applies:

```cpp
if (args->pair_file == NULL) args->pair_bonus = 0.0;
```

The header comment states that raw solo-word bonus constraints are checked
before a missing `--pairs` file clears the effective pair bonus. The three
callers pass `&out->common` before calling `finalize_dfs_workflow_args`.

`query-index.cpp:954` is deleted, and the gate at
`dfs-class-list-build.cpp:108` becomes `args.pair_bonus`.

## Ordering

dfs-anagrams and rerank-anagrams continue to finalize scalar bonuses before
`finalize_dfs_workflow_args`. query-index moves its finalization to the same
position, before workflow finalization,
`collect_workflow_exclude_pair_files`, and the `index_file` check. All three
tools therefore validate `word_bonus`, validate `pair_bonus`, normalize
`pair_bonus`, resolve workflow pair sources, and only later prepare scoring
inputs in that order.

This changes query-index's diagnostic precedence for multiply invalid
invocations: a negative solo bonus now reports before pair-source conflicts,
WFROOT and target errors, missing workflow or target directories, missing
workflow pair files, or a missing `-i`. Within `finalize_dfs_bonuses`, checking
the raw value before clearing it preserves the negative `--pair-bonus` error
when `--solo-words` is active without `--pairs`.

## Behavior

These readers all sit downstream of one of the two current gates and see the
same value after the move:

- `query-index.cpp:994`, the `prepare_dfs_scoring_inputs` arguments, and the
  two branches at :1047 and :1061 that choose plain count order over score
  order, all within `main()` (932-1071).
- `query-index.cpp:814`, in `score_value_list`, reached from :957.
- `dfs-anagrams.cpp:392` and `rerank-anagrams.cpp:388`, both
  `prepare_dfs_class_list`.

The `--near` path bypasses normalization both before and after the change:
`query-index::parse_args` returns from its near branch before calling the old
validator or the new finalizer, and `main()` enters `run_near_query`
(query-index.cpp:606-641) at :952 before the existing main-level gate.
Pair-related options are rejected as near-incompatible, and the unread raw
default therefore remains unchanged and has no effect on output.

## Tests

No successful test passes a positive `--pair-bonus` without `--pairs`. Every
such use in test-dfs-cli.sh and test-query-index.sh supplies `--pairs`
alongside it. Existing no-pairs exceptions are test-dfs-cli.sh:776, which
passes `--pair-bonus 0` where the value is already zero;
test-query-index.sh:308, which rejects a malformed value; and
test-query-index.sh:780-781, which passes `--solo-words cd --pair-bonus -1`
and expects status 2. Preserve the negative case: it directly requires
`finalize_dfs_bonuses` to validate the raw value before normalization clears
it.

Add one case to test-dfs-cli.sh: `dfs-anagrams --pair-bonus 1` with no
`--pairs` produces output identical to the same invocation without the flag.

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

- `finalize_dfs_workflow_args`; its responsibilities and call ordering remain
  unchanged except that query-index now calls bonus finalization before it.
- Changing `--word-bonus` semantics. It participates in the shared raw-bonus
  validation sequence but has no file-dependent normalization.
- Changing weighted-pair tier semantics. Their sources are resolved after the
  scalar bonuses are finalized, their conflict with `--pairs` is already
  checked at dfs-cli-args.cpp:929, and their bonuses are not scalars on
  `DfsCommonArgs`.
