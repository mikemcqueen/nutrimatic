# One result-row reader behind `segment-rows`

## Summary

`segment-rows.{cpp,h}` exists and `common-segments` reads through it. It was
built with `common-segments`, which needed a row reader, and it has had no
other caller since.

Five tools still parse result rows themselves. Four of them —
`segment_counts_read` in `segment-counts.cpp`, `find_segments` in
`first-segments.cpp`, `filter_stream` in `filter-segments.cpp` and `read_stats`
in `segment-stats.cpp` — open with the same thirty-five lines and differ only
in the program name embedded in their three diagnostics. The fifth,
`parse_segments` in `rerank-anagrams.cpp`, validates considerably more: it
rejects nonsense scores, refuses `--show-bonus` input, strips solo-word
annotations, constrains the segment alphabet and spacing, and checks the row
against the target's letter bag and segment count.

Move all of that validation into `segment-rows` and convert all six tools to
it. The stricter rules are what reading a `dfs-anagrams` result file has always
meant; only `rerank-anagrams` enforced them. Afterwards one implementation
decides what a well-formed row is, every tool reports the same diagnostics, and
`parse_segments` is deleted rather than reduced.

This changes behavior. The five tools that were lenient now reject rows they
used to misread, which is the point: fed `--show-bonus` output today,
`top-segments` splits `3.5 W,-,B sixth,vin diesel` into the segments `W`, `-`,
`B sixth` and `vin diesel`, and counts them.

## Interface

```cpp
// One nonempty result row. `line` is reused across calls, so a caller that
// needs the text past the current row copies it.
struct SegmentRow {
  std::string line;                   // the row as read, without a trailing CR
  size_t segments_start = 0;          // offset in `line` of the first segment
  std::vector<std::string> segments;  // comma-separated, solo suffix removed
};

// Reader state over one input.
struct SegmentRowReader {
  std::istream* input = NULL;
  char const* name = NULL;     // the file name in diagnostics; "-" for stdin
  char const* program = NULL;  // the diagnostic prefix

  // Optional row-shape check. Both are set or both are left alone; only
  // rerank-anagrams knows a target, so only rerank-anagrams sets them.
  std::string required_letters;   // the bag every row must spell
  int required_segments = 0;      // the exact segment count each row carries

  uint64_t line_number = 0;    // lines consumed, counting skipped blank ones
  bool failed = false;         // a row was malformed, or the stream went bad
};

bool segment_rows_next(SegmentRowReader* reader, SegmentRow* out);
```

`segments` holds the spelling to match and look up; `line` holds the row as
read. That split is what lets `filter-segments` keep printing rows verbatim
while matching on clean spellings, and what gives `rerank-anagrams` the bare
index entry its member lookup needs.

`segment-rows.h` includes `<stdint.h>`, `<istream>`, `<string>` and `<vector>`
and no local header. It stays a leaf: it knows the row format and nothing about
filtering, options, counting or the index.

## Reader behavior

Per row, in order. Every diagnostic takes the `program` prefix, so the six
tools report identically:

1. **Score.** Parse with `strtod` and require `score_end != line.c_str()`,
   `*score_end == ' '` and `score_end[1] != '\0'`, then require the value to
   satisfy `isfinite(value) && value >= 0.0`. `dfs_print_results` writes the
   column as `fprintf("%#.7g ", exp(log_score))` and `exp` yields `[0, +inf]`,
   so a negative, NaN or infinite score did not come from a result file. The
   value is discarded; no caller reads it.

   This replaces `rerank-anagrams`' `errno == ERANGE` test rather than adopting
   it. That test is backwards at both ends: glibc raises `ERANGE` on subnormal
   underflow, so `1e-320` — a legitimate score — is rejected, while a literal
   `inf` passes because `strtod` sets no `errno` for it. `isfinite` and `>= 0`
   reject `inf` and `1e400`, accept subnormals, and need no `errno` handling.

   Diagnostic: `expected "score segment[,segment ...]"`

2. **Annotation.** Reject the row when the first character at `segments_start`
   is `A`–`Z` or `-`. `dfs_print_results` emits an optional bonus column
   between the score and the entries, built by `dfs_spelling_bonus_list` from
   `W`/`P`/`S`/`Y`/`B` per segment with `-` for no bonus. Segments are lowercase
   letters, digits and spaces, so one character separates the two forms. The
   score validation guarantees a character is there to test.

   Diagnostic: `annotated input is not supported`

3. **Split.** Split on commas, rejecting an empty field.

   Diagnostic: `empty segment`

4. **Solo suffix.** For each segment, when it ends in `)` and holds a `" ("`
   whose contents are all `[a-z0-9]`, erase from the `" ("`.
   `dfs_spelling_entry_list` appends `" (" + word + ")"` whenever solo words
   are configured and not hidden, so saved files legitimately carry entries
   like `sixth (vin)`. Stripping happens before the alphabet check, because the
   annotation's parentheses would fail it.

5. **Alphabet and spacing.** Every remaining character is `[a-z0-9]` or a
   single interior space — no leading space, no trailing space, no doubled
   space.

   Diagnostics: `malformed spacing in segment "%s"`, `bad character '%c' in
   segment`

6. **Row shape**, only when `required_segments` is nonzero. The row carries
   exactly that many segments, and its letters are a permutation of
   `required_letters`. The letter bag is accumulated during step 5 and only
   when this check is enabled, so the ordinary readers pay nothing for it.

   Diagnostics: `expected %d segments, found %zu`, `row does not spell target
   letters "%s"`

`failed` is set on any rejection and on `input->bad()` once the stream is
exhausted, so a caller's whole error path is `if (reader.failed) return false;`
after its loop. `out->line` and `out->segments` are reused across calls.

## Tool changes

Each stream function loses its parsing preamble and keeps its own logic, as
`collect_common` in `common-segments.cpp` already does:

```cpp
SegmentRowReader reader = {input, name, data->program};
SegmentRow row;
while (segment_rows_next(&reader, &row)) {
  // the tool's own work, over row.segments
}
if (reader.failed) return false;
```

- `segment-counts`' `segment_counts_read` keeps its row attribution — a pass
  over `PairFilters::first_rejecting_layer` holding the earliest layer and
  stopping early on `PAIR_FILTER_CLASSIFIED_NO` — and its counting, reading
  `row.segments` where it built `segments`. It passes `data->program`, so the
  diagnostics keep naming the running tool rather than a literal.
- `first-segments`' `find_segments` keeps `while (result.size() <
  output_options.limit && segment_rows_next(&reader, &row))`, preserving its
  early stop. Its `reject_line` scan moves out of the split loop into a pass
  over `row.segments`; it does not short-circuit today either, so the same rows
  are rejected. Its closing message reads `reader.line_number`.
- `filter-segments`' `filter_stream` keeps its own early stop, becoming `while
  ((!have_limit || output_count < limit) && segment_rows_next(&reader, &row))`.
  It computes `include` and `regex_matched` over `row.segments`, and prints
  `row.line.c_str()` or `row.line.c_str() + row.segments_start` per
  `show_score` — the latter is today's `score_end + 1`.
- `segment-stats`' `read_stats` increments `total_rows` after a successful
  `segment_rows_next` rather than before parsing the score, and calls
  `add_segment` over `row.segments`, keeping its per-row `RowCounts` and the
  allowed-pair counters that follow. A malformed row makes both the old and the
  new code return false without printing, so the moved increment is not
  observable.
- `common-segments`' `collect_common` is unchanged apart from inheriting the
  new rules.
- `rerank-anagrams`' `rerank_stream` sets `required_letters` and
  `required_segments` from the target and loops over rows like the others,
  dropping its hand-rolled `getline`, CR strip, empty-line skip and
  `input->bad()` check along with `parse_segments`, which is deleted. Its
  member lookup and rejection pass read `row.segments`.

## Build

`segment_rows_dep` is already declared next to `segment_output_dep`, and
`common-segments` already lists it. Add it to the `dependencies` of
`segment_counts_dep` and of the `first-segments`, `filter-segments`,
`segment-stats` and `rerank-anagrams` executables. `top-segments` takes it
transitively through `segment_counts_dep` and its own list does not change,
because nothing left in `top-segments.cpp` reads a row.

## Validation

```bash
source ./setup.sh
source .env/bin/activate
source build/dep-info/conanbuild.sh
meson compile -C build top-segments first-segments filter-segments \
  segment-stats common-segments rerank-anagrams
meson test -C build top-segments first-segments filter-segments \
  common-segments rerank-anagrams --print-errorlogs
git diff --check
```

Well-formed rows are unaffected, so the good-row comparisons against
`tmp/baseline-2a16443` hold for all five binaries captured there. Every segment
in every test fixture and in the sampled result files is already within
`[a-z0-9 ,]`, and no result file carries a negative, NaN or infinite score, so
the registered tests should pass unchanged.

What changes is the treatment of malformed and annotated input, and each needs
a new expectation:

- a negative, NaN or infinite score is refused by all six, where five accepted
  it;
- `--show-bonus` output is refused by all six, where five silently split the
  bonus column into segments;
- a segment carrying a character outside `[a-z0-9 ]`, or malformed spacing, is
  refused by all six;
- `sixth (vin)` and `sixth` now count as one segment rather than two in
  `top-segments`, `first-segments` and `segment-stats`, while `filter-segments`
  still prints whichever form the row carried;
- `rerank-anagrams` accepts subnormal scores it used to refuse and refuses
  infinite ones it used to accept.

`segment-stats` has no registered test. Smoke it against
`tmp/baseline-2a16443/segment-stats` on a real results file, confirming
identical stdout, stderr and exit status, and cover the new rejections by hand.

## Not in scope

- Extracting the counting and ranked printing that `top-segments` and a second
  tool share. `plans/segment-counts.md` covers that and has landed.
- What each caller does with a rejection. The precedence itself is shared, in
  `PairFilters::first_rejecting_layer`; the use stays per-caller —
  `segment-counts` attributes to per-layer counters, `filter-segments` and
  `common-segments` collapse it to a boolean.
- `segment-report`, and `weighted-segments` through it. It reads a different
  `score count text` format.
- `query-index`'s stdin mode. Its lines carry comma-separated values with no
  score column.
