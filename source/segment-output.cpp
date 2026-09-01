#include "segment-output.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

namespace {

bool parse_limit(char const* text, uint64_t* limit) {
  if (text[0] == '\0' || text[0] == '-') return false;
  errno = 0;
  char* end;
  unsigned long long const parsed = strtoull(text, &end, 10);
  if (errno == ERANGE || *end != '\0') return false;
  *limit = parsed;
  return true;
}

}  // namespace

SegmentOutputOptionResult parse_segment_output_option(
    int argc, char* const argv[], int* index, char const* program,
    SegmentOutputOptions* out) {
  char const* const option = argv[*index];
  if (strcmp(option, "--pairs") == 0) {
    out->pairs = true;
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

std::string format_pair_segment(std::string segment) {
  std::replace(segment.begin(), segment.end(), ' ', ',');
  return segment;
}
