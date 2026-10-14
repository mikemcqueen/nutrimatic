#include "bag-stats-impl.h"

#include <inttypes.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "bad-bag-bitmap.h"
#include "dfs-cli-help.h"

char const BAG_STATS_MAGIC[] = "bag-stats 4";

char const BAG_STATS_COLUMNS[] =
    "PAIR,PAIRS,WORDBAGS,WB_WITH_PAIRS,TOTAL_PAIRS,MAX_PAIRS,WB_SOLO,MIN_WB,"
    "MIN_WB_WITH_PAIRS";

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

BagStatsSortFlag const BAG_STATS_SORT_FLAGS[7] = {
  {"wb", "WORDBAGS"},
  {"wbwp", "WB_WITH_PAIRS"},
  {"tp", "TOTAL_PAIRS"},
  {"mp", "MAX_PAIRS"},
  {"min-wb", "MIN_WB"},
  {"min-wbwp", "MIN_WB_WITH_PAIRS"},
  {"min-wb-frac", NULL},
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

// As read_field(), but a line holding only `name` is an empty value.
bool read_optional_field(FILE* in, char const* name, std::string* value) {
  std::string line;
  if (!read_line(in, &line)) return false;
  if (line == name) {
    value->clear();
    return true;
  }
  size_t const length = strlen(name);
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
  for (BagStatsSortFlag const& flag : BAG_STATS_SORT_FLAGS) {
    if (flag.column == NULL) continue;
    std::string const option = std::string("--") + flag.option;
    dfs_help_option(option.c_str(), "sort by %s", flag.column);
  }
  dfs_help_option("--min-wb-frac",
                  "sort by MIN_WB_WITH_PAIRS / MIN_WB, the fraction of "
                  "MIN_WB word bags whose remainder has enough exact pairs "
                  "(0 when MIN_WB is 0)");
}

BagStatsCount const* bag_stats_sort_key(BagStatsSortFlag const& flag) {
  return flag.column == NULL ? NULL : find_bag_stats_count(flag.column);
}

double bag_stats_min_wb_fraction(BagStatsRow const& row) {
  return row.min_word_bags == 0
      ? 0 : double(row.min_with_pairs) / double(row.min_word_bags);
}

double bag_stats_sort_value(BagStatsCount const* key, BagStatsRow const& row) {
  return key == NULL ? bag_stats_min_wb_fraction(row)
                     : double(row.*key->value);
}

size_t bag_stats_pair_length(std::string const& pair) {
  return size_t(std::count_if(pair.begin(), pair.end(), [](char ch) {
    return std::isalpha(static_cast<unsigned char>(ch));
  }));
}

std::string bag_stats_header_path(char const* path) {
  if (path == NULL) return "";
  if (strcmp(path, "-") == 0) return path;
  return std::filesystem::absolute(path).lexically_normal().string();
}

void write_bag_stats_header(FILE* out, BagStatsHeader const& header) {
  fprintf(out,
          "%s\nletters %s\nmin %d\nmin-wb %d\nss %s\npp%s%s\nsentence",
          BAG_STATS_MAGIC, header.letters.c_str(), header.min_word_length,
          header.min_word_bag_length, header.search.c_str(),
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
  char counts[8 * 21 + 2];
  snprintf(counts, sizeof counts,
           ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
           ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
           row.pairs, row.word_bags, row.with_pairs, row.total_pairs,
           row.max_pairs, row.solo, row.min_word_bags, row.min_with_pairs);
  *out += row.pair;
  *out += counts;
}

std::optional<BagStatsHeader> read_bag_stats_header(FILE* in,
                                                    std::string const& path) {
  std::string magic, min, min_wb, sentence, columns, blank;
  BagStatsHeader header;
  uint64_t min_length, min_wb_length, sentence_number = 0;
  if (!read_line(in, &magic) || magic != BAG_STATS_MAGIC) {
    fprintf(stderr, "%s: not %s output\n", path.c_str(), BAG_STATS_MAGIC);
    return std::nullopt;
  }
  if (!read_field(in, "letters", &header.letters) ||
      !read_field(in, "min", &min) ||
      !read_field(in, "min-wb", &min_wb) ||
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
      (!sentence.empty() && (!parse_count(sentence, &sentence_number) ||
                             sentence_number > uint64_t(INT32_MAX)))) {
    fprintf(stderr, "%s: malformed bag-stats header\n", path.c_str());
    return std::nullopt;
  }
  header.min_word_length = int(min_length);
  header.min_word_bag_length = int(min_wb_length);
  header.sentence =
      sentence.empty() ? CLASSIFIED_NO_SENTENCE : int(sentence_number);
  return header;
}

std::optional<BagStatsRow> parse_bag_stats_row(std::string const& line,
                                               std::string const& path,
                                               size_t line_number) {
  std::string fields[9];
  size_t field = 0;
  size_t start = 0;
  for (size_t comma; field < 8 &&
       (comma = line.find(',', start)) != std::string::npos; ++field) {
    fields[field] = line.substr(start, comma - start);
    start = comma + 1;
  }
  fields[field] = line.substr(start);

  BagStatsRow row;
  row.pair = fields[0];
  if (field != 8 || row.pair.empty() ||
      !parse_count(fields[1], &row.pairs) ||
      !parse_count(fields[2], &row.word_bags) ||
      !parse_count(fields[3], &row.with_pairs) ||
      !parse_count(fields[4], &row.total_pairs) ||
      !parse_count(fields[5], &row.max_pairs) ||
      !parse_count(fields[6], &row.solo) ||
      !parse_count(fields[7], &row.min_word_bags) ||
      !parse_count(fields[8], &row.min_with_pairs)) {
    fprintf(stderr, "%s line %zu: malformed bag-stats row\n", path.c_str(),
            line_number);
    return std::nullopt;
  }
  return row;
}
