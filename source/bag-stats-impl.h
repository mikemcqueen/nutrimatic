#ifndef NUTRIMATIC_BAG_STATS_IMPL_H
#define NUTRIMATIC_BAG_STATS_IMPL_H

#include <stdint.h>
#include <stdio.h>

#include <optional>
#include <string>

// The first line of bag-stats output.
extern char const BAG_STATS_MAGIC[];

// What a bag-stats run was given. Written as a text header, each line ending
// in '\n',
//   bag-stats 2
//   letters LETTERS      (what is left once used letters are removed, sorted)
//   min MIN_LENGTH       (of the words in each counted pair)
//   min-wb MIN_WB_LENGTH (of the word bags MIN_WB counts)
//   pairs PATH
//   dict PATH
//   columns BAG_STATS_COLUMNS
//   (an empty line)
// followed by one BagStatsRow per line.
struct BagStatsHeader {
  std::string letters;
  int min_word_length;
  int min_word_bag_length;
  std::string pairs;
  std::string dict;
};

// A MIN_WB word bag whose remainder has at least this many exact pairs counts
// in BagStatsRow::min_with_pairs.
constexpr uint64_t BAG_STATS_MIN_WB_PAIRS = 2;

// One pair bag's counts, written as BAG_STATS_COLUMNS lists them. See
// plans/bag-stats.md for what each counts.
struct BagStatsRow {
  std::string pair;
  std::string pair_bag;
  uint64_t pairs;
  uint64_t word_bags;
  uint64_t with_pairs;
  uint64_t total_pairs;
  uint64_t max_pairs;
  uint64_t solo;
  uint64_t min_word_bags;
  uint64_t min_with_pairs;
};

// The row columns, comma-separated:
//   PAIR,PAIRBAG,PAIRS,WORDBAGS,WB_WITH_PAIRS,TOTAL_PAIRS,MAX_PAIRS,WB_SOLO,
//   MIN_WB,MIN_WB_WITH_PAIRS
extern char const BAG_STATS_COLUMNS[];

// A count column of BAG_STATS_COLUMNS and the row member holding it.
struct BagStatsCount {
  char const* column;
  uint64_t BagStatsRow::*value;
};

// The count columns, in BAG_STATS_COLUMNS order.
extern BagStatsCount const BAG_STATS_COUNTS[8];

// The count column named `column`; null when there isn't one.
BagStatsCount const* find_bag_stats_count(std::string const& column);

void write_bag_stats_header(FILE* out, BagStatsHeader const& header);

void write_bag_stats_row(FILE* out, BagStatsRow const& row);

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
