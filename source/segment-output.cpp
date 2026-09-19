#include "segment-output.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include "option-value.h"

bool parse_limit(char const* text, uint64_t* limit) {
  if (text[0] == '\0' || text[0] == '-') return false;
  errno = 0;
  char* end;
  unsigned long long const parsed = strtoull(text, &end, 10);
  if (errno == ERANGE || *end != '\0') return false;
  *limit = parsed;
  return true;
}

bool select_segment_output(
    SegmentSelection selection, SegmentProjection projection,
    char const* program, SegmentOutputOptions* out) {
  if (out->mode_explicit &&
      (out->selection != selection || out->projection != projection)) {
    fprintf(stderr,
        "%s: segment output options are mutually exclusive\n",
        program);
    return false;
  }
  out->selection = selection;
  out->projection = projection;
  out->mode_explicit = true;
  return true;
}

SegmentOutputOptionResult parse_segment_output_option(
    int argc, char* const argv[], int* index, char const* program,
    SegmentOutputOptions* out) {
  char const* const option = argv[*index];
  if (strcmp(option, "--pairs") == 0) {
    return select_segment_output(
        SEGMENT_SELECTION_PAIRS, SEGMENT_PROJECTION_SEGMENTS, program, out)
        ? SEGMENT_OUTPUT_OPTION_HANDLED : SEGMENT_OUTPUT_OPTION_ERROR;
  }
  if (strcmp(option, "--solo-words") == 0) {
    return select_segment_output(
        SEGMENT_SELECTION_SOLO, SEGMENT_PROJECTION_SEGMENTS, program, out)
        ? SEGMENT_OUTPUT_OPTION_HANDLED : SEGMENT_OUTPUT_OPTION_ERROR;
  }
  if (strcmp(option, "--all-words") == 0) {
    return select_segment_output(
        SEGMENT_SELECTION_ALL, SEGMENT_PROJECTION_WORDS, program, out)
        ? SEGMENT_OUTPUT_OPTION_HANDLED : SEGMENT_OUTPUT_OPTION_ERROR;
  }
  if (strcmp(option, "--pair-words") == 0) {
    return select_segment_output(
        SEGMENT_SELECTION_PAIRS, SEGMENT_PROJECTION_WORDS, program, out)
        ? SEGMENT_OUTPUT_OPTION_HANDLED : SEGMENT_OUTPUT_OPTION_ERROR;
  }
  if (strcmp(option, "--unique") == 0) {
    out->weight = SEGMENT_WEIGHT_UNIQUE;
    return SEGMENT_OUTPUT_OPTION_HANDLED;
  }
  if (strcmp(option, "-l") == 0 || strcmp(option, "--by-length") == 0) {
    out->by_length = true;
    return SEGMENT_OUTPUT_OPTION_HANDLED;
  }
  char const* value;
  if (!match_option_value(argc, argv, index, "-n", NULL, &value))
    return SEGMENT_OUTPUT_OPTION_OTHER;

  if (value == NULL || !parse_limit(value, &out->limit)) {
    fprintf(stderr, "%s: -n requires a non-negative integer\n", program);
    return SEGMENT_OUTPUT_OPTION_ERROR;
  }
  return SEGMENT_OUTPUT_OPTION_HANDLED;
}

bool check_segment_output_options(
    SegmentOutputOptions const& options, char const* program) {
  if (options.weight == SEGMENT_WEIGHT_UNIQUE &&
      !is_pair_word_output(options)) {
    fprintf(stderr, "%s: --unique requires --pair-words\n", program);
    return false;
  }
  return true;
}

bool is_selected_segment(
    SegmentSelection selection, std::string const& segment) {
  if (selection == SEGMENT_SELECTION_PAIRS) return is_pair_segment(segment);
  if (selection == SEGMENT_SELECTION_SOLO) return is_solo_segment(segment);
  return true;
}

bool is_pair_word_output(SegmentOutputOptions const& options) {
  return options.selection == SEGMENT_SELECTION_PAIRS &&
      options.projection == SEGMENT_PROJECTION_WORDS;
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

std::string canonical_pair_segment(std::string const& segment) {
  size_t const space = segment.find(' ');
  if (space == std::string::npos) return segment;
  std::string const left = segment.substr(0, space);
  std::string const right = segment.substr(space + 1);
  return left < right ? segment : right + " " + left;
}
