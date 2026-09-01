#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fstream>
#include <iostream>
#include <string>

#include "pair-exclusions.h"

static void usage(FILE* fp, char const* program) {
  fprintf(fp,
      "usage: %s [-n N] [-r FILE | --reject FILE]... [--wf] [FILE]\n"
      "  print dfs-anagrams result lines that contain no rejected segment\n"
      "  -n N                 print at most N result lines\n"
      "  -r, --reject FILE    discard rows containing pairs listed in FILE;\n"
      "                       may be repeated\n"
      "  --wf                 reject pairs listed in\n"
      "                       $WFROOT/.wf/classified/no/no.pairs; missing\n"
      "                       files produce warnings\n"
      "  with no FILE, or when FILE is -, read standard input\n",
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

static bool filter_stream(
    std::istream* input, char const* name, DfsPairSet const& rejected,
    bool have_limit, uint64_t limit) {
  std::string line;
  uint64_t line_number = 0;
  uint64_t output_count = 0;
  while ((!have_limit || output_count < limit) &&
         std::getline(*input, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;

    char* score_end;
    (void) strtod(line.c_str(), &score_end);
    if (score_end == line.c_str() || *score_end != ' ' ||
        score_end[1] == '\0') {
      fprintf(stderr,
          "filter-segments: %s:%" PRIu64
          ": expected \"score segment[,segment ...]\"\n",
          name, line_number);
      return false;
    }

    bool include = true;
    size_t start = size_t(score_end - line.c_str()) + 1;
    while (true) {
      size_t const end = line.find(',', start);
      size_t const length =
          end == std::string::npos ? line.size() - start : end - start;
      if (length == 0) {
        fprintf(stderr,
            "filter-segments: %s:%" PRIu64 ": empty segment\n",
            name, line_number);
        return false;
      }

      if (rejected.find(line.substr(start, length)) != rejected.end())
        include = false;

      if (end == std::string::npos) break;
      start = end + 1;
    }

    if (include) {
      printf("%s\n", line.c_str());
      if (ferror(stdout)) return false;
      ++output_count;
    }
  }

  if (input->bad()) {
    fprintf(stderr, "filter-segments: can't read \"%s\"\n", name);
    return false;
  }
  return fflush(stdout) == 0;
}

int main(int argc, char* argv[]) {
  PairFilterOptions filter_options;
  char const* results_path = NULL;
  bool parse_options = true;
  bool have_limit = false;
  uint64_t limit = 0;
  for (int i = 1; i < argc; ++i) {
    if (parse_options) {
      PairFilterOptionResult const result = parse_pair_filter_option(
          argc, argv, &i, "filter-segments", false, false, &filter_options);
      if (result == PAIR_FILTER_OPTION_ERROR) {
        usage(stderr, argv[0]);
        return 2;
      }
      if (result == PAIR_FILTER_OPTION_HANDLED) continue;
    }

    if (parse_options && strcmp(argv[i], "--") == 0) {
      parse_options = false;
    } else if (parse_options &&
               (strcmp(argv[i], "-h") == 0 ||
                strcmp(argv[i], "--help") == 0)) {
      usage(stdout, argv[0]);
      return 0;
    } else if (parse_options && strcmp(argv[i], "-n") == 0) {
      if (++i == argc || !parse_limit(argv[i], &limit)) {
        fputs("filter-segments: -n requires a non-negative integer\n",
            stderr);
        usage(stderr, argv[0]);
        return 2;
      }
      have_limit = true;
    } else if (parse_options && argv[i][0] == '-' && argv[i][1] != '\0') {
      fprintf(stderr, "filter-segments: unknown option \"%s\"\n", argv[i]);
      usage(stderr, argv[0]);
      return 2;
    } else if (results_path != NULL) {
      fputs("filter-segments: at most one FILE may be specified\n", stderr);
      usage(stderr, argv[0]);
      return 2;
    } else {
      results_path = argv[i];
    }
  }

  DfsPairSet ignored;
  DfsPairSet rejected;
  if (!load_pair_filters(
          filter_options, "filter-segments", &ignored, &rejected))
    return 1;

  if (results_path == NULL || strcmp(results_path, "-") == 0)
    return filter_stream(
        &std::cin, "-", rejected, have_limit, limit) ? 0 : 1;

  errno = 0;
  std::ifstream input(results_path);
  if (!input.is_open()) {
    fprintf(stderr, "filter-segments: can't open \"%s\": %s\n",
        results_path, strerror(errno));
    return 1;
  }
  return filter_stream(
      &input, results_path, rejected, have_limit, limit) ? 0 : 1;
}
