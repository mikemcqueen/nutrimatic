#include "bad-bag-bitmap.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

char const BAD_BAG_BITMAP_MAGIC[] = "bad-bags-bitmap 1";

namespace {

// Reads a line of `in` that is `name`, a space, and the rest, into `value`.
bool read_field(FILE* in, char const* name, std::string* value) {
  std::string line;
  if (!read_line(in, &line)) return false;
  size_t const length = strlen(name);
  if (line.compare(0, length, name) != 0 || line.size() <= length ||
      line[length] != ' ')
    return false;
  *value = line.substr(length + 1);
  return true;
}

bool parse_size(std::string const& text, size_t* out) {
  if (text.empty() ||
      !std::all_of(text.begin(), text.end(),
                   [](char ch) { return ch >= '0' && ch <= '9'; }))
    return false;
  *out = size_t(strtoull(text.c_str(), NULL, 10));
  return true;
}

}  // namespace

bool read_line(FILE* in, std::string* line) {
  line->clear();
  for (int ch; (ch = getc(in)) != EOF;) {
    if (ch == '\n') return true;
    line->push_back(char(ch));
  }
  return !line->empty();
}

bool BadBagBitmap::is_bad(LetterCounts const& letter_counts) const {
  size_t length = 0;
  for (int count : letter_counts) length += size_t(count);
  if (length < min_length) return false;
  size_t number;
  if (!index.index_of(letter_counts, &number)) return false;
  return !is_good_bit(good, number);
}

void write_bad_bag_bitmap(FILE* out, std::string const& letters,
                          size_t min_length, SubBagIndex const& index,
                          std::vector<uint64_t> const& good) {
  fprintf(out, "%s\nletters %s\nmin %zu\nmax %zu\nbits %zu\n\n",
          BAD_BAG_BITMAP_MAGIC, letters.c_str(), min_length,
          index.max_length(), index.total());
  fwrite(good.data(), sizeof good[0], good.size(), out);
}

std::optional<BadBagBitmap> read_bad_bag_bitmap(FILE* in,
                                                std::string const& path) {
  std::string letters, min, max, bits, blank;
  size_t min_length, max_length, total;
  if (!read_field(in, "letters", &letters) ||
      !read_field(in, "min", &min) ||
      !read_field(in, "max", &max) ||
      !read_field(in, "bits", &bits) ||
      !read_line(in, &blank) || !blank.empty() ||
      !std::is_sorted(letters.begin(), letters.end()) ||
      !parse_size(min, &min_length) || !parse_size(max, &max_length) ||
      !parse_size(bits, &total)) {
    fprintf(stderr, "%s: malformed bad-bag bitmap header\n", path.c_str());
    return std::nullopt;
  }
  BadBagBitmap bitmap{letters, min_length, SubBagIndex(letters, max_length),
                      {}};
  if (!bitmap.index.fits() || bitmap.index.max_length() != max_length ||
      bitmap.index.total() != total) {
    fprintf(stderr, "%s: bad-bag bitmap header doesn't match its letters\n",
            path.c_str());
    return std::nullopt;
  }
  bitmap.good.resize(total / 64 + 1);
  if (fread(bitmap.good.data(), sizeof bitmap.good[0], bitmap.good.size(),
            in) != bitmap.good.size() ||
      getc(in) != EOF) {
    fprintf(stderr, "%s: bad-bag bitmap is the wrong size\n", path.c_str());
    return std::nullopt;
  }
  return bitmap;
}
