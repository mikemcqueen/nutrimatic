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
#include <unordered_set>
#include <vector>

#include "pair-exclusions.h"
#include "segment-output.h"

struct SegmentStats {
  uint64_t count = 0;
  uint64_t line_count = 0;
  uint64_t last_seen_row = 0;
  size_t length = 0;
};

typedef std::unordered_map<std::string, SegmentStats> SegmentCounts;

struct FilterStats {
  uint64_t classified_no_lines = 0;
  uint64_t target_no_lines = 0;
  uint64_t explicit_reject_lines = 0;
  uint64_t dictionary_lines = 0;
  uint64_t classified_yes_instances = 0;
  uint64_t explicit_ignore_instances = 0;
  DfsPairSet classified_yes_pairs;
};

static void usage(FILE* fp, char const* program) {
  fprintf(fp,
      "usage: %s [--pairs [-c] | --solo-words | --all-words |\n"
      "          --pair-words [--unique]]\n"
      "          [-l | --elim] [-n N]\n"
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
      "  --elim              sort by guaranteed result-line elimination and\n"
      "                      print \"DECISION REQUIRE REJECT COUNT SEGMENT\"\n"
      "                      rows\n"
      "  -n N                print at most N rows; 0 prints all; defaults to "
      "%" PRIu64 "\n"
      "  -i, --ignore FILE   do not count pairs listed in FILE; may be\n"
      "                      repeated\n",
      program, DEFAULT_SEGMENT_OUTPUT_LIMIT);
  print_reject_option_help(fp, 22);
  fprintf(fp,
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
      WORKFLOW_NO_PAIRS_PATH, WORKFLOW_DICT_PATH, WORKFLOW_TARGET_NO_PAIRS_PATH,
      WORKFLOW_DEFAULT_TARGET, WORKFLOW_RESULTS_NAME,
      WORKFLOW_YES_PAIRS_PATH);
}

static bool any_segment_in(
    std::vector<std::string> const& segments, DfsPairSet const& pairs) {
  for (std::string const& segment : segments)
    if (pairs.find(segment) != pairs.end()) return true;
  return false;
}

static bool any_rejected_segment(
    std::vector<std::string> const& segments, DfsPairSet const& rejected) {
  for (std::string const& segment : segments) {
    if (is_rejected_segment(rejected, segment)) return true;
  }
  return false;
}

static bool any_segment_outside(
    std::vector<std::string> const& segments,
    DfsDictionary const& dictionary) {
  for (std::string const& segment : segments)
    if (!all_words_in_dict(dictionary, segment)) return true;
  return false;
}

static std::string canonical_pair(std::string const& segment) {
  size_t const space = segment.find(' ');
  if (space == std::string::npos) return segment;
  std::string const left = segment.substr(0, space);
  std::string const right = segment.substr(space + 1);
  return left < right ? segment : right + " " + left;
}

static bool selected_segment(
    SegmentSelection selection, std::string const& segment) {
  if (selection == SEGMENT_SELECTION_PAIRS) return is_pair_segment(segment);
  if (selection == SEGMENT_SELECTION_SOLO) return is_solo_segment(segment);
  return true;
}

static bool count_candidate(
    std::string const& candidate, uint64_t row, bool count_occurrence,
    SegmentCounts* counts) {
  SegmentCounts::iterator entry = counts->find(candidate);
  if (entry == counts->end()) {
    SegmentStats stats;
    stats.length = segment_nonspace_length(candidate);
    entry = counts->emplace(candidate, stats).first;
  }
  if (count_occurrence) {
    if (entry->second.count == std::numeric_limits<uint64_t>::max()) {
      fputs("top-segments: segment count overflow\n", stderr);
      return false;
    }
    ++entry->second.count;
  }
  if (entry->second.last_seen_row != row) {
    if (entry->second.line_count == std::numeric_limits<uint64_t>::max()) {
      fputs("top-segments: segment line count overflow\n", stderr);
      return false;
    }
    ++entry->second.line_count;
    entry->second.last_seen_row = row;
  }
  return true;
}

static bool count_stream(
    std::istream* input, char const* name, DfsPairSet const& ignored,
    DfsPairSet const& rejected, DfsDictionary const& dictionary,
    PairFilterSources const& filter_sources, FilterStats* filter_stats,
    SegmentOutputOptions const& output_options, bool elimination,
    uint64_t* surviving_rows,
    std::unordered_set<std::string>* unique_segments, SegmentCounts* counts) {
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

      if (end == std::string::npos) break;
      start = end + 1;
    }

    // Virtual filtering pipeline. The first matching rejection owns the row,
    // so each rejected line contributes to exactly one summary counter.
    // Workflow-wide NO is authoritative over target-local NO; explicit
    // rejects and then the dictionary follow. Only surviving rows reach the
    // per-segment ignore pipeline, where classified YES owns an overlap with
    // an explicit ignore. Keeping this order visible is important because it
    // defines diagnostic attribution even though set union would produce the
    // same selected output.
    if (any_segment_in(segments, filter_sources.classified_no)) {
      ++filter_stats->classified_no_lines;
      continue;
    }
    if (any_segment_in(segments, filter_sources.target_no)) {
      ++filter_stats->target_no_lines;
      continue;
    }
    if (any_rejected_segment(segments, rejected)) {
      ++filter_stats->explicit_reject_lines;
      continue;
    }
    if (any_segment_outside(segments, dictionary)) {
      ++filter_stats->dictionary_lines;
      continue;
    }
    uint64_t row = 0;
    if (elimination) {
      if (*surviving_rows == std::numeric_limits<uint64_t>::max()) {
        fputs("top-segments: result line count overflow\n", stderr);
        return false;
      }
      row = ++*surviving_rows;
    }
    for (std::string const& segment : segments) {
      if (filter_sources.classified_yes.find(segment) !=
          filter_sources.classified_yes.end()) {
        ++filter_stats->classified_yes_instances;
        filter_stats->classified_yes_pairs.insert(canonical_pair(segment));
        continue;
      }
      if (ignored.find(segment) != ignored.end()) {
        ++filter_stats->explicit_ignore_instances;
        continue;
      }
      if (!elimination) {
        SegmentCounts::iterator entry = counts->find(segment);
        if (entry == counts->end()) {
          SegmentStats stats;
          stats.length = segment_nonspace_length(segment);
          entry = counts->emplace(segment, stats).first;
        }
        if (entry->second.count == std::numeric_limits<uint64_t>::max()) {
          fputs("top-segments: segment count overflow\n", stderr);
          return false;
        }
        ++entry->second.count;
        continue;
      }
      if (!selected_segment(output_options.selection, segment)) continue;

      if (output_options.projection == SEGMENT_PROJECTION_SEGMENTS) {
        if (!count_candidate(segment, row, true, counts)) return false;
        continue;
      }

      bool const count_occurrence =
          output_options.weight == SEGMENT_WEIGHT_OCCURRENCES ||
          unique_segments->insert(segment).second;
      std::vector<std::string> const words = split_segment_words(segment);
      for (std::string const& word : words)
        if (!count_candidate(word, row, count_occurrence, counts))
          return false;
    }
  }

  if (input->bad()) {
    fprintf(stderr, "top-segments: can't read \"%s\"\n", name);
    return false;
  }
  return true;
}

static void print_filter_summary(
    FilterStats const& stats, PairFilterSources const& sources) {
  if (stats.classified_no_lines != 0 || stats.target_no_lines != 0 ||
      stats.explicit_reject_lines != 0 || stats.dictionary_lines != 0) {
    fputs("top-segments: Filtered ", stderr);
    bool first = true;
    if (stats.classified_no_lines != 0) {
      fprintf(stderr, "%" PRIu64 " lines from classified/no/no.pairs",
          stats.classified_no_lines);
      first = false;
    }
    if (stats.target_no_lines != 0) {
      if (!first) fputs(", ", stderr);
      fprintf(stderr, "%" PRIu64 " lines from %s/no.pairs",
          stats.target_no_lines, sources.target.c_str());
      first = false;
    }
    if (stats.explicit_reject_lines != 0) {
      if (!first) fputs(", ", stderr);
      fprintf(stderr, "%" PRIu64 " rejected explicitly",
          stats.explicit_reject_lines);
      first = false;
    }
    if (stats.dictionary_lines != 0) {
      if (!first) fputs(", ", stderr);
      fprintf(stderr, "%" PRIu64 " rejected by dictionary",
          stats.dictionary_lines);
    }
    fputc('\n', stderr);
  }

  if (stats.classified_yes_instances != 0 ||
      stats.explicit_ignore_instances != 0) {
    fputs("top-segments: Ignored ", stderr);
    bool first = true;
    if (stats.classified_yes_instances != 0) {
      fprintf(stderr,
          "%" PRIu64 " instances of %zu pairs from classified/yes/yes.pairs",
          stats.classified_yes_instances, stats.classified_yes_pairs.size());
      first = false;
    }
    if (stats.explicit_ignore_instances != 0) {
      if (!first) fputs(", ", stderr);
      fprintf(stderr, "%" PRIu64 " ignored explicitly",
          stats.explicit_ignore_instances);
    }
    fputc('\n', stderr);
  }
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
    for (std::string const& value : split) {
      SegmentCounts::iterator word = words->find(value);
      if (word == words->end()) {
        SegmentStats stats;
        stats.length = value.size();
        word = words->emplace(value, stats).first;
      }
      if (word->second.count >
          std::numeric_limits<uint64_t>::max() - increment) {
        fputs("top-segments: word count overflow\n", stderr);
        return false;
      }
      word->second.count += increment;
    }
  }
  return true;
}

static bool print_counts(
    SegmentCounts const& counts, SegmentOutputOptions const& output_options,
    bool show_pair_counts, bool elimination, uint64_t surviving_rows) {
  SegmentCounts split;
  if (!elimination &&
      output_options.projection == SEGMENT_PROJECTION_WORDS &&
      !split_counts(counts, output_options.selection, output_options.weight,
          &split))
    return false;
  SegmentCounts const& rows = !elimination &&
          output_options.projection == SEGMENT_PROJECTION_WORDS
      ? split : counts;

  std::vector<SegmentCounts::const_iterator> ordered;
  ordered.reserve(rows.size());
  uint64_t largest = 0;
  uint64_t largest_decision_elim = 0;
  uint64_t largest_require_elim = 0;
  uint64_t largest_reject_elim = 0;
  for (SegmentCounts::const_iterator entry = rows.begin();
       entry != rows.end(); ++entry) {
    if (output_options.projection == SEGMENT_PROJECTION_SEGMENTS &&
        !selected_segment(output_options.selection, entry->first))
      continue;
    ordered.push_back(entry);
    largest = std::max(largest, entry->second.count);
    uint64_t const require_elim = surviving_rows - entry->second.line_count;
    largest_decision_elim = std::max(largest_decision_elim,
        std::min(entry->second.line_count, require_elim));
    largest_require_elim = std::max(largest_require_elim, require_elim);
    largest_reject_elim =
        std::max(largest_reject_elim, entry->second.line_count);
  }
  size_t const top = output_options.limit != 0 &&
          output_options.limit < ordered.size()
      ? size_t(output_options.limit) : ordered.size();
  std::partial_sort(ordered.begin(), ordered.begin() + top, ordered.end(),
    [&output_options, elimination, surviving_rows](
        SegmentCounts::const_iterator a, SegmentCounts::const_iterator b) {
      if (elimination) {
        uint64_t const a_decision = std::min(a->second.line_count,
            surviving_rows - a->second.line_count);
        uint64_t const b_decision = std::min(b->second.line_count,
            surviving_rows - b->second.line_count);
        if (a_decision != b_decision) return a_decision > b_decision;
        if (a->second.line_count != b->second.line_count)
          return a->second.line_count > b->second.line_count;
      }
      if (output_options.by_length && a->second.length != b->second.length)
        return a->second.length > b->second.length;
      if (a->second.count != b->second.count)
        return a->second.count > b->second.count;
      return a->first < b->first;
    });

  int width = snprintf(NULL, 0, "%" PRIu64, largest);
  int decision_width =
      snprintf(NULL, 0, "%" PRIu64, largest_decision_elim);
  int require_width =
      snprintf(NULL, 0, "%" PRIu64, largest_require_elim);
  int reject_width =
      snprintf(NULL, 0, "%" PRIu64, largest_reject_elim);
  if (elimination) {
    width = std::max(width, int(strlen("COUNT")));
    decision_width = std::max(decision_width, int(strlen("DECISION")));
    require_width = std::max(require_width, int(strlen("REQUIRE")));
    reject_width = std::max(reject_width, int(strlen("REJECT")));
    printf("%*s %*s %*s %*s %s\n",
        decision_width, "DECISION", require_width, "REQUIRE",
        reject_width, "REJECT", width, "COUNT", "SEGMENT");
  }
  for (size_t i = 0; i < top; ++i) {
    std::string const displayed =
        output_options.selection == SEGMENT_SELECTION_PAIRS &&
            output_options.projection == SEGMENT_PROJECTION_SEGMENTS
        ? format_pair_segment(ordered[i]->first) : ordered[i]->first;
    if (elimination) {
      uint64_t const reject_elim = ordered[i]->second.line_count;
      uint64_t const require_elim = surviving_rows - reject_elim;
      uint64_t const decision_elim = std::min(reject_elim, require_elim);
      printf("%*" PRIu64 " %*" PRIu64 " %*" PRIu64 " %*" PRIu64
             " %s\n",
          decision_width, decision_elim, require_width, require_elim,
          reject_width, reject_elim, width, ordered[i]->second.count,
          displayed.c_str());
      continue;
    }
    if (output_options.selection == SEGMENT_SELECTION_PAIRS &&
        output_options.projection == SEGMENT_PROJECTION_SEGMENTS) {
      if (show_pair_counts) {
        printf("%*" PRIu64 " %s\n",
            width, ordered[i]->second.count, displayed.c_str());
      } else {
        printf("%s\n", displayed.c_str());
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
  bool elimination = false;
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
      if (strcmp(argv[i], "--elim") == 0) {
        elimination = true;
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
  if (elimination && output_options.by_length) {
    fputs("top-segments: --elim and --by-length are mutually exclusive\n",
        stderr);
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
  PairFilterSources filter_sources;
  if (!load_pair_filters(
          filter_options, "top-segments", &ignored, &rejected, &dictionary,
          &filter_sources))
    return 1;

  SegmentCounts counts;
  uint64_t surviving_rows = 0;
  std::unordered_set<std::string> unique_segments;
  FilterStats filter_stats;
  if (paths.empty()) {
    if (!count_stream(&std::cin, "-", ignored, rejected, dictionary,
            filter_sources, &filter_stats, output_options, elimination,
            &surviving_rows, &unique_segments, &counts))
      return 1;
  } else {
    for (size_t i = 0; i < paths.size(); ++i) {
      if (strcmp(paths[i], "-") == 0) {
        if (!count_stream(
                &std::cin, "-", ignored, rejected, dictionary,
                filter_sources, &filter_stats, output_options, elimination,
                &surviving_rows, &unique_segments, &counts))
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
              &input, paths[i], ignored, rejected, dictionary,
              filter_sources, &filter_stats, output_options, elimination,
              &surviving_rows, &unique_segments, &counts))
        return 1;
    }
  }

  print_filter_summary(filter_stats, filter_sources);
  return print_counts(
      counts, output_options, show_pair_counts, elimination, surviving_rows)
      ? 0 : 1;
}
