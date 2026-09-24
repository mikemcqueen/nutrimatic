#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <iostream>
#include <regex>
#include <string>
#include <utility>

#include "option-value.h"
#include "pair-exclusions.h"
#include "segment-output.h"
#include "row-input.h"
#include "segment-rows.h"

static constexpr PairFilterSupport kSupport = {
    .allow = true, .sentence = true};

struct Args {
  PairFilterOptions filter_options;
  char const* results_path;
  std::regex with_regex;
  bool use_regex;
  bool show_score;
  bool have_limit;
  uint64_t limit;
};

static void usage(char const* program) {
  fprintf(stdout,
      "usage: %s [-n N] [--with-regex REGEX] [--no-score]\n"
      "          [-r FILE | --reject FILE]...\n"
      "          [-a FILE]...\n"
      "          [-d PATH]\n"
      "          [--wf | --wfroot DIR] [-t TARGET] [-s N] [FILE]\n"
      "  print dfs-anagrams result lines that contain no rejected segment\n"
      "  -n N                 print at most N result lines\n"
      "  --with-regex REGEX   keep only lines with a segment REGEX matches\n"
      "                       anywhere (ECMAScript syntax)\n"
      "  --no-score           omit the leading score from printed lines;\n"
      "                       the output can't be filtered or reranked\n",
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
      "                       TARGET must be that same target: %s\n",
      WORKFLOW_DICT_PATH, WORKFLOW_NO_PAIRS_PATH, WORKFLOW_TARGET_NO_PAIRS_PATH,
      WORKFLOW_DEFAULT_TARGET, WORKFLOW_RESULTS_NAME);
  print_sentence_option_help(stdout, 23);
  fputs("  with no FILE, or when FILE is -, read standard input\n", stdout);
}

static bool parse_args(
    int argc, char* argv[], Args* out, bool* requested_help) {
  *requested_help = false;
  PairFilterOptions filter_options;
  char const* results_path = NULL;
  bool parse_options = true;
  bool have_limit = false;
  uint64_t limit = 0;
  char const* with_regex_pattern = NULL;
  char const* value;
  bool show_score = true;
  for (int i = 1; i < argc; ++i) {
    if (parse_options) {
      PairFilterOptionResult const result = parse_pair_filter_option(
          argc, argv, &i, "filter-segments", kSupport, &filter_options);
      if (result == PAIR_FILTER_OPTION_ERROR) {
        usage(argv[0]);
        return false;
      }
      if (result == PAIR_FILTER_OPTION_HANDLED) continue;
    }

    if (parse_options && strcmp(argv[i], "--") == 0) {
      parse_options = false;
    } else if (parse_options &&
               (strcmp(argv[i], "-h") == 0 ||
                strcmp(argv[i], "--help") == 0)) {
      *requested_help = true;
      return true;
    } else if (parse_options &&
               match_option_value(argc, argv, &i, "-n", NULL, &value)) {
      if (value == NULL || !parse_limit(value, &limit)) {
        fputs("filter-segments: -n requires a non-negative integer\n",
            stderr);
        usage(argv[0]);
        return false;
      }
      have_limit = true;
    } else if (parse_options &&
               match_option_value(
                   argc, argv, &i, NULL, "--with-regex", &value)) {
      if (value == NULL || with_regex_pattern != NULL) {
        fputs("filter-segments: --with-regex requires one REGEX and may be"
            " given once\n", stderr);
        usage(argv[0]);
        return false;
      }
      with_regex_pattern = value;
    } else if (parse_options && strcmp(argv[i], "--no-score") == 0) {
      show_score = false;
    } else if (parse_options && argv[i][0] == '-' && argv[i][1] != '\0') {
      fprintf(stderr, "filter-segments: unknown option \"%s\"\n", argv[i]);
      usage(argv[0]);
      return false;
    } else if (results_path != NULL) {
      fputs("filter-segments: at most one FILE may be specified\n", stderr);
      usage(argv[0]);
      return false;
    } else {
      results_path = argv[i];
    }
  }

  if (!check_pair_filter_options(filter_options, "filter-segments")) {
    usage(argv[0]);
    return false;
  }

  if (with_regex_pattern != NULL) {
    try {
      out->with_regex.assign(with_regex_pattern);
    } catch (std::regex_error const& error) {
      fprintf(stderr, "filter-segments: bad --with-regex \"%s\": %s\n",
          with_regex_pattern, error.what());
      return false;
    }
  }

  if (results_path == NULL) results_path = "-";
  if (strcmp(results_path, "-") != 0)
    filter_options.input_path = results_path;

  out->filter_options = std::move(filter_options);
  out->results_path = results_path;
  out->use_regex = with_regex_pattern != NULL;
  out->show_score = show_score;
  out->have_limit = have_limit;
  out->limit = limit;
  return true;
}

static bool filter_stream(
    std::istream* input, char const* name, PairFilters const& filters,
    std::regex const* with_regex,
    bool show_score, bool have_limit, uint64_t limit) {
  SegmentRowReader reader = {input, name, "filter-segments"};
  SegmentRow row;
  uint64_t output_count = 0;
  while ((!have_limit || output_count < limit) &&
         segment_rows_next(&reader, &row)) {
    bool include = true;
    bool regex_matched = with_regex == NULL;
    for (std::string const& segment : row.segments) {
      if (filters.first_rejecting_layer(segment) != PAIR_FILTER_NONE)
        include = false;
      if (!regex_matched && std::regex_search(segment, *with_regex))
        regex_matched = true;
    }

    if (include && regex_matched) {
      printf("%s\n", show_score
          ? row.line.c_str() : row.line.c_str() + row.segments_start);
      if (ferror(stdout)) return false;
      ++output_count;
    }
  }

  if (reader.failed) return false;
  return fflush(stdout) == 0;
}

int main(int argc, char* argv[]) {
  Args args;
  bool requested_help;
  if (!parse_args(argc, argv, &args, &requested_help)) return 2;
  if (requested_help) {
    usage(argv[0]);
    return 0;
  }

  PairFilters filters;
  if (!load_pair_filters(
          args.filter_options, "filter-segments", kSupport, &filters))
    return 1;

  std::regex const* const with_regex_filter =
      args.use_regex ? &args.with_regex : NULL;
  return read_input_file("filter-segments", args.results_path,
      [&](std::istream& input) {
        return filter_stream(
            &input, args.results_path, filters, with_regex_filter,
            args.show_score, args.have_limit, args.limit);
      })
      ? 0 : 1;
}
