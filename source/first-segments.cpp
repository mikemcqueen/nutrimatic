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

struct FoundSegment {
  std::string text;
  size_t length;
};

static void usage(FILE* fp, char const* program) {
  fprintf(fp,
      "usage: %s [--pairs | --solo-words | --all-words]\n"
      "          [-l]\n"
      "          [-n N]\n"
      "          [-i FILE | --ignore FILE]...\n"
      "          [-r FILE | --reject FILE]...\n"
      "          [--wf | --wfroot DIR] [-t TARGET] [-y]\n"
      "          [RESULTS]\n"
      "  print the first N distinct, non-ignored segments from valid\n"
      "  dfs-anagrams RESULTS rows as comma-separated pairs\n"
      "  --pairs              print only multi-word segments\n"
      "  --solo-words         print only single-word segments\n"
      "  --all-words          print the first N distinct words, splitting\n"
      "                       multi-word segments into their words\n"
      "  -l, --by-length      sort by descending non-space character length\n"
      "  -n N                 maximum number of segments to print; defaults\n"
      "                       to %" PRIu64 "\n"
      "  -i, --ignore FILE    do not select pairs listed in FILE; may be\n"
      "                       repeated\n",
      program, DEFAULT_SEGMENT_OUTPUT_LIMIT);
  print_reject_option_help(fp, 23);
  fprintf(fp,
      "  --wfroot DIR         implies -r DIR/%s; discards\n"
      "                       rows with any word not in DIR/%s;\n"
      "                       also implies -r on the selected target's\n"
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
      WORKFLOW_NO_PAIRS_PATH, WORKFLOW_DICT_PATH, WORKFLOW_TARGET_NO_PAIRS_PATH,
      WORKFLOW_DEFAULT_TARGET, WORKFLOW_RESULTS_NAME,
      WORKFLOW_YES_PAIRS_PATH);
}

static bool print_segments(std::vector<FoundSegment> const& segments) {
  for (size_t i = 0; i < segments.size(); ++i)
    printf("%s\n", format_pair_segment(segments[i].text).c_str());
  return !ferror(stdout);
}

static bool find_segments(
    std::istream* input, char const* name, DfsPairSet const& ignored,
    DfsPairSet const& rejected, DfsDictionary const& dictionary,
    SegmentOutputOptions const& output_options) {
  std::unordered_set<std::string> found;
  std::vector<FoundSegment> result;
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
      if (is_rejected_segment(rejected, segments.back()) ||
          !all_words_in_dict(dictionary, segments.back()))
        reject_line = true;

      if (end == std::string::npos) break;
      start = end + 1;
    }

    if (reject_line) continue;
    for (std::string const& segment : segments) {
      if (result.size() == output_options.limit) break;
      if (ignored.find(segment) != ignored.end()) continue;
      if (output_options.selection == SEGMENT_SELECTION_PAIRS &&
          !is_pair_segment(segment))
        continue;
      if (output_options.selection == SEGMENT_SELECTION_SOLO &&
          !is_solo_segment(segment))
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

  if (input->bad()) {
    fprintf(stderr, "first-segments: can't read \"%s\"\n", name);
    return false;
  }
  if (output_options.by_length) {
    std::stable_sort(result.begin(), result.end(),
        [](FoundSegment const& a, FoundSegment const& b) {
          return a.length > b.length;
        });
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

  if (!check_pair_filter_options(filter_options, "first-segments")) {
    usage(stderr, argv[0]);
    return 2;
  }

  if (results_path != NULL && strcmp(results_path, "-") != 0)
    filter_options.input_path = results_path;

  DfsPairSet ignored;
  DfsPairSet rejected;
  DfsDictionary dictionary;
  if (!load_pair_filters(
          filter_options, "first-segments", &ignored, &rejected, &dictionary))
    return 1;

  if (results_path == NULL || strcmp(results_path, "-") == 0)
    return find_segments(
        &std::cin, "-", ignored, rejected, dictionary, output_options)
        ? 0 : 1;

  errno = 0;
  std::ifstream input(results_path);
  if (!input.is_open()) {
    fprintf(stderr, "first-segments: can't open \"%s\": %s\n",
        results_path, strerror(errno));
    return 1;
  }
  return find_segments(
      &input, results_path, ignored, rejected, dictionary, output_options)
      ? 0 : 1;
}
