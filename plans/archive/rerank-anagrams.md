# Add workflow-aware `rerank-anagrams`

## Summary

Create a new `rerank-anagrams` tool rather than expanding
`filter-segments`. It will revalidate, rescore, and sort an existing DFS
candidate pool using current workflow inputs. `--show-bonus` controls only the
additional marker column.

With unchanged inputs, ordinary output must be byte-identical to the input and
`--show-bonus` output must match an equivalent `dfs-anagrams --show-bonus`
run. With changed inputs, results are limited to candidates present in the
saved file; omitted candidates cannot be backfilled.

## Interface

```text
rerank-anagrams (--wf | --wfroot DIR) -t FULL-TARGET
  [-r FILE]... [--best-pairs FILE]... [--show-bonus] [FILE]
```

- Require exactly one workflow-root option and a fully scoped
  `sN/[ou]-letters/mN/gN` target.
- Read one ordinary, unannotated DFS result file, or stdin when omitted or `-`.
- Load the workflow index, dictionary, sentence seed, classified YES,
  classified NO, target NO, target letters, and target BEST implicitly.
- When one or more `--best-pairs` files are supplied, merge those files and
  replace, rather than augment, the implicit target `best.pairs`.
- Apply repeatable `-r` files using `filter-segments` semantics: standalone
  words or exact bidirectional pairs reject the complete row.
- Do not expose `-i`, `-m`, `-g`, `-x`, `-P`, `--word-bonus`, or `-n` in v1.
  Derive `mN` and `gN` from the target and use the workflow generator's fixed
  defaults. Emit every surviving candidate.
- Leave `filter-segments` and its unchanged-row streaming contract untouched.

## Evaluation behavior

- Parse and validate the target's working letter bag and require every input
  row to spell it using exactly `gN` segments.
- Build the same `DfsClassList` `dfs-anagrams` builds for the working bag,
  with the same dictionary, classified-NO and target-NO exclusions, pair
  tiers, minimum length, `-x 2`, and missing external-pair synthesis. This
  applies every extraction policy exactly as phase 1 does, rather than
  re-deriving it per segment.
- Index the list once by member text, mapping each spelling to its
  `(class_index, member_index)`. One spelling per trie path makes the mapping
  unique. A row with any segment absent from the map is no longer generatable
  and is discarded; explicit `-r` rejections are applied to the row as well.
- Score each surviving row with `DfsTopN::emit`'s arithmetic, in row segment
  order, which is the order DFS emitted the path in:
  - the representative score is class member 0's
    `member_upper_log_score` for the first segment, then `append_log_score`
    of each later segment's member 0 score;
  - add, in segment order, the chosen member's upper score minus member 0's
    for every segment whose member index is nonzero, as
    `spelling_upper_log_score` does; and
  - add the `dfs_exact_result_matching` correction, which applies descending
    BEST scoring.
  Share that computation with `emit` rather than restating it, so unchanged
  inputs reproduce the exact `log_score` bits and therefore DFS's exact-tie
  ordering. Bonus markers come from the same member score flags `emit` uses.
- Sort using `dfs-anagrams` ordering: descending score, then word-set key, then
  spelling text. Render scores with the same `%#.4g` contract.
- Without `--show-bonus`, emit standard
  `score segment[,segment ...]` rows. With it, insert the existing aligned
  `S`/`Y`/`B`/`-` marker column. The current workflow's zero word bonus means
  it does not produce `W`.
- Reject already annotated input in v1. Annotated output remains presentation
  output and is not made compatible with the existing segment tools.

## Shared code

Rerank reuses the `dfs-anagrams` code paths below instead of restating them.

- **Workflow inputs.** Parse options with optparse and
  `dfs_parse_common_option`, using a rerank-owned table listing only the
  `--wf`, `--wfroot`, `-t`, and `--best-pairs` rows plus `-r` and
  `--show-bonus`. Omitting the other common rows is what keeps `-m`, `-x`,
  `-P`, `--word-bonus`, and `-n` off the interface. Resolve inputs with
  `finalize_dfs_workflow_args` and `collect_workflow_exclude_pair_files`,
  then load them with `load_weighted_pair_files` and
  `load_exclude_pair_files`. `finalize_dfs_workflow_args` gains a flag that
  skips the target `best.pairs` when explicit `--best-pairs` files were
  given; `dfs-anagrams` passes the additive setting. The segment tools'
  `load_pair_filters` is not used.
- **Class-list construction.** Move the dictionary, pair, and exclusion
  loading and the `DfsScoreModel`, `DfsSoloWords`, and `DfsClassList`
  construction out of `dfs-anagrams` `main()` into one helper, including the
  descending-BEST policy selection. `dfs-anagrams` and rerank both call it.
- **Working bag.** Add a helper beside `parse_workflow_target` that reads
  `ROOT/.wf/best/sN/letters` and applies `u-` subtraction or `o-` selection
  through `clean_letters`, `subtract_letters`, and `check_bag_length`,
  matching the generator's derivation.
- **Spelling construction.** Factor the per-spelling body of
  `DfsTopN::emit` into a function taking class indexes, member indexes, and
  the representative score. It builds the text, segment lengths, word-set
  key, bonus flags, and exact-matching correction. `emit` calls it for each
  expanded tuple; rerank calls it once per row. `make_word_set_key` moves
  with it.
- **Ordering and rendering.** Name the `take_sorted_results` comparator and
  use it for rerank's sort. Move the `dfs-anagrams` result-printing loop,
  including its `--show-bonus` branch, into a shared function.
- **Rejections.** Load each `-r` file with
  `load_pair_file(path, "reject list", &rejected, true, true, true)` and test
  segments with `is_rejected_segment`, exactly as `load_pair_filters` does for
  `filter-segments`.

The result-row parser remains local to rerank; the existing segment tools'
inline parsers are not consolidated here.

## Impact on `query-index`

- `finalize_dfs_workflow_args` gains its BEST-replacement flag;
  `query-index` passes the additive setting, so its target `best.pairs`
  handling is unchanged.
- `query-index` keeps its own class-list construction. It differs from the
  shared helper by selecting `include_phrases`, loading no exclusions, using
  index-only external pairs, and using fixed BEST scoring.
- `query-index --score` keeps its existing scoring. It does not call the
  shared spelling construction.
- `query-index` does not use `DfsTopN`, the result printer, or the bag helper,
  and gains no options.

## Validation

- Add one focused CLI smoke test using a small workflow fixture:
  - run `dfs-anagrams` normally and with `--show-bonus`;
  - rerank the ordinary output under identical inputs;
  - byte-compare both ordinary and annotated outputs with their DFS
    counterparts.
- In the same fixture, replace BEST with explicit `--best-pairs`, add an
  explicit rejection, and verify filtering, changed scores, markers, and
  ordering within the saved pool.
- Cover missing/non-full targets, mutually exclusive or absent workflow-root
  options, malformed rows, wrong segment counts, and replacement rather than
  additive BEST behavior.
- Compile and run the new focused target/test plus the tests covering the
  shared code: `dfs-output`, `dfs-search`, `dfs-cli`, and `query-index-cli`. Run `git diff --check` and `/review` before any commit.
