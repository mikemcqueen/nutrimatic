#ifndef NUTRIMATIC_SEGMENT_COUNTS_H
#define NUTRIMATIC_SEGMENT_COUNTS_H

#include <stdint.h>

#include <istream>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "pair-exclusions.h"
#include "segment-output.h"

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
  // -u: cleaned letters to remove from the first row's letter bag; only
  // segments that fit in what remains are printed.
  std::optional<std::string> used_letters;
};

// Everything accumulated across every input. `program` prefixes diagnostics.
struct SegmentCountsData {
  char const* program = NULL;
  SegmentStatsMap segments;
  PairFilterStats filtered;
  uint64_t surviving_rows = 0;
  std::unordered_set<std::string> unique_segments;
  // With -u, the first row's letters less the used letters, sorted.
  std::optional<std::string> remaining_letters;
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

#endif
