#include "bag-stats-impl.h"

#include <inttypes.h>

#include <algorithm>
#include <cstdlib>

#include "bad-bag-bitmap.h"

char const BAG_STATS_MAGIC[] = "bag-stats 2";

char const BAG_STATS_COLUMNS[] =
    "PAIR,PAIRBAG,PAIRS,WORDBAGS,WB_WITH_PAIRS,TOTAL_PAIRS,MAX_PAIRS,WB_SOLO,"
    "MIN_WB,MIN_WB_WITH_PAIRS";

BagStatsCount const BAG_STATS_COUNTS[8] = {
  {"PAIRS", &BagStatsRow::pairs},
  {"WORDBAGS", &BagStatsRow::word_bags},
  {"WB_WITH_PAIRS", &BagStatsRow::with_pairs},
  {"TOTAL_PAIRS", &BagStatsRow::total_pairs},
  {"MAX_PAIRS", &BagStatsRow::max_pairs},
  {"WB_SOLO", &BagStatsRow::solo},
  {"MIN_WB", &BagStatsRow::min_word_bags},
  {"MIN_WB_WITH_PAIRS", &BagStatsRow::min_with_pairs},
};

namespace {

bool parse_count(std::string const& text, uint64_t* out) {
  if (text.empty() || text.size() > 19 ||
      !std::all_of(text.begin(), text.end(),
                   [](char ch) { return ch >= '0' && ch <= '9'; }))
    return false;
  *out = strtoull(text.c_str(), NULL, 10);
  return true;
}

}  // namespace

BagStatsCount const* find_bag_stats_count(std::string const& column) {
  for (BagStatsCount const& count : BAG_STATS_COUNTS)
    if (column == count.column) return &count;
  return NULL;
}

void write_bag_stats_header(FILE* out, BagStatsHeader const& header) {
  fprintf(out,
          "%s\nletters %s\nmin %d\nmin-wb %d\npairs %s\ndict %s\n"
          "columns %s\n\n",
          BAG_STATS_MAGIC, header.letters.c_str(), header.min_word_length,
          header.min_word_bag_length, header.pairs.c_str(), header.dict.c_str(), BAG_STATS_COLUMNS);
}

void write_bag_stats_row(FILE* out, BagStatsRow const& row) {
  fprintf(out,
          "%s,%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
          ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
          row.pair.c_str(), row.pair_bag.c_str(), row.pairs, row.word_bags,
          row.with_pairs, row.total_pairs, row.max_pairs, row.solo,
          row.min_word_bags, row.min_with_pairs);
}

std::optional<BagStatsHeader> read_bag_stats_header(FILE* in,
                                                    std::string const& path) {
  std::string magic, min, min_wb, columns, blank;
  BagStatsHeader header;
  uint64_t min_length, min_wb_length;
  if (!read_line(in, &magic) || magic != BAG_STATS_MAGIC) {
    fprintf(stderr, "%s: not %s output\n", path.c_str(), BAG_STATS_MAGIC);
    return std::nullopt;
  }
  if (!read_field(in, "letters", &header.letters) ||
      !read_field(in, "min", &min) ||
      !read_field(in, "min-wb", &min_wb) ||
      !read_field(in, "pairs", &header.pairs) ||
      !read_field(in, "dict", &header.dict) ||
      !read_field(in, "columns", &columns) || columns != BAG_STATS_COLUMNS ||
      !read_line(in, &blank) || !blank.empty() ||
      !std::is_sorted(header.letters.begin(), header.letters.end()) ||
      !parse_count(min, &min_length) ||
      !parse_count(min_wb, &min_wb_length)) {
    fprintf(stderr, "%s: malformed bag-stats header\n", path.c_str());
    return std::nullopt;
  }
  header.min_word_length = int(min_length);
  header.min_word_bag_length = int(min_wb_length);
  return header;
}

std::optional<BagStatsRow> parse_bag_stats_row(std::string const& line,
                                               std::string const& path,
                                               size_t line_number) {
  std::string fields[10];
  size_t field = 0;
  size_t start = 0;
  for (size_t comma; field < 9 &&
       (comma = line.find(',', start)) != std::string::npos; ++field) {
    fields[field] = line.substr(start, comma - start);
    start = comma + 1;
  }
  fields[field] = line.substr(start);

  BagStatsRow row;
  row.pair = fields[0];
  row.pair_bag = fields[1];
  if (field != 9 || row.pair.empty() || row.pair_bag.empty() ||
      !std::is_sorted(row.pair_bag.begin(), row.pair_bag.end()) ||
      !parse_count(fields[2], &row.pairs) ||
      !parse_count(fields[3], &row.word_bags) ||
      !parse_count(fields[4], &row.with_pairs) ||
      !parse_count(fields[5], &row.total_pairs) ||
      !parse_count(fields[6], &row.max_pairs) ||
      !parse_count(fields[7], &row.solo) ||
      !parse_count(fields[8], &row.min_word_bags) ||
      !parse_count(fields[9], &row.min_with_pairs)) {
    fprintf(stderr, "%s line %zu: malformed bag-stats row\n", path.c_str(),
            line_number);
    return std::nullopt;
  }
  return row;
}
