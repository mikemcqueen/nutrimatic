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

#include "segment-output.h"

struct Stats {
  uint64_t total_rows = 0;
  uint64_t pair_segments = 0;
  uint64_t solo_word_segments = 0;
  uint64_t total_segments = 0;
  uint64_t total_words = 0;
  size_t unique_pair_segments = 0;
  size_t unique_solo_word_segments = 0;
  std::unordered_set<std::string> unique_segments;
  std::unordered_set<std::string> unique_words;
  std::vector<uint64_t> words_by_length;
  std::vector<size_t> unique_words_by_length;
};

static void usage(FILE* fp, char const* program) {
  fprintf(fp,
      "usage: %s [FILE]\n"
      "  calculate segment and word statistics for dfs-anagrams output\n"
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

static bool add_segment(std::string const& segment, Stats* stats) {
  if (!increment(&stats->total_segments, "segment count")) return false;
  bool const pair = is_pair_segment(segment);
  if (pair) {
    if (!increment(&stats->pair_segments, "pair segment count")) return false;
  } else if (!increment(
                 &stats->solo_word_segments, "solo-word segment count")) {
    return false;
  }
  if (stats->unique_segments.insert(segment).second) {
    if (pair)
      ++stats->unique_pair_segments;
    else
      ++stats->unique_solo_word_segments;
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

static bool read_stats(std::istream* input, char const* name, Stats* stats) {
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
      if (!add_segment(line.substr(start, length), stats)) return false;

      if (end == std::string::npos) break;
      start = end + 1;
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

static bool print_stats(Stats const& stats) {
  int const segment_label_width = 14;
  int const segment_count_width = decimal_width(stats.total_segments);
  print_count("total rows", segment_label_width,
      stats.total_rows, segment_count_width);
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

int main(int argc, char* argv[]) {
  char const* input_path = NULL;
  bool parse_options = true;
  for (int i = 1; i < argc; ++i) {
    if (parse_options && strcmp(argv[i], "--") == 0) {
      parse_options = false;
    } else if (parse_options &&
               (strcmp(argv[i], "-h") == 0 ||
                strcmp(argv[i], "--help") == 0)) {
      usage(stdout, argv[0]);
      return 0;
    } else if (parse_options && argv[i][0] == '-' && argv[i][1] != '\0') {
      fprintf(stderr, "segment-stats: unknown option \"%s\"\n", argv[i]);
      usage(stderr, argv[0]);
      return 2;
    } else if (input_path != NULL) {
      fputs("segment-stats: at most one FILE may be given\n", stderr);
      usage(stderr, argv[0]);
      return 2;
    } else {
      input_path = argv[i];
    }
  }

  Stats stats;
  if (input_path == NULL || strcmp(input_path, "-") == 0) {
    if (!read_stats(&std::cin, "-", &stats)) return 1;
  } else {
    errno = 0;
    std::ifstream input(input_path);
    if (!input.is_open()) {
      fprintf(stderr, "segment-stats: can't open \"%s\": %s\n",
          input_path, strerror(errno));
      return 1;
    }
    if (!read_stats(&input, input_path, &stats)) return 1;
  }

  return print_stats(stats) ? 0 : 1;
}
