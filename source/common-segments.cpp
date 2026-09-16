#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "dfs-cli-args.h"
#include "pair-exclusions.h"
#include "segment-output.h"
#include "segment-rows.h"

static constexpr PairFilterSupport kSupport = {
    .ignore = true, .allow = true, .workflow_yes = true};

struct Args {
  char const* results_path = NULL;
  char const* pairs_path = NULL;
  bool print_rows = false;
  PairFilterOptions filter_options;
};

static void usage(char const* program) {
  fprintf(stdout,
      "usage: %s [-i FILE | --ignore FILE]...\n"
      "          [-r FILE | --reject FILE]...\n"
      "          [-a FILE]...\n"
      "          [-d PATH]\n"
      "          [--wf | --wfroot DIR] [-t TARGET] [-y] [--results]\n"
      "          RESULTS PAIRS\n"
      "  print the segments of the result rows that hold two or more PAIRS,\n"
      "  one per line in ascending order, in pair-file format\n"
      "  RESULTS             dfs-anagrams result rows; - reads standard\n"
      "                      input\n"
      "  PAIRS               word,word lines, as in best.pairs\n"
      "  -i, --ignore FILE   do not print pairs listed in FILE; may be\n"
      "                      repeated\n"
      "  --results           print the result rows holding two or more\n"
      "                      pairs, as read, instead of their segments\n",
      program);
  print_reject_option_help(stdout, 22);
  print_allow_pairs_option_help(stdout, 22);
  fprintf(stdout,
      "  -d, --dict PATH     discard rows with any word not in PATH; with\n"
      "                      --wf or --wfroot, defaults to DIR/%s\n"
      "  --wfroot DIR        implies -r DIR/%s; also implies\n"
      "                      -r on the selected target's\n"
      "                      DIR/%s\n"
      "  --wf                shortcut for --wfroot $WFROOT\n"
      "  -t, --target TARGET with --wf or --wfroot, the target selected by\n"
      "                      DIR/.wf/best/TARGET; defaults to the target\n"
      "                      RESULTS names of its own, by its directory or\n"
      "                      by its name, and otherwise to %s; an explicit\n"
      "                      TARGET must be that same target: %s\n"
      "  -y, --yes           with --wf or --wfroot, ignore pairs in the\n"
      "                      selected root's %s\n",
      WORKFLOW_DICT_PATH, WORKFLOW_NO_PAIRS_PATH, WORKFLOW_TARGET_NO_PAIRS_PATH,
      WORKFLOW_DEFAULT_TARGET, WORKFLOW_RESULTS_NAME,
      WORKFLOW_YES_PAIRS_PATH);
}

static bool parse_args(
    int argc, char* argv[], Args* out, bool* requested_help) {
  *requested_help = false;
  std::vector<char const*> paths;
  PairFilterOptions filter_options;
  bool print_rows = false;
  bool parse_options = true;
  for (int i = 1; i < argc; ++i) {
    if (parse_options) {
      PairFilterOptionResult const filter_result = parse_pair_filter_option(
          argc, argv, &i, "common-segments", kSupport, &filter_options);
      if (filter_result == PAIR_FILTER_OPTION_ERROR) {
        usage(argv[0]);
        return false;
      }
      if (filter_result == PAIR_FILTER_OPTION_HANDLED) continue;
    }

    if (parse_options && strcmp(argv[i], "--") == 0) {
      parse_options = false;
    } else if (parse_options && strcmp(argv[i], "--results") == 0) {
      print_rows = true;
    } else if (parse_options &&
               (strcmp(argv[i], "-h") == 0 ||
                strcmp(argv[i], "--help") == 0)) {
      *requested_help = true;
      return true;
    } else if (parse_options && argv[i][0] == '-' && argv[i][1] != '\0') {
      fprintf(stderr, "common-segments: unknown option \"%s\"\n", argv[i]);
      usage(argv[0]);
      return false;
    } else {
      paths.push_back(argv[i]);
    }
  }

  if (!check_pair_filter_options(filter_options, "common-segments")) {
    usage(argv[0]);
    return false;
  }
  if (paths.size() != 2) {
    fputs("common-segments: RESULTS and PAIRS are both required\n", stderr);
    usage(argv[0]);
    return false;
  }

  if (strcmp(paths[0], "-") != 0) filter_options.input_path = paths[0];

  out->results_path = paths[0];
  out->pairs_path = paths[1];
  out->print_rows = print_rows;
  out->filter_options = std::move(filter_options);
  return true;
}

static bool any_segment_in(
    std::vector<std::string> const& segments, DfsPairSet const& pairs) {
  for (std::string const& segment : segments)
    if (pairs.find(segment) != pairs.end()) return true;
  return false;
}

static bool any_rejected_segment(
    std::vector<std::string> const& segments, DfsPairSet const& rejected) {
  for (std::string const& segment : segments)
    if (is_rejected_segment(rejected, segment)) return true;
  return false;
}

static bool any_segment_disallowed(
    std::vector<std::string> const& segments,
    std::optional<DfsPairSet> const& allowed) {
  for (std::string const& segment : segments)
    if (!is_allowed_segment(allowed, segment)) return true;
  return false;
}

static bool any_segment_outside(
    std::vector<std::string> const& segments,
    DfsDictionary const& dictionary) {
  for (std::string const& segment : segments)
    if (!all_words_in_dict(dictionary, segment)) return true;
  return false;
}

static bool collect_common(
    std::istream* input, char const* name, PairFilters const& filters,
    DfsPairSet const& pairs, bool print_rows, std::set<std::string>* common,
    uint64_t* holding_rows) {
  SegmentRowReader reader = {input, name, "common-segments"};
  SegmentRow row;
  std::set<std::string> held;
  while (segment_rows_next(&reader, &row)) {
    // The row filters in segment-counts' precedence. Nothing is attributed
    // here, so the first match simply drops the row.
    if (any_segment_in(row.segments, filters.sources.classified_no)) continue;
    if (any_segment_in(row.segments, filters.sources.target_no)) continue;
    if (any_rejected_segment(row.segments, filters.rejected)) continue;
    if (any_segment_disallowed(row.segments, filters.allowed)) continue;
    if (any_segment_outside(row.segments, filters.dictionary)) continue;

    held.clear();
    for (std::string const& segment : row.segments)
      if (pairs.find(segment) != pairs.end())
        held.insert(canonical_pair_segment(segment));
    if (held.size() < 2) continue;
    ++*holding_rows;

    if (print_rows) {
      printf("%s\n", row.line.c_str());
      continue;
    }

    // A row holding exactly two pairs lies in one intersection, and that
    // intersection excludes both of them; a third pair puts every pair in
    // some other pair's intersection.
    bool const hold_back_pairs = held.size() == 2;
    for (std::string const& segment : row.segments) {
      if (hold_back_pairs && pairs.find(segment) != pairs.end()) continue;
      if (filters.sources.classified_yes.find(segment) !=
          filters.sources.classified_yes.end())
        continue;
      if (filters.ignored.find(segment) != filters.ignored.end()) continue;
      common->insert(segment);
    }
  }

  return !reader.failed;
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
          args.filter_options, "common-segments", kSupport, &filters))
    return 1;

  DfsPairSet pairs;
  if (!load_pair_file(args.pairs_path, "pair list", &pairs, true, true))
    return 1;

  std::set<std::string> common;
  uint64_t holding_rows = 0;
  if (strcmp(args.results_path, "-") == 0) {
    if (!collect_common(&std::cin, "-", filters, pairs, args.print_rows,
            &common, &holding_rows))
      return 1;
  } else {
    errno = 0;
    std::ifstream input(args.results_path);
    if (!input.is_open()) {
      fprintf(stderr, "common-segments: can't open \"%s\": %s\n",
          args.results_path, strerror(errno));
      return 1;
    }
    if (!collect_common(&input, args.results_path, filters, pairs,
            args.print_rows, &common, &holding_rows))
      return 1;
  }

  if (holding_rows == 0) {
    fputs("common-segments: no result row holds two or more pairs\n", stderr);
    return 0;
  }

  for (std::string const& segment : common)
    printf("%s\n", format_pair_segment(segment).c_str());
  return ferror(stdout) ? 1 : 0;
}
