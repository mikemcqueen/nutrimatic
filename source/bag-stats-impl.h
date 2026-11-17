#ifndef NUTRIMATIC_BAG_STATS_IMPL_H
#define NUTRIMATIC_BAG_STATS_IMPL_H

#include <stdint.h>
#include <stdio.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>

#include "classified.h"

// The first line of bag-stats output.
extern char const BAG_STATS_MAGIC[];

// What a bag-stats run was given. Written as a text header, each line ending
// in '\n',
//   bag-stats 6
//   letters LETTERS      (what is left once used letters are removed, sorted)
//   min MIN_LENGTH       (of the words in each counted pair)
//   min-wb MIN_WB_LENGTH (of the DICT words loaded as word bags)
//   min-pb MIN_PB_LENGTH (of the PAIRS bags pb picks)
//   ss STEPS             (the search sequence, in canonical order)
//   pp PATH              (the prospective pairs; `pp` alone when none)
//   sentence N           (-s's sentence; `sentence` alone when none)
//   pairs PATH
//   dict PATH
//   columns BAG_STATS_COLUMNS
//   (an empty line)
// followed by one BagStatsRow per line.
struct BagStatsHeader {
  std::string letters;
  int min_word_length;
  int min_word_bag_length;
  int min_pair_bag_length;
  std::string search;
  std::string prospects;
  // CLASSIFIED_NO_SENTENCE when none.
  int sentence;
  std::string pairs;
  std::string dict;
};

// A word bag combination whose remainder has at least this many exact pairs
// counts in BagStatsRow::with_min_pairs.
constexpr uint64_t BAG_STATS_MIN_PAIRS = 2;

// The counts that depend only on what a row's pair picks leave.
struct BagStatsCounts {
  uint64_t word_bags;
  uint64_t with_pairs;
  uint64_t total_pairs;
  uint64_t max_pairs;
  uint64_t solo;
  uint64_t with_min_pairs;

  // Adds `part`'s counts, of another share of the same remainder.
  void add(BagStatsCounts const& part) {
    word_bags += part.word_bags;
    with_pairs += part.with_pairs;
    total_pairs += part.total_pairs;
    max_pairs = std::max(max_pairs, part.max_pairs);
    solo += part.solo;
    with_min_pairs += part.with_min_pairs;
  }
};

// One combination of pair bags' counts, written as BAG_STATS_COLUMNS lists
// them. See plans/bag-stats.md and plans/bag-stats-search-sequence.md for what
// each counts.
struct BagStatsRow : BagStatsCounts {
  std::string pair;
  uint64_t pairs;
};

// The row columns, comma-separated:
//   PAIR,PAIRS,WORDBAGS,WB_WITH_PAIRS,TOTAL_PAIRS,MAX_PAIRS,WB_SOLO,
//   WB_WITH_MIN_PAIRS
extern char const BAG_STATS_COLUMNS[];

// A count column of BAG_STATS_COLUMNS and the row member holding it.
struct BagStatsCount {
  char const* column;
  uint64_t BagStatsRow::*value;
};

// The count columns, in BAG_STATS_COLUMNS order.
extern BagStatsCount const BAG_STATS_COUNTS[7];

// The count column named `column`; null when there isn't one.
BagStatsCount const* find_bag_stats_count(std::string const& column);

// A sort key: `option`, its name, and the value it sorts a row by,
// `column`'s count, over `per`'s when `per` isn't NULL (0 when that count is
// 0); `value` and `per_value` the row members holding them.
struct BagStatsSort {
  char const* option;
  char const* column;
  char const* per;
  uint64_t BagStatsRow::*value;
  uint64_t BagStatsRow::*per_value;
};

// bag-stats' and bag-stats-cuda's sort options, each --OPTION.
extern BagStatsSort const BAG_STATS_SORT_FLAGS[6];

// bag-stats-sort's --sort keys.
extern BagStatsSort const BAG_STATS_SORTS[12];

// Prints the --help lines of BAG_STATS_SORT_FLAGS.
void bag_stats_help_sort_flags();

// Prints the --help lines of BAG_STATS_SORTS, under --sort.
void bag_stats_help_sorts();

// The BAG_STATS_SORTS key named `name`; null when there isn't one.
BagStatsSort const* find_bag_stats_sort(std::string_view name);

// The value `sort` sorts `row` by.
double bag_stats_sort_value(BagStatsSort const* sort, BagStatsRow const& row);

// The letters in `pair`: its pair picks' combined length.
size_t bag_stats_pair_length(std::string const& pair);

// The mid-rank percentile of a value among `size` values, `below` of them
// less than it and `through` of them at most it.
inline double bag_stats_percentile(uint64_t below, uint64_t through,
                                   uint64_t size) {
  return double(below + through) / 2 / double(size);
}

// `path` as the header names it: absolute, or `-` for standard input.
std::string bag_stats_header_path(std::filesystem::path const& path);

void write_bag_stats_header(FILE* out, BagStatsHeader const& header);

void write_bag_stats_row(FILE* out, BagStatsRow const& row);

// Appends the line write_bag_stats_row() writes to `out`.
void append_bag_stats_row(std::string* out, BagStatsRow const& row);

// The header at the start of `in`, the file at `path`, read through its
// empty line; none, diagnosed, when it isn't a BAG_STATS_MAGIC header.
std::optional<BagStatsHeader> read_bag_stats_header(FILE* in,
                                                    std::string const& path);

// The row in `line`, line `line_number` of `path`; none, diagnosed, when it
// isn't one.
std::optional<BagStatsRow> parse_bag_stats_row(std::string const& line,
                                               std::string const& path,
                                               size_t line_number);

#endif
