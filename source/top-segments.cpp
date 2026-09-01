#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "pair-exclusions.h"
#include "segment-output.h"

typedef std::unordered_map<std::string, uint64_t> SegmentCounts;

static void usage(FILE* fp, char const* program) {
  fprintf(fp,
      "usage: %s [--pairs] [-n N] [-i FILE | --ignore FILE]...\n"
      "          [-r FILE | --reject FILE]... [--wf [-y | --yes]]\n"
      "          [FILE ...]\n"
      "  count comma-delimited segments in dfs-anagrams output and print\n"
      "  \"count segment\" rows in descending count order\n"
      "  --pairs             print only multi-word segments as\n"
      "                      comma-separated words, without counts\n"
      "  -n N                print at most N rows; defaults to %" PRIu64 "\n"
      "  -i, --ignore FILE   do not count pairs listed in FILE; may be\n"
      "                      repeated\n"
      "  -r, --reject FILE   discard rows containing pairs listed in FILE;\n"
      "                      may be repeated\n"
      "  --wf                reject pairs listed in\n"
      "                      $WFROOT/.wf/classified/no/no.pairs; missing\n"
      "                      files produce warnings\n"
      "  -y, --yes           with --wf, ignore pairs listed in\n"
      "                      $WFROOT/.wf/classified/yes/yes.pairs\n"
      "  with no FILE, or when FILE is -, read standard input\n",
      program, DEFAULT_SEGMENT_OUTPUT_LIMIT);
}

static bool count_stream(
    std::istream* input, char const* name, DfsPairSet const& ignored,
    DfsPairSet const& rejected, SegmentCounts* counts) {
  std::string line;
  uint64_t line_number = 0;
  while (std::getline(*input, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;

    char* score_end;
    (void) strtod(line.c_str(), &score_end);
    if (score_end == line.c_str() || *score_end != ' ' ||
        score_end[1] == '\0') {
      fprintf(stderr,
          "top-segments: %s:%" PRIu64
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
            "top-segments: %s:%" PRIu64 ": empty segment\n",
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
      if (ignored.find(segment) != ignored.end()) continue;
      uint64_t& count = (*counts)[segment];
      if (count == std::numeric_limits<uint64_t>::max()) {
        fprintf(stderr, "top-segments: segment count overflow\n");
        return false;
      }
      ++count;
    }
  }

  if (input->bad()) {
    fprintf(stderr, "top-segments: can't read \"%s\"\n", name);
    return false;
  }
  return true;
}

static bool print_counts(
    SegmentCounts const& counts, bool pairs, uint64_t limit) {
  std::vector<SegmentCounts::const_iterator> ordered;
  ordered.reserve(counts.size());
  uint64_t largest = 0;
  for (SegmentCounts::const_iterator entry = counts.begin();
       entry != counts.end(); ++entry) {
    if (pairs && !is_pair_segment(entry->first)) continue;
    ordered.push_back(entry);
    largest = std::max(largest, entry->second);
  }
  size_t const top = limit < ordered.size() ? size_t(limit) : ordered.size();
  std::partial_sort(ordered.begin(), ordered.begin() + top, ordered.end(),
    [](SegmentCounts::const_iterator a, SegmentCounts::const_iterator b) {
      if (a->second != b->second) return a->second > b->second;
      return a->first < b->first;
    });

  int const width = snprintf(NULL, 0, "%" PRIu64, largest);
  for (size_t i = 0; i < top; ++i) {
    if (pairs) {
      printf("%s\n", format_pair_segment(ordered[i]->first).c_str());
    } else {
      printf("%*" PRIu64 " %s\n", width, ordered[i]->second,
          ordered[i]->first.c_str());
    }
  }
  return !ferror(stdout);
}

int main(int argc, char* argv[]) {
  std::vector<char const*> paths;
  PairFilterOptions filter_options;
  bool parse_options = true;
  SegmentOutputOptions output_options;
  for (int i = 1; i < argc; ++i) {
    if (parse_options) {
      PairFilterOptionResult const filter_result = parse_pair_filter_option(
          argc, argv, &i, "top-segments", true, true, &filter_options);
      if (filter_result == PAIR_FILTER_OPTION_ERROR) {
        usage(stderr, argv[0]);
        return 2;
      }
      if (filter_result == PAIR_FILTER_OPTION_HANDLED) continue;

      SegmentOutputOptionResult const result = parse_segment_output_option(
          argc, argv, &i, "top-segments", &output_options);
      if (result == SEGMENT_OUTPUT_OPTION_ERROR) {
        usage(stderr, argv[0]);
        return 2;
      }
      if (result == SEGMENT_OUTPUT_OPTION_HANDLED) continue;
    }

    if (parse_options && strcmp(argv[i], "--") == 0) {
      parse_options = false;
    } else if (parse_options &&
               (strcmp(argv[i], "-h") == 0 ||
                strcmp(argv[i], "--help") == 0)) {
      usage(stdout, argv[0]);
      return 0;
    } else if (parse_options && argv[i][0] == '-' && argv[i][1] != '\0') {
      fprintf(stderr, "top-segments: unknown option \"%s\"\n", argv[i]);
      usage(stderr, argv[0]);
      return 2;
    } else {
      paths.push_back(argv[i]);
    }
  }

  if (filter_options.workflow_yes && !filter_options.workflow) {
    fputs("top-segments: --yes requires --wf\n", stderr);
    usage(stderr, argv[0]);
    return 2;
  }

  DfsPairSet ignored;
  DfsPairSet rejected;
  if (!load_pair_filters(
          filter_options, "top-segments", &ignored, &rejected))
    return 1;

  SegmentCounts counts;
  if (paths.empty()) {
    if (!count_stream(&std::cin, "-", ignored, rejected, &counts)) return 1;
  } else {
    for (size_t i = 0; i < paths.size(); ++i) {
      if (strcmp(paths[i], "-") == 0) {
        if (!count_stream(&std::cin, "-", ignored, rejected, &counts))
          return 1;
        continue;
      }

      errno = 0;
      std::ifstream input(paths[i]);
      if (!input.is_open()) {
        fprintf(stderr, "top-segments: can't open \"%s\": %s\n",
            paths[i], strerror(errno));
        return 1;
      }
      if (!count_stream(&input, paths[i], ignored, rejected, &counts))
        return 1;
    }
  }

  return print_counts(
      counts, output_options.pairs, output_options.limit) ? 0 : 1;
}
