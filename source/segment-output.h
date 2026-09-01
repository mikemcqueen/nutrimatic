#ifndef NUTRIMATIC_SEGMENT_OUTPUT_H
#define NUTRIMATIC_SEGMENT_OUTPUT_H

#include <stdint.h>

#include <string>
#include <vector>

static uint64_t const DEFAULT_SEGMENT_OUTPUT_LIMIT = 1000;

// What a printed row is: a segment of any width by default, a multi-word
// segment, a single-word segment, or a word drawn from any segment.
enum SegmentOutputMode {
  SEGMENT_OUTPUT_SEGMENTS,
  SEGMENT_OUTPUT_PAIRS,
  SEGMENT_OUTPUT_SOLO_WORDS,
  SEGMENT_OUTPUT_ALL_WORDS,
};

struct SegmentOutputOptions {
  SegmentOutputMode mode = SEGMENT_OUTPUT_SEGMENTS;
  uint64_t limit = DEFAULT_SEGMENT_OUTPUT_LIMIT;
};

enum SegmentOutputOptionResult {
  SEGMENT_OUTPUT_OPTION_OTHER,
  SEGMENT_OUTPUT_OPTION_HANDLED,
  SEGMENT_OUTPUT_OPTION_ERROR,
};

// Parses --pairs, --solo-words, --all-words and -n N. `index` points to the
// current argument and advances over a consumed N. The three mode options are
// mutually exclusive, though repeating one is allowed. Errors are diagnosed
// already.
SegmentOutputOptionResult parse_segment_output_option(
    int argc, char* const argv[], int* index, char const* program,
    SegmentOutputOptions* out);

bool is_pair_segment(std::string const& segment);

bool is_solo_segment(std::string const& segment);

// Returns the words of a segment in order; a solo segment yields itself.
std::vector<std::string> split_segment_words(std::string const& segment);

// Returns a pair-file representation of a segment.
std::string format_pair_segment(std::string segment);

#endif
