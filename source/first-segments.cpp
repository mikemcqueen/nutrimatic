#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

#include "pair-exclusions.h"
#include "segment-output.h"

static void usage(FILE* fp, char const* program) {
  fprintf(fp,
      "usage: %s [--pairs] [-n N] [-i FILE | --ignore FILE]...\n"
      "          [-r FILE | --reject FILE]... [--wf [-y | --yes]]\n"
      "          [RESULTS]\n"
      "  print the first N distinct, non-ignored segments from valid\n"
      "  dfs-anagrams RESULTS rows as comma-separated pairs\n"
      "  --pairs             print only multi-word segments\n"
      "  -n N                 maximum number of segments to print; defaults\n"
      "                       to %" PRIu64 "\n"
      "  -i, --ignore FILE    do not select pairs listed in FILE; may be\n"
      "                       repeated\n"
      "  -r, --reject FILE    discard rows containing pairs listed in FILE;\n"
      "                       may be repeated\n"
      "  --wf                 reject pairs listed in\n"
      "                       $WFROOT/.wf/classified/no/no.pairs; missing\n"
      "                       files produce warnings\n"
      "  -y, --yes            with --wf, ignore pairs listed in\n"
      "                       $WFROOT/.wf/classified/yes/yes.pairs\n"
      "  with no RESULTS, or when RESULTS is -, read standard input\n",
      program, DEFAULT_SEGMENT_OUTPUT_LIMIT);
}

static bool print_segments(std::vector<std::string> const& segments) {
  for (size_t i = 0; i < segments.size(); ++i)
    printf("%s\n", format_pair_segment(segments[i]).c_str());
  return !ferror(stdout);
}

static bool find_segments(
    std::istream* input, char const* name, DfsPairSet const& ignored,
    DfsPairSet const& rejected,
    SegmentOutputOptions const& output_options) {
  std::unordered_set<std::string> found;
  std::vector<std::string> result;
  std::string line;
  uint64_t line_number = 0;
  while (result.size() < output_options.limit && std::getline(*input, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;

    char* score_end;
    (void) strtod(line.c_str(), &score_end);
    if (score_end == line.c_str() || *score_end != ' ' ||
        score_end[1] == '\0') {
      fprintf(stderr,
          "first-segments: %s:%" PRIu64
          ": expected \"score segment[,segment ...]\"\n",
          name, line_number);
      return false;
    }

    std::vector<std::string> segments;
    bool reject_line = false;
    size_t start = size_t(score_end - line.c_str()) + 1;
    while (true) {
      size_t const end = line.find(',', start);
      size_t const length =
          end == std::string::npos ? line.size() - start : end - start;
      if (length == 0) {
        fprintf(stderr,
            "first-segments: %s:%" PRIu64 ": empty segment\n",
            name, line_number);
        return false;
      }

      segments.push_back(line.substr(start, length));
      if (rejected.find(segments.back()) != rejected.end())
        reject_line = true;

      if (end == std::string::npos) break;
      start = end + 1;
    }

    if (reject_line) continue;
    for (std::string const& segment : segments) {
      if (result.size() == output_options.limit) break;
      if ((!output_options.pairs || is_pair_segment(segment)) &&
          ignored.find(segment) == ignored.end() &&
          found.insert(segment).second)
        result.push_back(segment);
    }
  }

  if (input->bad()) {
    fprintf(stderr, "first-segments: can't read \"%s\"\n", name);
    return false;
  }
  fprintf(stderr, "found segment %zu on line %" PRIu64 "\n",
      result.size(), line_number);
  return print_segments(result);
}

int main(int argc, char* argv[]) {
  PairFilterOptions filter_options;
  char const* results_path = NULL;
  bool parse_options = true;
  SegmentOutputOptions output_options;
  for (int i = 1; i < argc; ++i) {
    if (parse_options) {
      PairFilterOptionResult const result = parse_pair_filter_option(
          argc, argv, &i, "first-segments", true, true, &filter_options);
      if (result == PAIR_FILTER_OPTION_ERROR) {
        usage(stderr, argv[0]);
        return 2;
      }
      if (result == PAIR_FILTER_OPTION_HANDLED) continue;

      SegmentOutputOptionResult const output_result =
          parse_segment_output_option(
              argc, argv, &i, "first-segments", &output_options);
      if (output_result == SEGMENT_OUTPUT_OPTION_ERROR) {
        usage(stderr, argv[0]);
        return 2;
      }
      if (output_result == SEGMENT_OUTPUT_OPTION_HANDLED) continue;
    }

    if (parse_options && strcmp(argv[i], "--") == 0) {
      parse_options = false;
    } else if (parse_options &&
               (strcmp(argv[i], "-h") == 0 ||
                strcmp(argv[i], "--help") == 0)) {
      usage(stdout, argv[0]);
      return 0;
    } else if (parse_options && argv[i][0] == '-' && argv[i][1] != '\0') {
      fprintf(stderr, "first-segments: unknown option \"%s\"\n", argv[i]);
      usage(stderr, argv[0]);
      return 2;
    } else if (results_path != NULL) {
      fputs("first-segments: at most one RESULTS file may be given\n", stderr);
      usage(stderr, argv[0]);
      return 2;
    } else {
      results_path = argv[i];
    }
  }

  if (filter_options.workflow_yes && !filter_options.workflow) {
    fputs("first-segments: --yes requires --wf\n", stderr);
    usage(stderr, argv[0]);
    return 2;
  }

  DfsPairSet ignored;
  DfsPairSet rejected;
  if (!load_pair_filters(
          filter_options, "first-segments", &ignored, &rejected))
    return 1;

  if (results_path == NULL || strcmp(results_path, "-") == 0)
    return find_segments(
        &std::cin, "-", ignored, rejected, output_options) ? 0 : 1;

  errno = 0;
  std::ifstream input(results_path);
  if (!input.is_open()) {
    fprintf(stderr, "first-segments: can't open \"%s\": %s\n",
        results_path, strerror(errno));
    return 1;
  }
  return find_segments(
      &input, results_path, ignored, rejected, output_options) ? 0 : 1;
}
