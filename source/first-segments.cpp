#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

#include "pair-exclusions.h"

static void usage(FILE* fp, char const* program) {
  fprintf(fp,
      "usage: %s -n N [-x FILE | --exclude FILE]... [--wf] RESULTS\n"
      "  print the first N distinct, non-excluded segments from\n"
      "  dfs-anagrams RESULTS as comma-separated pairs\n"
      "  -n N                 maximum number of segments to print\n"
      "  -x, --exclude FILE   exclude comma-separated pairs from FILE;\n"
      "                       may be repeated\n"
      "  --wf                 exclude classified YES and NO pairs below\n"
      "                       $WFROOT; missing files produce warnings\n",
      program);
}

static bool parse_limit(char const* text, uint64_t* limit) {
  if (text[0] == '\0' || text[0] == '-') return false;
  errno = 0;
  char* end;
  unsigned long long const parsed = strtoull(text, &end, 10);
  if (errno == ERANGE || *end != '\0') return false;
  *limit = parsed;
  return true;
}

static bool print_segments(std::vector<std::string> const& segments) {
  for (size_t i = 0; i < segments.size(); ++i) {
    std::string pair = segments[i];
    std::replace(pair.begin(), pair.end(), ' ', ',');
    printf("%s\n", pair.c_str());
  }
  return !ferror(stdout);
}

static bool find_segments(
    std::istream* input, char const* name, DfsPairSet const& excluded,
    uint64_t limit) {
  std::unordered_set<std::string> found;
  std::vector<std::string> result;
  std::string line;
  uint64_t line_number = 0;
  while (result.size() < limit && std::getline(*input, line)) {
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

    size_t start = size_t(score_end - line.c_str()) + 1;
    while (result.size() < limit) {
      size_t const end = line.find(',', start);
      size_t const length =
          end == std::string::npos ? line.size() - start : end - start;
      if (length == 0) {
        fprintf(stderr,
            "first-segments: %s:%" PRIu64 ": empty segment\n",
            name, line_number);
        return false;
      }

      std::string const segment = line.substr(start, length);
      if (excluded.find(segment) == excluded.end() &&
          found.insert(segment).second)
        result.push_back(segment);

      if (end == std::string::npos) break;
      start = end + 1;
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
  PairExclusionOptions exclusion_options;
  char const* results_path = NULL;
  bool parse_options = true;
  bool have_limit = false;
  uint64_t limit = 0;
  for (int i = 1; i < argc; ++i) {
    if (parse_options) {
      PairExclusionOptionResult const result = parse_pair_exclusion_option(
          argc, argv, &i, "first-segments", &exclusion_options);
      if (result == PAIR_EXCLUSION_OPTION_ERROR) {
        usage(stderr, argv[0]);
        return 2;
      }
      if (result == PAIR_EXCLUSION_OPTION_HANDLED) continue;
    }

    if (parse_options && strcmp(argv[i], "--") == 0) {
      parse_options = false;
    } else if (parse_options &&
               (strcmp(argv[i], "-h") == 0 ||
                strcmp(argv[i], "--help") == 0)) {
      usage(stdout, argv[0]);
      return 0;
    } else if (parse_options &&
               (strcmp(argv[i], "-n") == 0)) {
      if (++i == argc || !parse_limit(argv[i], &limit)) {
        fputs("first-segments: -n requires a non-negative integer\n", stderr);
        usage(stderr, argv[0]);
        return 2;
      }
      have_limit = true;
    } else if (parse_options && argv[i][0] == '-' && argv[i][1] != '\0') {
      fprintf(stderr, "first-segments: unknown option \"%s\"\n", argv[i]);
      usage(stderr, argv[0]);
      return 2;
    } else if (results_path != NULL) {
      fputs("first-segments: exactly one RESULTS file is required\n", stderr);
      usage(stderr, argv[0]);
      return 2;
    } else {
      results_path = argv[i];
    }
  }

  if (!have_limit || results_path == NULL) {
    fputs("first-segments: -n N and RESULTS are required\n", stderr);
    usage(stderr, argv[0]);
    return 2;
  }

  DfsPairSet excluded;
  if (!load_pair_exclusions(
          exclusion_options, "first-segments", &excluded))
    return 1;

  if (strcmp(results_path, "-") == 0)
    return find_segments(&std::cin, "-", excluded, limit) ? 0 : 1;

  errno = 0;
  std::ifstream input(results_path);
  if (!input.is_open()) {
    fprintf(stderr, "first-segments: can't open \"%s\": %s\n",
        results_path, strerror(errno));
    return 1;
  }
  return find_segments(&input, results_path, excluded, limit) ? 0 : 1;
}
