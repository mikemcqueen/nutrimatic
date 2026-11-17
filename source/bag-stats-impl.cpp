#include "bag-stats-impl.h"

#include <inttypes.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>

#include "bad-bag-bitmap.h"
#include "dfs-cli-help.h"

char const BAG_STATS_MAGIC[] = "bag-stats 6";

char const BAG_STATS_COLUMNS[] =
    "PAIR,PAIRS,WORDBAGS,WB_WITH_PAIRS,TOTAL_PAIRS,MAX_PAIRS,WB_SOLO,"
    "WB_WITH_MIN_PAIRS";

BagStatsCount const BAG_STATS_COUNTS[7] = {
  {"PAIRS", &BagStatsRow::pairs},
  {"WORDBAGS", &BagStatsRow::word_bags},
  {"WB_WITH_PAIRS", &BagStatsRow::with_pairs},
  {"TOTAL_PAIRS", &BagStatsRow::total_pairs},
  {"MAX_PAIRS", &BagStatsRow::max_pairs},
  {"WB_SOLO", &BagStatsRow::solo},
  {"WB_WITH_MIN_PAIRS", &BagStatsRow::with_min_pairs},
};

#define BAG_STATS_SORT(option, column, member) \
  {option, column, NULL, &BagStatsRow::member, NULL}
#define BAG_STATS_SORT_PER(option, column, member, per, per_member) \
  {option, column, per, &BagStatsRow::member, &BagStatsRow::per_member}

BagStatsSort const BAG_STATS_SORT_FLAGS[6] = {
  BAG_STATS_SORT("wb", "WORDBAGS", word_bags),
  BAG_STATS_SORT("wbwp", "WB_WITH_PAIRS", with_pairs),
  BAG_STATS_SORT("tp", "TOTAL_PAIRS", total_pairs),
  BAG_STATS_SORT("mp", "MAX_PAIRS", max_pairs),
  BAG_STATS_SORT("wbwp-min", "WB_WITH_MIN_PAIRS", with_min_pairs),
  BAG_STATS_SORT_PER("wbfrac-min", "WB_WITH_MIN_PAIRS", with_min_pairs,
                     "WORDBAGS", word_bags),
};

BagStatsSort const BAG_STATS_SORTS[12] = {
  BAG_STATS_SORT("wb", "WORDBAGS", word_bags),
  BAG_STATS_SORT("wbwp", "WB_WITH_PAIRS", with_pairs),
  BAG_STATS_SORT("tp", "TOTAL_PAIRS", total_pairs),
  BAG_STATS_SORT("mp", "MAX_PAIRS", max_pairs),
  BAG_STATS_SORT("wbwp-min", "WB_WITH_MIN_PAIRS", with_min_pairs),
  BAG_STATS_SORT_PER("wbwp-per-wb", "WB_WITH_PAIRS", with_pairs,
                     "WORDBAGS", word_bags),
  BAG_STATS_SORT_PER("wbwp-min-per-wb", "WB_WITH_MIN_PAIRS", with_min_pairs,
                     "WORDBAGS", word_bags),
  BAG_STATS_SORT_PER("wbwp-min-per-wbwp", "WB_WITH_MIN_PAIRS",
                     with_min_pairs, "WB_WITH_PAIRS", with_pairs),
  BAG_STATS_SORT_PER("wbwp-per-wbwp-min", "WB_WITH_PAIRS", with_pairs,
                     "WB_WITH_MIN_PAIRS", with_min_pairs),
  BAG_STATS_SORT_PER("tp-per-wb", "TOTAL_PAIRS", total_pairs,
                     "WORDBAGS", word_bags),
  BAG_STATS_SORT_PER("tp-per-wbwp", "TOTAL_PAIRS", total_pairs,
                     "WB_WITH_PAIRS", with_pairs),
  BAG_STATS_SORT_PER("tp-per-wbwp-min", "TOTAL_PAIRS", total_pairs,
                     "WB_WITH_MIN_PAIRS", with_min_pairs),
};

#undef BAG_STATS_SORT
#undef BAG_STATS_SORT_PER

namespace {

bool parse_count(std::string const& text, uint64_t* out) {
  if (text.empty() || text.size() > 19 ||
      !std::all_of(text.begin(), text.end(),
                   [](char ch) { return ch >= '0' && ch <= '9'; }))
    return false;
  *out = strtoull(text.c_str(), NULL, 10);
  return true;
}

// As read_field(), but a line holding only `name` is an empty value.
bool read_optional_field(FILE* in, std::string_view name,
                         std::string* value) {
  std::string line;
  if (!read_line(in, &line)) return false;
  if (line == name) {
    value->clear();
    return true;
  }
  size_t const length = name.size();
  if (line.compare(0, length, name) != 0 || line.size() <= length ||
      line[length] != ' ')
    return false;
  *value = line.substr(length + 1);
  return true;
}

}  // namespace

BagStatsCount const* find_bag_stats_count(std::string const& column) {
  for (BagStatsCount const& count : BAG_STATS_COUNTS)
    if (column == count.column) return &count;
  return NULL;
}

void bag_stats_help_sort_flags() {
  for (BagStatsSort const& flag : BAG_STATS_SORT_FLAGS) {
    if (flag.per != NULL) continue;
    std::string const option = std::string("--") + flag.option;
    dfs_help_option(option.c_str(), "sort by %s", flag.column);
  }
  dfs_help_option("--wbfrac-min",
                  "sort by WB_WITH_MIN_PAIRS / WORDBAGS, the fraction of "
                  "word bag combinations whose remainder has enough exact "
                  "pairs (0 when WORDBAGS is 0)");
}

void bag_stats_help_sorts() {
  for (BagStatsSort const& sort : BAG_STATS_SORTS) {
    if (sort.per == NULL)
      dfs_help_option(sort.option, "%s", sort.column);
    else
      dfs_help_option(sort.option, "%s / %s, 0 when %s is 0", sort.column,
                      sort.per, sort.per);
  }
}

BagStatsSort const* find_bag_stats_sort(std::string_view name) {
  for (BagStatsSort const& sort : BAG_STATS_SORTS)
    if (name == sort.option) return &sort;
  return NULL;
}

double bag_stats_sort_value(BagStatsSort const* sort, BagStatsRow const& row) {
  double const value = double(row.*sort->value);
  if (sort->per == NULL) return value;
  uint64_t const per = row.*sort->per_value;
  return per == 0 ? 0 : value / double(per);
}

size_t bag_stats_pair_length(std::string const& pair) {
  return size_t(std::count_if(pair.begin(), pair.end(), [](char ch) {
    return std::isalpha(static_cast<unsigned char>(ch));
  }));
}

std::string bag_stats_header_path(std::filesystem::path const& path) {
  if (path == "-") return "-";
  return std::filesystem::absolute(path).lexically_normal().string();
}

void write_bag_stats_header(FILE* out, BagStatsHeader const& header) {
  fprintf(out,
          "%s\nletters %s\nmin %d\nmin-wb %d\nmin-pb %d\nss %s\npp%s%s\n"
          "sentence",
          BAG_STATS_MAGIC, header.letters.c_str(), header.min_word_length,
          header.min_word_bag_length, header.min_pair_bag_length,
          header.search.c_str(),
          header.prospects.empty() ? "" : " ", header.prospects.c_str());
  if (header.sentence != CLASSIFIED_NO_SENTENCE)
    fprintf(out, " %d", header.sentence);
  fprintf(out, "\npairs %s\ndict %s\ncolumns %s\n\n", header.pairs.c_str(),
          header.dict.c_str(), BAG_STATS_COLUMNS);
}

void write_bag_stats_row(FILE* out, BagStatsRow const& row) {
  std::string line;
  append_bag_stats_row(&line, row);
  fwrite(line.data(), 1, line.size(), out);
}

void append_bag_stats_row(std::string* out, BagStatsRow const& row) {
  char counts[7 * 21 + 2];
  snprintf(counts, sizeof counts,
           ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
           ",%" PRIu64 ",%" PRIu64 "\n",
           row.pairs, row.word_bags, row.with_pairs, row.total_pairs,
           row.max_pairs, row.solo, row.with_min_pairs);
  *out += row.pair;
  *out += counts;
}

std::optional<BagStatsHeader> read_bag_stats_header(FILE* in,
                                                    std::string const& path) {
  std::string magic, min, min_wb, min_pb, sentence, columns, blank;
  BagStatsHeader header;
  uint64_t min_length, min_wb_length, min_pb_length, sentence_number = 0;
  if (!read_line(in, &magic) || magic != BAG_STATS_MAGIC) {
    fprintf(stderr, "%s: not %s output\n", path.c_str(), BAG_STATS_MAGIC);
    return std::nullopt;
  }
  if (!read_field(in, "letters", &header.letters) ||
      !read_field(in, "min", &min) ||
      !read_field(in, "min-wb", &min_wb) ||
      !read_field(in, "min-pb", &min_pb) ||
      !read_field(in, "ss", &header.search) ||
      !read_optional_field(in, "pp", &header.prospects) ||
      !read_optional_field(in, "sentence", &sentence) ||
      !read_field(in, "pairs", &header.pairs) ||
      !read_field(in, "dict", &header.dict) ||
      !read_field(in, "columns", &columns) || columns != BAG_STATS_COLUMNS ||
      !read_line(in, &blank) || !blank.empty() ||
      !std::is_sorted(header.letters.begin(), header.letters.end()) ||
      !parse_count(min, &min_length) ||
      !parse_count(min_wb, &min_wb_length) ||
      !parse_count(min_pb, &min_pb_length) ||
      (!sentence.empty() && (!parse_count(sentence, &sentence_number) ||
                             sentence_number > uint64_t(INT32_MAX)))) {
    fprintf(stderr, "%s: malformed bag-stats header\n", path.c_str());
    return std::nullopt;
  }
  header.min_word_length = int(min_length);
  header.min_word_bag_length = int(min_wb_length);
  header.min_pair_bag_length = int(min_pb_length);
  header.sentence =
      sentence.empty() ? CLASSIFIED_NO_SENTENCE : int(sentence_number);
  return header;
}

std::optional<BagStatsRow> parse_bag_stats_row(std::string const& line,
                                               std::string const& path,
                                               size_t line_number) {
  std::string fields[8];
  size_t field = 0;
  size_t start = 0;
  for (size_t comma; field < 7 &&
       (comma = line.find(',', start)) != std::string::npos; ++field) {
    fields[field] = line.substr(start, comma - start);
    start = comma + 1;
  }
  fields[field] = line.substr(start);

  BagStatsRow row;
  row.pair = fields[0];
  if (field != 7 || row.pair.empty() ||
      !parse_count(fields[1], &row.pairs) ||
      !parse_count(fields[2], &row.word_bags) ||
      !parse_count(fields[3], &row.with_pairs) ||
      !parse_count(fields[4], &row.total_pairs) ||
      !parse_count(fields[5], &row.max_pairs) ||
      !parse_count(fields[6], &row.solo) ||
      !parse_count(fields[7], &row.with_min_pairs)) {
    fprintf(stderr, "%s line %zu: malformed bag-stats row\n", path.c_str(),
            line_number);
    return std::nullopt;
  }
  return row;
}
