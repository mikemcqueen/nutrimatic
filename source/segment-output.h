#ifndef NUTRIMATIC_SEGMENT_OUTPUT_H
#define NUTRIMATIC_SEGMENT_OUTPUT_H

#include <stdint.h>

#include <string>

static uint64_t const DEFAULT_SEGMENT_OUTPUT_LIMIT = 1000;

struct SegmentOutputOptions {
  bool pairs = false;
  uint64_t limit = DEFAULT_SEGMENT_OUTPUT_LIMIT;
};

enum SegmentOutputOptionResult {
  SEGMENT_OUTPUT_OPTION_OTHER,
  SEGMENT_OUTPUT_OPTION_HANDLED,
  SEGMENT_OUTPUT_OPTION_ERROR,
};

// Parses --pairs and -n N. `index` points to the current argument and advances
// over a consumed N. Errors are diagnosed already.
SegmentOutputOptionResult parse_segment_output_option(
    int argc, char* const argv[], int* index, char const* program,
    SegmentOutputOptions* out);

bool is_pair_segment(std::string const& segment);

// Returns a pair-file representation of a segment.
std::string format_pair_segment(std::string segment);

#endif
