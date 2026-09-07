#ifndef NUTRIMATIC_SEGMENT_OUTPUT_H
#define NUTRIMATIC_SEGMENT_OUTPUT_H

#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

static uint64_t const DEFAULT_SEGMENT_OUTPUT_LIMIT = 1000;

// The unit selected for output. With no unit, tools operate on whole segments
// of any width.
enum SegmentUnit {
  SEGMENT_UNIT_PAIRS,
  SEGMENT_UNIT_SOLO_WORD,
  SEGMENT_UNIT_ALL_WORDS,
};

struct SegmentOutputOptions {
  std::optional<SegmentUnit> unit;
  uint64_t limit = DEFAULT_SEGMENT_OUTPUT_LIMIT;
  bool by_length = false;
};

enum SegmentOutputOptionResult {
  SEGMENT_OUTPUT_OPTION_OTHER,
  SEGMENT_OUTPUT_OPTION_HANDLED,
  SEGMENT_OUTPUT_OPTION_ERROR,
};

// Reads a -n N value: a non-negative integer, with 0 meaning no limit for
// the tools that spell it that way. Shared so every -n takes the same
// spellings. Nothing is diagnosed here; the caller names its own option.
bool parse_limit(char const* text, uint64_t* limit);

// Parses --pairs, --solo-words, --all-words, -l/--by-length and -n N. `index`
// points to the current argument and advances over a consumed N. The three
// unit options are mutually exclusive, though repeating one is allowed. Errors
// are diagnosed already.
SegmentOutputOptionResult parse_segment_output_option(
    int argc, char* const argv[], int* index, char const* program,
    SegmentOutputOptions* out);

bool is_pair_segment(std::string const& segment);

bool is_solo_segment(std::string const& segment);

// Returns the words of a segment in order; a solo segment yields itself.
std::vector<std::string> split_segment_words(std::string const& segment);

// Returns the number of characters other than spaces in a segment.
size_t segment_nonspace_length(std::string const& segment);

// Returns a pair-file representation of a segment.
std::string format_pair_segment(std::string segment);

#endif
