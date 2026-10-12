# Extract the segment counting and ranked printing behind `segment-counts`

## Summary

`top-segments` is one file holding two separable things: a CLI — usage text,
argument parsing, mutual-exclusion checks — and the operation those arguments
describe, which is to count every eligible segment occurrence across a result
set and print the top `N` by descending count.

Move the operation into `segment-counts.{cpp,h}` and leave `top-segments.cpp`
as the CLI wrapper around it. A second tool then links the same counting and
printing without inheriting `top-segments`' option surface.

No behavior changes. `top-segments` produces identical stdout and stderr for
every combination of its options before and after.

## Interface

```cpp
#include "pair-exclusions.h"  // PairFilters, PairFilterSources, DfsPairSet
#include "segment-output.h"   // SegmentOutputOptions

// What one counted segment accumulates. `line_count` counts distinct
// surviving result rows containing the segment and `last_seen_row` is the
// bookkeeping that keeps one row from counting twice; both are --elim only.
struct SegmentStats {
  uint64_t count = 0;
  uint64_t line_count = 0;
  uint64_t last_seen_row = 0;
  size_t length = 0;
};

typedef std::unordered_map<std::string, SegmentStats> SegmentStatsMap;

// What the pair filters dropped, by layer, so a caller can say which layer
// owned each rejection. The row counters are mutually exclusive: the first
// matching layer owns the row.
struct PairFilterStats {
  uint64_t classified_no_lines = 0;
  uint64_t target_no_lines = 0;
  uint64_t explicit_reject_lines = 0;
  uint64_t allow_lines = 0;
  uint64_t dictionary_lines = 0;
  uint64_t classified_yes_instances = 0;
  uint64_t explicit_ignore_instances = 0;
  DfsPairSet classified_yes_pairs;
};

// How segments are selected while reading and ordered while printing.
struct SegmentCountsOptions {
  SegmentOutputOptions output;
  bool elimination = false;  // --elim: rank by decision impact, not count
  bool show_counts = false;  // print the count column
};

// Everything accumulated across every input. `program` prefixes diagnostics.
struct SegmentCountsData {
  char const* program = NULL;
  SegmentStatsMap segments;
  PairFilterStats filtered;
  uint64_t surviving_rows = 0;
  std::unordered_set<std::string> unique_segments;
};

// Reads one input, adding to *data. `name` is the file name in diagnostics.
// Call once per input; several inputs accumulate into the same counts.
bool segment_counts_read(
    std::istream* input, char const* name, PairFilters const& filters,
    SegmentCountsOptions const& options, SegmentCountsData* data);

// Writes the per-layer filter summary to stderr, or nothing when no layer
// dropped anything.
void segment_counts_print_filter_summary(
    SegmentCountsData const& data, PairFilterSources const& sources);

// Writes the top rows to stdout, applying the word projection, the ordering
// and the limit in `options`.
bool segment_counts_print_top(
    SegmentCountsData const& data, SegmentCountsOptions const& options);
```

`segment-counts.h` also includes `<stdint.h>`, `<istream>`, `<string>`,
`<unordered_map>` and `<unordered_set>` for its own declarations, so a TU that
includes it alone can call both entry points and name every type in their
signatures.

## What moves

From `top-segments.cpp` into `segment-counts.cpp`, unchanged except as noted:

- `SegmentStats`, the `SegmentCounts` typedef renamed to `SegmentStatsMap`,
  and `FilterStats` renamed to `PairFilterStats`, all three to the header.
- `any_segment_in`, `any_rejected_segment`, `any_segment_outside`,
  `any_segment_disallowed`, `canonical_pair`, `selected_segment`,
  `count_candidate` and `split_counts`, all staying static.
- `count_stream` becomes `segment_counts_read`, `print_filter_summary` becomes
  `segment_counts_print_filter_summary`, and `print_counts` becomes
  `segment_counts_print_top`.

`usage`, `kSupport`, `Args`, `parse_args` and `main` stay in
`top-segments.cpp`. `Args` holds one `SegmentCountsOptions` in place of its
separate `output_options`, `elimination` and `show_counts` members.

## Parameter bundling

`segment_counts_read` and `segment_counts_print_top` each take one
`SegmentCountsOptions` rather than an options struct followed by loose bools.
`show_counts` stays derived in `parse_args`: it falls out of `--counts`,
`--no-counts` and whether the output is pair segments, and the three
mutual-exclusion checks around it (`--counts` against `--no-counts`, `--elim`
against `--no-counts`, `--elim` against `--by-length`) are CLI concerns that
diagnose and call `usage`.

`program` is a field of `SegmentCountsData` rather than a parameter on each
function, so a caller states its name once and every diagnostic below picks it
up.

## Diagnostics

Every message keeps its present text with the hardcoded `top-segments` prefix
replaced by `data->program`. `test-top-segments.sh` compares these, so they
stay byte for byte:

- `%s: %s:%" PRIu64 ": expected \"score segment[,segment ...]\"\n`
- `%s: %s:%" PRIu64 ": empty segment\n`
- `%s: can't read \"%s\"\n`
- `%s: segment count overflow\n`, `%s: segment line count overflow\n`,
  `%s: result line count overflow\n`, `%s: word count overflow\n`
- the `%s: Filtered ` and `%s: Ignored ` summary lines

## Includes

`top-segments.cpp` includes `pair-exclusions.h`, `segment-counts.h` and
`segment-output.h`. It keeps the first and third directly because `parse_args`
and `main` call into both — `parse_pair_filter_option`, `load_pair_filters`,
`print_reject_option_help`, `select_segment_output`,
`parse_segment_output_option` and the selection enums — rather than taking
them transitively through `segment-counts.h`.

Its standard includes drop `<algorithm>`, `<limits>`, `<unordered_map>` and
`<unordered_set>`, which follow the counting out. `<fstream>`, `<iostream>`,
`<utility>`, `<string>` and `<vector>` stay for the input loop and `Args`.

## Build

Declare the source next to `segment_output_dep`, naming the two dependencies
its header exposes so a consumer gets them by linking one thing:

```meson
segment_counts_dep = declare_dependency(
  sources: files('segment-counts.cpp'),
  dependencies: [pair_exclusions_dep, segment_output_dep],
)
```

`top-segments` takes `dependencies: [pair_exclusions_dep, segment_counts_dep,
segment_output_dep]`, listing what it calls directly for the same reason it
includes all three.

## Validation

```bash
source ./setup.sh
source .env/bin/activate
source build/dep-info/conanbuild.sh
meson compile -C build top-segments
meson test -C build top-segments --print-errorlogs
git diff --check
```

Then diff against `tmp/baseline/top-segments`, the pre-refactor binary taken
from this working tree before either plan started. `tmp/baseline/PROVENANCE`
records the HEAD, the git status and the source hashes it was built from, and
the binary is statically linked against the project libraries, so rebuilding
`build/` does not disturb it.

Cover the modes the test does not: the default, `--pairs`, `--solo-words`,
`--all-words`, `--pair-words --unique`, `--elim`, `--counts`, `--no-counts`,
`-l` and `-n 0`, and a workflow root so the filter summary and its per-layer
counters appear on stderr. stdout, stderr and exit status all match.

## Not in scope

- The second tool. This change makes it linkable; it does not add it.
- Splitting `segment_counts_print_top` into a ranking step and a formatting
  step. It stays one function until a caller wants a different format.
- The row parser that `segment_counts_read` carries with it, which
  `plans/segment-rows-refactor.md` extracts separately.
