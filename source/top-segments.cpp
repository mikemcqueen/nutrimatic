#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "dfs-cli-args.h"
#include "option-value.h"
#include "pair-exclusions.h"
#include "segment-counts.h"
#include "segment-output.h"

static constexpr PairFilterSupport kSupport = {
    .ignore = true, .allow = true, .workflow_yes = true};

struct Args {
  std::vector<char const*> paths;
  PairFilterOptions filter_options;
  SegmentCountsOptions options;
};

static void usage(char const* program) {
  fprintf(stdout,
      "usage: %s [--pairs | --solo-words | --all-words |\n"
      "          --pair-words [--unique]]\n"
      "          [-c | --no-counts] [-l | --elim] [-n N]\n"
      "          [-u LETTERS | --used-letters LETTERS]...\n"
      "          [-i FILE | --ignore FILE]...\n"
      "          [-r FILE | --reject FILE]...\n"
      "          [-a FILE]...\n"
      "          [-d PATH]\n"
      "          [--wf | --wfroot DIR] [-t TARGET] [-y]\n"
      "          [FILE ...]\n"
      "  count comma-delimited segments in dfs-anagrams output and print\n"
      "  results in descending count order\n"
      "  --pairs             print only multi-word segments as\n"
      "                      comma-separated words\n"
      "  -c, --counts        include counts; default except with --pairs\n"
      "  --no-counts, --nc   omit counts; default with --pairs\n"
      "  --solo-words        print only single-word segments\n"
      "  --all-words         count every word occurrence, splitting\n"
      "                      multi-word segments into their words\n"
      "  --pair-words        count words occurring in multi-word segments\n"
      "  --unique            with --pair-words, count each distinct\n"
      "                      multi-word segment once\n"
      "  -l, --by-length     sort by descending non-space character length\n"
      "  --elim              sort by guaranteed result-line elimination and\n"
      "                      print \"DECISION REQUIRE REJECT COUNT SEGMENT\"\n"
      "                      rows\n"
      "  -n N                print at most N rows; 0 prints all; defaults to "
      "%" PRIu64 "\n"
      "  -u, --used-letters LETTERS\n"
      "                      print only segments made from the first input\n"
      "                      row's letters less LETTERS; may be repeated\n"
      "  -i, --ignore FILE   do not count pairs listed in FILE; may be\n"
      "                      repeated\n",
      program, DEFAULT_SEGMENT_OUTPUT_LIMIT);
  print_reject_option_help(stdout, 22);
  print_allow_pairs_option_help(stdout, 22);
  fprintf(stdout,
      "  -d, --dict PATH     discard rows with any word not in PATH; with\n"
      "                      --wf or --wfroot, defaults to DIR/%s\n"
      "  --wfroot DIR        implies -r DIR/%s; also implies\n"
      "                      -r on the selected target's\n"
      "                      DIR/%s\n"
      "  --wf                shortcut for --wfroot $WFROOT; with no unit\n"
      "                      option, --wf and --wfroot imply --pairs -y\n"
      "  -t, --target TARGET with --wf or --wfroot, the target selected by\n"
      "                      DIR/.wf/best/TARGET; defaults to the target a\n"
      "                      FILE names of its own, by its directory or by\n"
      "                      its name, and otherwise to %s; an explicit\n"
      "                      TARGET must be that same target: %s\n"
      "  -y, --yes           with --wf or --wfroot, ignore pairs in the\n"
      "                      selected root's %s\n"
      "  with no FILE, or when FILE is -, read standard input\n",
      WORKFLOW_DICT_PATH, WORKFLOW_NO_PAIRS_PATH, WORKFLOW_TARGET_NO_PAIRS_PATH,
      WORKFLOW_DEFAULT_TARGET, WORKFLOW_RESULTS_NAME,
      WORKFLOW_YES_PAIRS_PATH);
}

static bool parse_args(
    int argc, char* argv[], Args* out, bool* requested_help) {
  *requested_help = false;
  std::vector<char const*> paths;
  PairFilterOptions filter_options;
  bool parse_options = true;
  bool force_counts = false;
  bool suppress_counts = false;
  bool elimination = false;
  std::optional<std::string> used_letters;
  SegmentOutputOptions output_options;
  for (int i = 1; i < argc; ++i) {
    if (parse_options) {
      if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--counts") == 0) {
        force_counts = true;
        continue;
      }
      if (strcmp(argv[i], "--no-counts") == 0 ||
          strcmp(argv[i], "--nc") == 0) {
        suppress_counts = true;
        continue;
      }
      if (strcmp(argv[i], "--elim") == 0) {
        elimination = true;
        continue;
      }

      PairFilterOptionResult const filter_result = parse_pair_filter_option(
          argc, argv, &i, "top-segments", kSupport, &filter_options);
      if (filter_result == PAIR_FILTER_OPTION_ERROR) {
        usage(argv[0]);
        return false;
      }
      if (filter_result == PAIR_FILTER_OPTION_HANDLED) continue;

      SegmentOutputOptionResult const result = parse_segment_output_option(
          argc, argv, &i, "top-segments", &output_options);
      if (result == SEGMENT_OUTPUT_OPTION_ERROR) {
        usage(argv[0]);
        return false;
      }
      if (result == SEGMENT_OUTPUT_OPTION_HANDLED) continue;

      char const* value;
      if (match_option_value(
              argc, argv, &i, "-u", "--used-letters", &value)) {
        if (value == NULL) {
          fputs("top-segments: -u requires LETTERS\n", stderr);
          usage(argv[0]);
          return false;
        }
        if (!used_letters) used_letters.emplace();
        if (!clean_letters(value, "used letters", &*used_letters)) {
          usage(argv[0]);
          return false;
        }
        continue;
      }
    }

    if (parse_options && strcmp(argv[i], "--") == 0) {
      parse_options = false;
    } else if (parse_options &&
               (strcmp(argv[i], "-h") == 0 ||
                strcmp(argv[i], "--help") == 0)) {
      *requested_help = true;
      return true;
    } else if (parse_options && argv[i][0] == '-' && argv[i][1] != '\0') {
      fprintf(stderr, "top-segments: unknown option \"%s\"\n", argv[i]);
      usage(argv[0]);
      return false;
    } else {
      paths.push_back(argv[i]);
    }
  }

  if (!check_pair_filter_options(filter_options, "top-segments")) {
    usage(argv[0]);
    return false;
  }
  bool const workflow =
      filter_options.workflow || !filter_options.workflow_root.empty();
  if (workflow && !output_options.mode_explicit) {
    output_options.selection = SEGMENT_SELECTION_PAIRS;
    output_options.projection = SEGMENT_PROJECTION_SEGMENTS;
    filter_options.workflow_yes = true;
  }
  bool const pair_segments =
      output_options.selection == SEGMENT_SELECTION_PAIRS &&
      output_options.projection == SEGMENT_PROJECTION_SEGMENTS;
  if (force_counts && suppress_counts) {
    fputs("top-segments: --counts and --no-counts are mutually exclusive\n",
        stderr);
    usage(argv[0]);
    return false;
  }
  if (elimination && suppress_counts) {
    fputs("top-segments: --elim and --no-counts are mutually exclusive\n",
        stderr);
    usage(argv[0]);
    return false;
  }
  bool const show_counts = force_counts ||
      (!suppress_counts && !pair_segments);
  if (!check_segment_output_options(output_options, "top-segments")) {
    usage(argv[0]);
    return false;
  }
  if (elimination && output_options.by_length) {
    fputs("top-segments: --elim and --by-length are mutually exclusive\n",
        stderr);
    usage(argv[0]);
    return false;
  }

  if (paths.empty()) paths.push_back("-");

  // Only a single named file names a single target; several files may sit in
  // several, and standard input sits in none.
  if (paths.size() == 1 && strcmp(paths[0], "-") != 0)
    filter_options.input_path = paths[0];

  out->paths = std::move(paths);
  out->filter_options = std::move(filter_options);
  out->options.output = output_options;
  out->options.elimination = elimination;
  out->options.show_counts = show_counts;
  out->options.used_letters = std::move(used_letters);
  return true;
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
          args.filter_options, "top-segments", kSupport, &filters))
    return 1;

  SegmentCountsData data;
  data.program = "top-segments";
  for (size_t i = 0; i < args.paths.size(); ++i) {
    char const* const path = args.paths[i];
    if (strcmp(path, "-") == 0) {
      if (!segment_counts_read(&std::cin, "-", filters, args.options, &data))
        return 1;
      continue;
    }

    errno = 0;
    std::ifstream input(path);
    if (!input.is_open()) {
      fprintf(stderr, "top-segments: can't open \"%s\": %s\n",
          path, strerror(errno));
      return 1;
    }
    if (!segment_counts_read(&input, path, filters, args.options, &data))
      return 1;
  }

  segment_counts_print_filter_summary(data, filters.sources);
  return segment_counts_print_top(data, args.options) ? 0 : 1;
}
