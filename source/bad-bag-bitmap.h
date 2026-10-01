#ifndef NUTRIMATIC_BAD_BAG_BITMAP_H
#define NUTRIMATIC_BAD_BAG_BITMAP_H

#include <stdint.h>
#include <stdio.h>

#include <optional>
#include <string>
#include <vector>

#include "sub-bag-index.h"

// The first line of a bad-bag bitmap file.
extern char const BAD_BAG_BITMAP_MAGIC[];

// Whether bit `number` of `good`, laid out as BadBagBitmap::good, is set.
inline bool is_good_bit(std::vector<uint64_t> const& good, size_t number) {
  return (good[number / 64] >> (number % 64)) & 1;
}

// Which sub-bags of `letters` words of at least min_length letters spell
// exactly, one bit per sub-bag of at most index.max_length() letters by
// its number in index, set when spellable. Bits of sub-bags shorter than
// min_length are clear but mean nothing.
//
// Written by bad-bags --bitmap as a text header, each line ending in '\n',
//   bad-bags-bitmap 1
//   letters LETTERS      (sorted)
//   min MIN_LENGTH
//   max MAX_LENGTH
//   bits TOTAL           (index.total())
//   (an empty line)
// followed by TOTAL / 64 + 1 native-endian uint64_t words, bit i of the
// whole being bit i % 64 of word i / 64.
struct BadBagBitmap {
  std::string letters;
  size_t min_length;
  SubBagIndex index;
  std::vector<uint64_t> good;

  // Whether the bag holding `letter_counts` is a sub-bag of `letters` of
  // min_length through index.max_length() letters that no words spell.
  bool is_bad(LetterCounts const& letter_counts) const;
};

// Writes the header and `good` described at BadBagBitmap to `out`.
void write_bad_bag_bitmap(FILE* out, std::string const& letters,
                          size_t min_length, SubBagIndex const& index,
                          std::vector<uint64_t> const& good);

// Reads one line of `in` into `line`, without its '\n'; false at end of
// file with nothing read.
bool read_line(FILE* in, std::string* line);

// The bitmap in `in`, the file at `path` read through its
// BAD_BAG_BITMAP_MAGIC line; none, diagnosed, when the rest isn't a
// well-formed bitmap.
std::optional<BadBagBitmap> read_bad_bag_bitmap(FILE* in,
                                                std::string const& path);

#endif
