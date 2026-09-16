# A native `common-segments`

## Summary

`nutrimatic/rerank_all.py` and `nutrimatic/all_top_segments.py` answer the same
question by shelling out: for each BEST pair, rerank the result file and collect
what survives. Between them they spawn `rerank-anagrams | grep | sort` and
`rerank-anagrams | top-segments --pairs -n 0` once per pair, write one
intermediate file per pair, and duplicate ~120 lines of target, pair and
pipeline plumbing across the two scripts.

`rerank_all.py` also carries a known bug, recorded as a TODO above its `grep`:

> Match complete comma-delimited segments in both orientations. This unanchored
> grep accepts unrelated segments (for example, "tree homes" for "tree,home")
> and omits the reverse orientation even though `--one-best-pair` admits it.

`common-segments` does the pass in one process, with no subprocess and no
intermediate file. It reads the result file once and matches complete segments
in both orientations, which closes that TODO by construction.

`plans/segment-counts.md` names this tool in its "Not in scope": *"The second
tool. This change makes it linkable; it does not add it."* This is that tool.

## What it does

```
common-segments RESULTS PAIRS --wf -t FULL-TARGET
```

`RESULTS` holds `score segment[,segment ...]` rows; `PAIRS` holds `word,word`
lines in `best.pairs` format. A row *holds* a pair when one of its
comma-delimited segments equals that pair's two words in either order — a
complete segment match, not a substring.

Printed, one per line in ascending order, is every segment appearing on a row
that holds two or more distinct pairs, less the pairs that row is credited for.
Output is pair-file format: multi-word segments as `word,word` through
`format_pair_segment()`, solo words bare. That is what `top-segments --pairs`
emits, so the output feeds back into a pairs file.

## The per-row rule

The question is posed pairwise: intersect each pair's set of result rows with
every other pair's, and union the leftover segments of the rows in common. That
is O(|PAIRS|²) intersections over an index built for every row.

The same output falls out of one rule applied per row. Let `P` be the set of
distinct pairs a row holds:

- `|P| < 2` — the row is in no intersection and contributes nothing.
- `|P| == 2` — the row is in exactly one intersection, and both its pairs are
  excluded there. It contributes its non-pair segments only.
- `|P| >= 3` — for any `p` in `P` there is a combination `(i, j)` with `p`
  neither `i` nor `j`, which contributes `p`. The row contributes every segment.

So: drop rows holding fewer than two pairs; otherwise insert every segment,
holding back the pair segments when the row holds exactly two. No per-pair row
index is built, and no row is retained that cannot contribute — rows are
processed and discarded as they are read, so nothing scales with the size of
`RESULTS`.

## Changes

### `segment-rows.{cpp,h}`

Implement the row reader specified in `plans/segment-rows-refactor.md` —
`SegmentRow`, `SegmentRowReader`, `segment_rows_next()` — to that plan's
interface, diagnostics and reuse rules verbatim. It is a leaf: `<stdint.h>`,
`<istream>`, `<string>` and `<vector>`, no local header.

`common-segments` is its only client. The four existing copies stay put:
`segment_counts_read` in `segment-counts.cpp`, `find_segments` in
`first-segments.cpp`, `filter_stream` in `filter-segments.cpp` and `read_stats`
in `segment-stats.cpp` are not converted here. Converting them is the rest of
`plans/segment-rows-refactor.md`.

### `canonical_pair_segment()` moves into `segment-output.{cpp,h}`

`canonical_pair()` is a static in `segment-counts.cpp`; it returns a two-word
segment with the lexicographically smaller word first. `common-segments` needs
exactly that to collapse `hobbit home` and `home hobbit` to one pair identity.

Move it beside `format_pair_segment()` as `canonical_pair_segment()` and have
`segment-counts.cpp` call it. No behavior changes; `top-segments` output does
not move.

### `common-segments.cpp`

Structured like `top-segments.cpp`: `usage()` to stdout, `parse_args()`,
`main()`.

Options are the full pair-filter stack:

```cpp
static constexpr PairFilterSupport kSupport = {
    .ignore = true, .allow = true, .workflow_yes = true};
```

Each argument goes to `parse_pair_filter_option()` first, as in
`top-segments.cpp`, then to `--`, `-h`, unknown-option and positional handling.
That brings `-r`, `-i`, `-a`, `-d`, `--wf`, `--wfroot`, `-t` and `-y` in with
the spellings and diagnostics they already have. `check_pair_filter_options()`
runs before loading.

Two positionals in order, `RESULTS` then `PAIRS`, both required; a missing or
third positional is a usage error. `RESULTS` may be `-`.

`filter_options.input_path` is set to `RESULTS` when it is a named file, as
`top-segments.cpp` does, so target inference from the results path works and an
explicit `-t` disagreeing with the file is diagnosed.

`-t` alone is not accepted: `check_pair_filter_options()` refuses `-t` and `-y`
without a workflow root, and that rule is shared with every segment tool. The
invocation is `--wf -t FULL-TARGET`. Unlike the Python, the target need not be
spelled in full `sN/[ou]-letters/mN/gN` form, because `resolve_target_name()`
accepts abbreviated and symlinked names and announces what it resolved to.
Every name the Python accepted still works.

`PAIRS` loads in one call:

```cpp
DfsPairSet pairs;
if (!load_pair_file(path, "pair list", &pairs, true, true)) return 1;
```

`load_pair_file()` applies `load_dictionary()`'s normalization, rejects a line
that is not two nonempty comma-separated fields, and inserts both word orders.
Both-orientation matching is therefore free. With `allow_single_words` left
false, a single-word line is an error, matching the Python's `PAIR_RE`.

Reading is one pass over `segment_rows_next()`. Per row:

1. Apply the row filters in `segment_counts_read`'s precedence —
   `sources.classified_no`, `sources.target_no`, `is_rejected_segment`,
   `is_allowed_segment`, `all_words_in_dict` — skipping the row on the first
   match, through the predicates in `pair-exclusions.h`.
2. Collect the held pairs: for each segment in `pairs`, insert
   `canonical_pair_segment(segment)` into a per-row set.
3. Drop the row if that set holds fewer than two.
4. Otherwise insert the row's segments into a `std::set<std::string>`, skipping
   the two pair segments when the row holds exactly two, and skipping anything
   in `filters.ignored` or `sources.classified_yes`.

Printing walks the set in order through `format_pair_segment()`, then checks
`ferror(stdout)`.

When no row held two or more pairs, stdout is empty and the status is 0, with
one stderr line saying so. `rerank-all` fails when its grep matches nothing;
that is not carried over, because an empty intersection is an answer.

### Build

Declare the leaf next to `segment_output_dep`:

```meson
segment_rows_dep = declare_dependency(
  sources: files('segment-rows.cpp'),
)
```

Add the executable beside `top_segments`:

```meson
common_segments = executable('common-segments', 'common-segments.cpp',
  dependencies: [pair_exclusions_dep, segment_output_dep, segment_rows_dep],
  install: true)
```

`segment_counts_dep` is not needed: this tool counts nothing and calls neither
`segment_counts_read()` nor `segment_counts_print_top()`.

Register the test beside the `top-segments` entry:

```meson
test(
  'common-segments',
  bash,
  args: [files('test-common-segments.sh'), common_segments],
)
```

### `test-common-segments.sh`

A smoke test in `test-top-segments.sh`'s style — binary as `$1`, `mktemp -d`,
`trap cleanup EXIT`, `fail()`, `echo PASS` — pinning the semantics and no more:

- a two-pair row contributes its non-pair segments and neither pair;
- a three-pair row contributes all three pairs;
- a one-pair row contributes nothing;
- a reversed segment (`home hobbit` for `hobbit,home`) matches;
- a substring near-miss (`tree homes` for `tree,home`) does not;
- a malformed results row, and a malformed pairs line, each fail;
- missing positionals fail.

### `docs/common-segments.md`

A behavioral note shaped like `docs/top-segments.md`: the row format, what
holding a pair means, the per-row rule and the output format.

## Files

| File | Change |
|---|---|
| `source/segment-rows.{h,cpp}` | new leaf row reader |
| `source/common-segments.cpp` | new tool |
| `source/test-common-segments.sh` | new smoke test |
| `source/segment-output.{h,cpp}` | gains `canonical_pair_segment()` |
| `source/segment-counts.cpp` | drops its static `canonical_pair()` |
| `source/meson.build` | `segment_rows_dep`, executable, test |
| `docs/common-segments.md` | new |

Reused unmodified: `pair-exclusions.h` for options, target resolution and the
filter predicates; `dfs-cli-args.h`'s `load_pair_file()`; `segment-output.h`'s
`format_pair_segment()` and `is_pair_segment()`; `workflow-paths.h`.

## Validation

```bash
source ./setup.sh
source .env/bin/activate
source build/dep-info/conanbuild.sh
conan build .
meson test -C build common-segments top-segments --print-errorlogs
git diff --check
```

`conan build .` does not run the tests, so `meson test` is its own step.
`top-segments` is retested because `canonical_pair_segment()` touches
`segment-counts.cpp`; its output must be byte for byte what it was.

End to end on real data, from the repo root:

```bash
./build/common-segments results/s6/dfs.s6.g4.1000000 results/s6/best.pairs --wf
```

The target is inferred from the results file name and the resolution is
announced on stderr. `results/s6/best.pairs` carries several pairs;
`results/s3/best.pairs` carries one and is the input for the empty-output path.

Cross-check against the scripts this replaces: for a pair present in
`results/s6/all.dfs.s6.g4.1000000/dfs/`, every segment printed for that pair
should appear among the rows `rerank-all` collected, and the near-miss rows the
old `grep` admitted should be absent.

## Not in scope

- Converting the four existing row-parser copies to `segment_rows_next()`. That
  is the remainder of `plans/segment-rows-refactor.md`.
- Retiring `nutrimatic/rerank_all.py`, `nutrimatic/all_top_segments.py` or their
  launchers. They stay until this tool has run against real data.
- Reranking or rescoring. `common-segments` reads the result file as given, does
  not link the DFS machinery, and has no analogue of `--one-best-pair`.
