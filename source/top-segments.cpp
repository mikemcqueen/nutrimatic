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

struct SegmentStats {
  uint64_t count;
  size_t length;
};

typedef std::unordered_map<std::string, SegmentStats> SegmentCounts;

static void usage(FILE* fp, char const* program) {
  fprintf(fp,
      "usage: %s [--pairs [-c] | --solo-words | --all-words |\n"
      "          --pair-words [--unique]]\n"
      "          [-l] [-n N]\n"
      "          [-i FILE | --ignore FILE]...\n"
      "          [-r FILE | --reject FILE]...\n"
      "          [--wf | --wfroot DIR] [-t TARGET] [-y]\n"
      "          [FILE ...]\n"
      "  count comma-delimited segments in dfs-anagrams output and print\n"
      "  \"count segment\" rows in descending count order\n"
      "  --pairs             print only multi-word segments as\n"
      "                      comma-separated words, without counts\n"
      "  -c, --counts        include counts with --pairs\n"
      "  --solo-words        print only single-word segments\n"
      "  --all-words         count every word occurrence, splitting\n"
      "                      multi-word segments into their words\n"
      "  --pair-words        count words occurring in multi-word segments\n"
      "  --unique            with --pair-words, count each distinct\n"
      "                      multi-word segment once\n"
      "  -l, --by-length     sort by descending non-space character length\n"
      "  -n N                print at most N rows; 0 prints all; defaults to "
      "%" PRIu64 "\n"
      "  -i, --ignore FILE   do not count pairs listed in FILE; may be\n"
      "                      repeated\n"
      "  -r, --reject FILE   discard rows containing pairs listed in FILE;\n"
      "                      may be repeated\n"
      "  --wfroot DIR        implies -r DIR/%s; discards\n"
      "                      rows with any word not in DIR/%s;\n"
      "                      also implies -r on the selected target's\n"
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
      program, DEFAULT_SEGMENT_OUTPUT_LIMIT, WORKFLOW_NO_PAIRS_PATH,
      WORKFLOW_DICT_PATH, WORKFLOW_TARGET_NO_PAIRS_PATH,
      WORKFLOW_DEFAULT_TARGET, WORKFLOW_RESULTS_NAME,
      WORKFLOW_YES_PAIRS_PATH);
}

static bool count_stream(
    std::istream* input, char const* name, DfsPairSet const& ignored,
    DfsPairSet const& rejected, DfsDictionary const& dictionary,
    SegmentCounts* counts) {
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
      if (is_rejected_segment(rejected, segments.back()) ||
          !all_words_in_dict(dictionary, segments.back()))
        reject_line = true;

      if (end == std::string::npos) break;
      start = end + 1;
    }

    if (reject_line) continue;
    for (std::string const& segment : segments) {
      if (ignored.find(segment) != ignored.end()) continue;
      SegmentCounts::iterator entry = counts->find(segment);
      if (entry == counts->end()) {
        entry = counts->emplace(
            segment, SegmentStats{0, segment_nonspace_length(segment)}).first;
      }
      if (entry->second.count == std::numeric_limits<uint64_t>::max()) {
        fprintf(stderr, "top-segments: segment count overflow\n");
        return false;
      }
      ++entry->second.count;
    }
  }

  if (input->bad()) {
    fprintf(stderr, "top-segments: can't read \"%s\"\n", name);
    return false;
  }
  return true;
}

static bool selected_segment(
    SegmentSelection selection, std::string const& segment) {
  if (selection == SEGMENT_SELECTION_PAIRS) return is_pair_segment(segment);
  if (selection == SEGMENT_SELECTION_SOLO) return is_solo_segment(segment);
  return true;
}

static bool split_counts(
    SegmentCounts const& counts, SegmentSelection selection,
    SegmentWeight weight, SegmentCounts* words) {
  for (SegmentCounts::const_iterator entry = counts.begin();
       entry != counts.end(); ++entry) {
    if (!selected_segment(selection, entry->first)) continue;
    std::vector<std::string> const split = split_segment_words(entry->first);
    uint64_t const increment = weight == SEGMENT_WEIGHT_UNIQUE
        ? 1 : entry->second.count;
    for (size_t i = 0; i < split.size(); ++i) {
      SegmentCounts::iterator word = words->find(split[i]);
      if (word == words->end()) {
        word = words->emplace(
            split[i], SegmentStats{0, split[i].size()}).first;
      }
      if (word->second.count >
          std::numeric_limits<uint64_t>::max() - increment) {
        fprintf(stderr, "top-segments: word count overflow\n");
        return false;
      }
      word->second.count += increment;
    }
  }
  return true;
}

static bool print_counts(
    SegmentCounts const& counts, SegmentOutputOptions const& output_options,
    bool show_pair_counts) {
  SegmentCounts split;
  if (output_options.projection == SEGMENT_PROJECTION_WORDS &&
      !split_counts(counts, output_options.selection, output_options.weight,
          &split))
    return false;
  SegmentCounts const& rows =
      output_options.projection == SEGMENT_PROJECTION_WORDS ? split : counts;

  std::vector<SegmentCounts::const_iterator> ordered;
  ordered.reserve(rows.size());
  uint64_t largest = 0;
  for (SegmentCounts::const_iterator entry = rows.begin();
       entry != rows.end(); ++entry) {
    if (output_options.projection == SEGMENT_PROJECTION_SEGMENTS &&
        !selected_segment(output_options.selection, entry->first))
      continue;
    ordered.push_back(entry);
    largest = std::max(largest, entry->second.count);
  }
  size_t const top = output_options.limit != 0 &&
          output_options.limit < ordered.size()
      ? size_t(output_options.limit) : ordered.size();
  std::partial_sort(ordered.begin(), ordered.begin() + top, ordered.end(),
    [&output_options](
        SegmentCounts::const_iterator a, SegmentCounts::const_iterator b) {
      if (output_options.by_length && a->second.length != b->second.length)
        return a->second.length > b->second.length;
      if (a->second.count != b->second.count)
        return a->second.count > b->second.count;
      return a->first < b->first;
    });

  int const width = snprintf(NULL, 0, "%" PRIu64, largest);
  for (size_t i = 0; i < top; ++i) {
    if (output_options.selection == SEGMENT_SELECTION_PAIRS &&
        output_options.projection == SEGMENT_PROJECTION_SEGMENTS) {
      std::string const pair = format_pair_segment(ordered[i]->first);
      if (show_pair_counts) {
        printf("%*" PRIu64 " %s\n",
            width, ordered[i]->second.count, pair.c_str());
      } else {
        printf("%s\n", pair.c_str());
      }
    } else {
      printf("%*" PRIu64 " %s\n", width, ordered[i]->second.count,
          ordered[i]->first.c_str());
    }
  }
  return !ferror(stdout);
}

int main(int argc, char* argv[]) {
  std::vector<char const*> paths;
  PairFilterOptions filter_options;
  bool parse_options = true;
  bool show_pair_counts = false;
  SegmentOutputOptions output_options;
  for (int i = 1; i < argc; ++i) {
    if (parse_options) {
      if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--counts") == 0) {
        show_pair_counts = true;
        continue;
      }
      if (strcmp(argv[i], "--pair-words") == 0) {
        if (!select_segment_output(
                SEGMENT_SELECTION_PAIRS, SEGMENT_PROJECTION_WORDS,
                "top-segments", &output_options)) {
          usage(stderr, argv[0]);
          return 2;
        }
        continue;
      }
      if (strcmp(argv[i], "--unique") == 0) {
        output_options.weight = SEGMENT_WEIGHT_UNIQUE;
        continue;
      }

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

  if (!check_pair_filter_options(filter_options, "top-segments")) {
    usage(stderr, argv[0]);
    return 2;
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
  bool const pair_words =
      output_options.selection == SEGMENT_SELECTION_PAIRS &&
      output_options.projection == SEGMENT_PROJECTION_WORDS;
  if (show_pair_counts && !pair_segments) {
    fputs("top-segments: --counts requires --pairs\n", stderr);
    usage(stderr, argv[0]);
    return 2;
  }
  if (output_options.weight == SEGMENT_WEIGHT_UNIQUE && !pair_words) {
    fputs("top-segments: --unique requires --pair-words\n", stderr);
    usage(stderr, argv[0]);
    return 2;
  }

  // Only a single named file names a single target; several files may sit in
  // several, and standard input sits in none.
  if (paths.size() == 1 && strcmp(paths[0], "-") != 0)
    filter_options.input_path = paths[0];

  DfsPairSet ignored;
  DfsPairSet rejected;
  DfsDictionary dictionary;
  if (!load_pair_filters(
          filter_options, "top-segments", &ignored, &rejected, &dictionary))
    return 1;

  SegmentCounts counts;
  if (paths.empty()) {
    if (!count_stream(&std::cin, "-", ignored, rejected, dictionary, &counts))
      return 1;
  } else {
    for (size_t i = 0; i < paths.size(); ++i) {
      if (strcmp(paths[i], "-") == 0) {
        if (!count_stream(
                &std::cin, "-", ignored, rejected, dictionary, &counts))
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
      if (!count_stream(
              &input, paths[i], ignored, rejected, dictionary, &counts))
        return 1;
    }
  }

  return print_counts(
      counts, output_options, show_pair_counts)
      ? 0 : 1;
}
