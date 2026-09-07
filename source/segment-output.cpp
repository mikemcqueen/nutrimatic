#include "segment-output.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

bool parse_limit(char const* text, uint64_t* limit) {
  if (text[0] == '\0' || text[0] == '-') return false;
  errno = 0;
  char* end;
  unsigned long long const parsed = strtoull(text, &end, 10);
  if (errno == ERANGE || *end != '\0') return false;
  *limit = parsed;
  return true;
}

namespace {

bool set_mode(
    SegmentOutputMode mode, char const* program, SegmentOutputOptions* out) {
  if (out->mode != SEGMENT_OUTPUT_SEGMENTS && out->mode != mode) {
    fprintf(stderr,
        "%s: --pairs, --solo-words and --all-words are mutually exclusive\n",
        program);
    return false;
  }
  out->mode = mode;
  return true;
}

}  // namespace

SegmentOutputOptionResult parse_segment_output_option(
    int argc, char* const argv[], int* index, char const* program,
    SegmentOutputOptions* out) {
  char const* const option = argv[*index];
  if (strcmp(option, "--pairs") == 0) {
    return set_mode(SEGMENT_OUTPUT_PAIRS, program, out)
        ? SEGMENT_OUTPUT_OPTION_HANDLED : SEGMENT_OUTPUT_OPTION_ERROR;
  }
  if (strcmp(option, "--solo-words") == 0) {
    return set_mode(SEGMENT_OUTPUT_SOLO_WORDS, program, out)
        ? SEGMENT_OUTPUT_OPTION_HANDLED : SEGMENT_OUTPUT_OPTION_ERROR;
  }
  if (strcmp(option, "--all-words") == 0) {
    return set_mode(SEGMENT_OUTPUT_ALL_WORDS, program, out)
        ? SEGMENT_OUTPUT_OPTION_HANDLED : SEGMENT_OUTPUT_OPTION_ERROR;
  }
  if (strcmp(option, "-l") == 0 || strcmp(option, "--by-length") == 0) {
    out->by_length = true;
    return SEGMENT_OUTPUT_OPTION_HANDLED;
  }
  if (strcmp(option, "-n") != 0) return SEGMENT_OUTPUT_OPTION_OTHER;

  if (++*index == argc || !parse_limit(argv[*index], &out->limit)) {
    fprintf(stderr, "%s: -n requires a non-negative integer\n", program);
    return SEGMENT_OUTPUT_OPTION_ERROR;
  }
  return SEGMENT_OUTPUT_OPTION_HANDLED;
}

bool is_pair_segment(std::string const& segment) {
  return segment.find(' ') != std::string::npos;
}

bool is_solo_segment(std::string const& segment) {
  return !is_pair_segment(segment);
}

std::vector<std::string> split_segment_words(std::string const& segment) {
  std::vector<std::string> words;
  size_t start = 0;
  while (start <= segment.size()) {
    size_t const end = segment.find(' ', start);
    size_t const length =
        end == std::string::npos ? segment.size() - start : end - start;
    if (length > 0) words.push_back(segment.substr(start, length));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return words;
}

size_t segment_nonspace_length(std::string const& segment) {
  return segment.size() - std::count(segment.begin(), segment.end(), ' ');
}

std::string format_pair_segment(std::string segment) {
  std::replace(segment.begin(), segment.end(), ' ', ',');
  return segment;
}
