#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

#include "pair-exclusions.h"
#include "segment-output.h"
#include "segment-rows.h"

static constexpr PairFilterSupport kSupport = {
    .ignore = true, .workflow_yes = true};

struct FoundSegment {
  std::string text;
  size_t length;
};

static void usage(char const* program) {
  fprintf(stdout,
      "usage: %s [--pairs | --solo-words | --all-words |\n"
      "          --pair-words [--unique]]\n"
      "          [-l]\n"
      "          [-n N]\n"
      "          [-i FILE | --ignore FILE]...\n"
      "          [-r FILE | --reject FILE]...\n"
      "          [-d PATH]\n"
      "          [--wf | --wfroot DIR] [-t TARGET] [-y]\n"
      "          [RESULTS]\n"
      "  print the first N distinct, non-ignored segments from valid\n"
      "  dfs-anagrams RESULTS rows as comma-separated pairs\n"
      "  --pairs              print only multi-word segments\n"
      "  --solo-words         print only single-word segments\n"
      "  --all-words          print the first N distinct words, splitting\n"
      "                       multi-word segments into their words\n"
      "  --pair-words         print the first N distinct words from\n"
      "                       multi-word segments\n"
      "  --unique             with --pair-words, process each distinct\n"
      "                       multi-word segment once\n"
      "  -l, --by-length      sort by descending non-space character length\n"
      "  -n N                 maximum number of segments to print; defaults\n"
      "                       to %" PRIu64 "\n"
      "  -i, --ignore FILE    do not select pairs listed in FILE; may be\n"
      "                       repeated\n",
      program, DEFAULT_SEGMENT_OUTPUT_LIMIT);
  print_reject_option_help(stdout, 23);
  fprintf(stdout,
      "  -d, --dict PATH      discard rows with any word not in PATH; with\n"
      "                       --wf or --wfroot, defaults to DIR/%s\n"
      "  --wfroot DIR         implies -r DIR/%s; also implies\n"
      "                       -r on the selected target's\n"
      "                       DIR/%s\n"
      "  --wf                 shortcut for --wfroot $WFROOT\n"
      "  -t, --target TARGET  with --wf or --wfroot, the target selected by\n"
      "                       DIR/.wf/best/TARGET; defaults to the target\n"
      "                       RESULTS names of its own, by its directory or\n"
      "                       by its name, and otherwise to %s; an explicit\n"
      "                       TARGET must be that same target: %s\n"
      "  -y, --yes            with --wf or --wfroot, ignore pairs in the\n"
      "                       selected root's %s\n"
      "  with no RESULTS, or when RESULTS is -, read standard input\n",
      WORKFLOW_DICT_PATH, WORKFLOW_NO_PAIRS_PATH, WORKFLOW_TARGET_NO_PAIRS_PATH,
      WORKFLOW_DEFAULT_TARGET, WORKFLOW_RESULTS_NAME,
      WORKFLOW_YES_PAIRS_PATH);
}

static bool print_segments(std::vector<FoundSegment> const& segments) {
  for (size_t i = 0; i < segments.size(); ++i)
    printf("%s\n", format_pair_segment(segments[i].text).c_str());
  return !ferror(stdout);
}

static bool find_segments(
    std::istream* input, char const* name, PairFilters const& filters,
    SegmentOutputOptions const& output_options) {
  std::unordered_set<std::string> found;
  std::unordered_set<std::string> unique_segments;
  std::vector<FoundSegment> result;
  SegmentRowReader reader = {input, name, "first-segments"};
  SegmentRow row;
  while (result.size() < output_options.limit &&
         segment_rows_next(&reader, &row)) {
    bool reject_line = false;
    for (std::string const& segment : row.segments)
      if (is_rejected_segment(filters.rejected, segment) ||
          !all_words_in_dict(filters.dictionary, segment))
        reject_line = true;

    if (reject_line) continue;
    for (std::string const& segment : row.segments) {
      if (result.size() == output_options.limit) break;
      if (filters.ignored.find(segment) != filters.ignored.end()) continue;
      if (!is_selected_segment(output_options.selection, segment)) continue;
      if (output_options.weight == SEGMENT_WEIGHT_UNIQUE &&
          !unique_segments.insert(segment).second)
        continue;

      if (output_options.projection == SEGMENT_PROJECTION_SEGMENTS) {
        if (found.insert(segment).second) {
          result.push_back(
              FoundSegment{segment, segment_nonspace_length(segment)});
        }
        continue;
      }

      std::vector<std::string> const words = split_segment_words(segment);
      for (size_t i = 0; i < words.size(); ++i) {
        if (result.size() == output_options.limit) break;
        if (found.insert(words[i]).second)
          result.push_back(FoundSegment{words[i], words[i].size()});
      }
    }
  }

  if (reader.failed) return false;
  if (output_options.by_length) {
    std::stable_sort(result.begin(), result.end(),
        [](FoundSegment const& a, FoundSegment const& b) {
          return a.length > b.length;
        });
  }
  fprintf(stderr, "found segment %zu on line %" PRIu64 "\n",
      result.size(), reader.line_number);
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
          argc, argv, &i, "first-segments", kSupport, &filter_options);
      if (result == PAIR_FILTER_OPTION_ERROR) {
        usage(argv[0]);
        return 2;
      }
      if (result == PAIR_FILTER_OPTION_HANDLED) continue;

      SegmentOutputOptionResult const output_result =
          parse_segment_output_option(
              argc, argv, &i, "first-segments", &output_options);
      if (output_result == SEGMENT_OUTPUT_OPTION_ERROR) {
        usage(argv[0]);
        return 2;
      }
      if (output_result == SEGMENT_OUTPUT_OPTION_HANDLED) continue;
    }

    if (parse_options && strcmp(argv[i], "--") == 0) {
      parse_options = false;
    } else if (parse_options &&
               (strcmp(argv[i], "-h") == 0 ||
                strcmp(argv[i], "--help") == 0)) {
      usage(argv[0]);
      return 0;
    } else if (parse_options && argv[i][0] == '-' && argv[i][1] != '\0') {
      fprintf(stderr, "first-segments: unknown option \"%s\"\n", argv[i]);
      usage(argv[0]);
      return 2;
    } else if (results_path != NULL) {
      fputs("first-segments: at most one RESULTS file may be given\n", stderr);
      usage(argv[0]);
      return 2;
    } else {
      results_path = argv[i];
    }
  }

  if (!check_pair_filter_options(filter_options, "first-segments")) {
    usage(argv[0]);
    return 2;
  }
  if (!check_segment_output_options(output_options, "first-segments")) {
    usage(argv[0]);
    return 2;
  }

  if (results_path != NULL && strcmp(results_path, "-") != 0)
    filter_options.input_path = results_path;

  PairFilters filters;
  if (!load_pair_filters(
          filter_options, "first-segments", kSupport, &filters))
    return 1;

  if (results_path == NULL || strcmp(results_path, "-") == 0)
    return find_segments(&std::cin, "-", filters, output_options) ? 0 : 1;

  errno = 0;
  std::ifstream input(results_path);
  if (!input.is_open()) {
    fprintf(stderr, "first-segments: can't open \"%s\": %s\n",
        results_path, strerror(errno));
    return 1;
  }
  return find_segments(&input, results_path, filters, output_options) ? 0 : 1;
}
