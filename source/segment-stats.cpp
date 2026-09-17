#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

#include "dfs-cli-args.h"
#include "optparse.h"
#include "segment-output.h"

struct Args {
  char const* input_path = NULL;
  std::vector<std::string> allow_paths;
};

struct RowCounts {
  size_t pair_segments = 0;
  size_t allowed_pair_segments = 0;
};

struct Stats {
  uint64_t total_rows = 0;
  uint64_t all_allowed_rows = 0;
  uint64_t at_least_half_allowed_rows = 0;
  uint64_t any_allowed_rows = 0;
  uint64_t pair_segments = 0;
  uint64_t solo_word_segments = 0;
  uint64_t total_segments = 0;
  uint64_t total_words = 0;
  size_t unique_pair_segments = 0;
  size_t unique_solo_word_segments = 0;
  size_t allowed_unique_pair_segments = 0;
  std::unordered_set<std::string> unique_segments;
  std::unordered_set<std::string> unique_words;
  std::vector<uint64_t> words_by_length;
  std::vector<size_t> unique_words_by_length;
};

static void usage(char const* program) {
  fprintf(stdout,
      "usage: %s [-a FILE]... [FILE]\n"
      "  calculate segment and word statistics for dfs-anagrams output\n"
      "  -a, --allow-pairs FILE  also report how many unique pair segments\n"
      "                          are listed in FILE; pairs match in either\n"
      "                          word order; may be repeated\n"
      "  with no FILE, or when FILE is -, read standard input\n",
      program);
}

static bool increment(uint64_t* count, char const* description) {
  if (*count == std::numeric_limits<uint64_t>::max()) {
    fprintf(stderr, "segment-stats: %s overflow\n", description);
    return false;
  }
  ++*count;
  return true;
}

static bool add_segment(
    std::string const& segment, DfsPairSet const* allowed, Stats* stats,
    RowCounts* row) {
  if (!increment(&stats->total_segments, "segment count")) return false;
  bool const pair = is_pair_segment(segment);
  bool const allowed_pair =
      pair && allowed != NULL && allowed->find(segment) != allowed->end();
  if (pair) {
    if (!increment(&stats->pair_segments, "pair segment count")) return false;
    ++row->pair_segments;
    if (allowed_pair) ++row->allowed_pair_segments;
  } else if (!increment(
                 &stats->solo_word_segments, "solo-word segment count")) {
    return false;
  }
  if (stats->unique_segments.insert(segment).second) {
    if (pair) {
      ++stats->unique_pair_segments;
      if (allowed_pair) ++stats->allowed_unique_pair_segments;
    } else {
      ++stats->unique_solo_word_segments;
    }
  }

  std::vector<std::string> const words = split_segment_words(segment);
  for (std::string const& word : words) {
    if (!increment(&stats->total_words, "word count")) return false;
    if (stats->words_by_length.size() <= word.size())
      stats->words_by_length.resize(word.size() + 1);
    if (!increment(&stats->words_by_length[word.size()],
                   "word-length count"))
      return false;
    if (stats->unique_words.insert(word).second) {
      if (stats->unique_words_by_length.size() <= word.size())
        stats->unique_words_by_length.resize(word.size() + 1);
      ++stats->unique_words_by_length[word.size()];
    }
  }
  return true;
}

static bool read_stats(
    std::istream* input, char const* name, DfsPairSet const* allowed,
    Stats* stats) {
  std::string line;
  uint64_t line_number = 0;
  while (std::getline(*input, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    if (!increment(&stats->total_rows, "row count")) return false;

    char* score_end;
    (void) strtod(line.c_str(), &score_end);
    if (score_end == line.c_str() || *score_end != ' ' ||
        score_end[1] == '\0') {
      fprintf(stderr,
          "segment-stats: %s:%" PRIu64
          ": expected \"score segment[,segment ...]\"\n",
          name, line_number);
      return false;
    }

    size_t start = size_t(score_end - line.c_str()) + 1;
    RowCounts row;
    while (true) {
      size_t const end = line.find(',', start);
      size_t const length =
          end == std::string::npos ? line.size() - start : end - start;
      if (length == 0) {
        fprintf(stderr,
            "segment-stats: %s:%" PRIu64 ": empty segment\n",
            name, line_number);
        return false;
      }
      if (!add_segment(line.substr(start, length), allowed, stats, &row))
        return false;

      if (end == std::string::npos) break;
      start = end + 1;
    }

    if (row.pair_segments > 0) {
      if (row.allowed_pair_segments == row.pair_segments &&
          !increment(&stats->all_allowed_rows, "all-allowed row count"))
        return false;
      if (row.allowed_pair_segments * 2 >= row.pair_segments &&
          !increment(&stats->at_least_half_allowed_rows,
                     "half-allowed row count"))
        return false;
      if (row.allowed_pair_segments > 0 &&
          !increment(&stats->any_allowed_rows, "any-allowed row count"))
        return false;
    }
  }

  if (input->bad()) {
    fprintf(stderr, "segment-stats: can't read \"%s\"\n", name);
    return false;
  }
  return true;
}

static double percentage(double count, double total) {
  return total == 0 ? 0.0 : 100.0 * double(count) / double(total);
}

static int decimal_width(uintmax_t value) {
  int width = 1;
  while (value >= 10) {
    value /= 10;
    ++width;
  }
  return width;
}

static void print_count(
    std::string const& label, int label_width, uintmax_t count,
    int count_width) {
  printf("%*s: %*" PRIuMAX "\n",
      label_width, label.c_str(), count_width, count);
}

static void print_percentage_count(
    std::string const& label, int label_width, uintmax_t count,
    int count_width, double share, char const* basis) {
  printf("%*s: %*" PRIuMAX " (%.2f%% of %s)\n",
      label_width, label.c_str(), count_width, count, share, basis);
}

static bool print_stats(Stats const& stats, bool show_allowed) {
  int const segment_label_width = 14;
  int const segment_count_width = decimal_width(stats.total_segments);
  print_count("total rows", segment_label_width,
      stats.total_rows, segment_count_width);
  if (show_allowed) {
    int const row_label_width = int(strlen(">=50% allowed pairs"));
    int const row_count_width = decimal_width(stats.total_rows);
    print_percentage_count("all allowed pairs", row_label_width,
        stats.all_allowed_rows, row_count_width,
        percentage(stats.all_allowed_rows, stats.total_rows), "total");
    print_percentage_count(">=50% allowed pairs", row_label_width,
        stats.at_least_half_allowed_rows, row_count_width,
        percentage(stats.at_least_half_allowed_rows, stats.total_rows),
        "total");
    print_percentage_count("any allowed pairs", row_label_width,
        stats.any_allowed_rows, row_count_width,
        percentage(stats.any_allowed_rows, stats.total_rows), "total");
    putchar('\n');
  }
  print_count("total segments", segment_label_width,
      stats.total_segments, segment_count_width);
  print_count("unique", segment_label_width,
      stats.unique_segments.size(), segment_count_width);
  print_percentage_count("pair segments", segment_label_width,
      stats.pair_segments, segment_count_width,
      percentage(stats.pair_segments, stats.total_segments), "total");
  print_percentage_count("unique", segment_label_width,
      stats.unique_pair_segments, segment_count_width,
      percentage(stats.unique_pair_segments, stats.unique_segments.size()),
      "unique");
  if (show_allowed)
    print_percentage_count("allowed", segment_label_width,
        stats.allowed_unique_pair_segments, segment_count_width,
        percentage(
            stats.allowed_unique_pair_segments, stats.unique_pair_segments),
        "unique pair segments");
  print_percentage_count("solo segments", segment_label_width,
      stats.solo_word_segments, segment_count_width,
      percentage(stats.solo_word_segments, stats.total_segments), "total");
  print_percentage_count("unique", segment_label_width,
      stats.unique_solo_word_segments, segment_count_width,
      percentage(
          stats.unique_solo_word_segments, stats.unique_segments.size()),
      "unique");

  size_t const maximum_length = stats.words_by_length.empty()
      ? 0 : stats.words_by_length.size() - 1;
  int const word_label_width = std::max(
      size_t(14),
      std::to_string(maximum_length).size() + strlen("-letter words"));
  int const word_count_width = decimal_width(stats.total_words);
  putchar('\n');
  print_count("total words", word_label_width,
      stats.total_words, word_count_width);
  print_count("unique", word_label_width,
      stats.unique_words.size(), word_count_width);
  for (size_t length = 1; length < stats.words_by_length.size(); ++length) {
    if (stats.words_by_length[length] == 0) continue;
    size_t const unique = length < stats.unique_words_by_length.size()
        ? stats.unique_words_by_length[length] : 0;
    std::string const label = std::to_string(length) + "-letter words";
    print_percentage_count(label, word_label_width,
        stats.words_by_length[length], word_count_width,
        percentage(stats.words_by_length[length], stats.total_words),
        "total");
    print_percentage_count("unique", word_label_width,
        unique, word_count_width,
        percentage(unique, stats.unique_words.size()), "unique");
  }
  return fflush(stdout) == 0;
}

static bool parse_args(char* argv[], Args* out, bool* requested_help) {
  enum {
    OPT_HELP = 256,
  };
  static struct optparse_long const long_options[] = {
    {"allow-pairs", 'a', OPTPARSE_REQUIRED},
    {"help", OPT_HELP, OPTPARSE_NONE},
    {NULL, 'h', OPTPARSE_NONE},
    {NULL, 0, OPTPARSE_NONE},
  };

  *requested_help = false;

  struct optparse options;
  optparse_init(&options, argv);
  int option;
  while ((option = optparse_long(&options, long_options, NULL)) != -1) {
    switch (option) {
      case 'a':
        if (options.optarg[0] == '\0') {
          fputs("segment-stats: -a/--allow-pairs requires a file\n", stderr);
          return false;
        }
        out->allow_paths.push_back(options.optarg);
        break;
      case 'h':
      case OPT_HELP:
        *requested_help = true;
        return true;
      default:
        fprintf(stderr, "segment-stats: %s\n", options.errmsg);
        return false;
    }
  }

  out->input_path = optparse_arg(&options);
  if (out->input_path != NULL && optparse_arg(&options) != NULL) {
    fputs("segment-stats: at most one FILE may be given\n", stderr);
    return false;
  }
  return true;
}

int main(int argc, char* argv[]) {
  (void) argc;
  Args args;
  bool requested_help;
  if (!parse_args(argv, &args, &requested_help)) {
    usage(argv[0]);
    return 2;
  }
  if (requested_help) {
    usage(argv[0]);
    return 0;
  }

  DfsPairSet allowed;
  for (size_t i = 0; i < args.allow_paths.size(); ++i) {
    if (!load_pair_file(args.allow_paths[i].c_str(), "allow list", &allowed,
                        /*quiet=*/true, /*reject_hyphens=*/true,
                        /*allow_single_words=*/true))
      return 1;
  }
  DfsPairSet const* const allowed_pairs =
      args.allow_paths.empty() ? NULL : &allowed;

  Stats stats;
  if (args.input_path == NULL || strcmp(args.input_path, "-") == 0) {
    if (!read_stats(&std::cin, "-", allowed_pairs, &stats)) return 1;
  } else {
    errno = 0;
    std::ifstream input(args.input_path);
    if (!input.is_open()) {
      fprintf(stderr, "segment-stats: can't open \"%s\": %s\n",
          args.input_path, strerror(errno));
      return 1;
    }
    if (!read_stats(&input, args.input_path, allowed_pairs, &stats)) return 1;
  }

  return print_stats(stats, allowed_pairs != NULL) ? 0 : 1;
}
