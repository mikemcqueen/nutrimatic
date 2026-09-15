#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fstream>
#include <iostream>
#include <string>

#include "pair-exclusions.h"
#include "segment-output.h"

static void usage(char const* program) {
  fprintf(stdout,
      "usage: %s [-n N] [-r FILE | --reject FILE]...\n"
      "          [-a FILE]...\n"
      "          [-d PATH]\n"
      "          [--wf | --wfroot DIR] [-t TARGET] [FILE]\n"
      "  print dfs-anagrams result lines that contain no rejected segment\n"
      "  -n N                 print at most N result lines\n",
      program);
  print_reject_option_help(stdout, 23);
  print_allow_pairs_option_help(stdout, 23);
  fprintf(stdout,
      "  -d, --dict PATH      discard rows with any word not in PATH; with\n"
      "                       --wf or --wfroot, defaults to DIR/%s\n"
      "  --wfroot DIR         implies -r DIR/%s; also implies\n"
      "                       -r on the selected target's\n"
      "                       DIR/%s\n"
      "  --wf                 shortcut for --wfroot $WFROOT\n"
      "  -t, --target TARGET  with --wf or --wfroot, the target selected by\n"
      "                       DIR/.wf/best/TARGET; defaults to the target\n"
      "                       FILE names of its own, by its directory or by\n"
      "                       its name, and otherwise to %s; an explicit\n"
      "                       TARGET must be that same target: %s\n"
      "  with no FILE, or when FILE is -, read standard input\n",
      WORKFLOW_DICT_PATH, WORKFLOW_NO_PAIRS_PATH, WORKFLOW_TARGET_NO_PAIRS_PATH,
      WORKFLOW_DEFAULT_TARGET, WORKFLOW_RESULTS_NAME);
}

static bool filter_stream(
    std::istream* input, char const* name, DfsPairSet const& rejected,
    std::optional<DfsPairSet> const& allowed,
    DfsDictionary const& dictionary, bool have_limit, uint64_t limit) {
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

      std::string const segment = line.substr(start, length);
      if (is_rejected_segment(rejected, segment) ||
          !is_allowed_segment(allowed, segment) ||
          !all_words_in_dict(dictionary, segment))
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
          argc, argv, &i, "filter-segments", false, false, &filter_options,
          true);
      if (result == PAIR_FILTER_OPTION_ERROR) {
        usage(argv[0]);
        return 2;
      }
      if (result == PAIR_FILTER_OPTION_HANDLED) continue;
    }

    if (parse_options && strcmp(argv[i], "--") == 0) {
      parse_options = false;
    } else if (parse_options &&
               (strcmp(argv[i], "-h") == 0 ||
                strcmp(argv[i], "--help") == 0)) {
      usage(argv[0]);
      return 0;
    } else if (parse_options && strcmp(argv[i], "-n") == 0) {
      if (++i == argc || !parse_limit(argv[i], &limit)) {
        fputs("filter-segments: -n requires a non-negative integer\n",
            stderr);
        usage(argv[0]);
        return 2;
      }
      have_limit = true;
    } else if (parse_options && argv[i][0] == '-' && argv[i][1] != '\0') {
      fprintf(stderr, "filter-segments: unknown option \"%s\"\n", argv[i]);
      usage(argv[0]);
      return 2;
    } else if (results_path != NULL) {
      fputs("filter-segments: at most one FILE may be specified\n", stderr);
      usage(argv[0]);
      return 2;
    } else {
      results_path = argv[i];
    }
  }

  if (!check_pair_filter_options(filter_options, "filter-segments")) {
    usage(argv[0]);
    return 2;
  }

  if (results_path != NULL && strcmp(results_path, "-") != 0)
    filter_options.input_path = results_path;

  DfsPairSet ignored;
  DfsPairSet rejected;
  DfsDictionary dictionary;
  std::optional<DfsPairSet> allowed;
  if (!load_pair_filters(
          filter_options, "filter-segments", &ignored, &rejected, &dictionary,
          &allowed))
    return 1;

  if (results_path == NULL || strcmp(results_path, "-") == 0)
    return filter_stream(
        &std::cin, "-", rejected, allowed, dictionary, have_limit, limit)
        ? 0 : 1;

  errno = 0;
  std::ifstream input(results_path);
  if (!input.is_open()) {
    fprintf(stderr, "filter-segments: can't open \"%s\": %s\n",
        results_path, strerror(errno));
    return 1;
  }
  return filter_stream(
      &input, results_path, rejected, allowed, dictionary, have_limit, limit)
      ? 0 : 1;
}
