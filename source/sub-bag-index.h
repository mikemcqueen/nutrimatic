#ifndef NUTRIMATIC_SUB_BAG_INDEX_H
#define NUTRIMATIC_SUB_BAG_INDEX_H

#include <limits.h>
#include <stddef.h>

#include <span>
#include <string>
#include <vector>

#include "letter-bag.h"

// Numbers the sub-bags of a letter bag holding at most max_length() letters,
// 0 through total() - 1, the empty bag 0. A sub-bag is one digit per symbol,
// how many of that letter it holds; sub-bags are numbered in increasing
// order of their digits, the first symbol's most significant.
class SubBagIndex {
 public:
  // Past this many sub-bags there is no numbering; fits() is false.
  static size_t const MAX_SUB_BAGS = size_t(1) << 33;

  // Numbers the sub-bags of `letters`, sorted, of at most `max_length`
  // letters, or of all of them when `max_length` is longer.
  SubBagIndex(std::string const& letters, size_t max_length);

  bool fits() const { return sub_bags_ <= MAX_SUB_BAGS; }
  long double sub_bag_count() const { return sub_bags_; }
  // The number of sub-bags; 0 unless fits().
  size_t total() const { return total_; }
  size_t max_length() const { return max_length_; }
  // The distinct letters, sorted, and how many of each the bag holds.
  std::string const& symbols() const { return symbols_; }
  std::vector<int> const& counts() const { return counts_; }
  // The slot of `ch` in symbols(), or SLOT_MISSING.
  int slot_of(char ch) const { return slot_of_[(unsigned char) ch]; }

  // How many sub-bags put a digit below `digit` at slot `slot` when the
  // slots before it leave `budget` letters to spend; a sub-bag's number is
  // the sum of this over its slots. Needs fits().
  size_t rank(size_t slot, size_t budget, size_t digit) const {
    return ranks_[(slot * (max_length_ + 1) + budget) * (max_count_ + 1) +
                  digit];
  }

  // The number of the sub-bag holding `digits`, one per slot, whose digits
  // before slot `first` are all zero. Needs fits().
  size_t number(std::span<int const> digits, size_t first = 0) const;

  // Sets `*index` to the number of the sub-bag holding `letter_counts`;
  // false when that isn't a sub-bag of at most max_length() letters. Needs
  // fits().
  bool index_of(LetterCounts const& letter_counts, size_t* index) const;

  static int const SLOT_MISSING = -1;

 private:
  size_t max_length_;
  std::string symbols_;
  std::vector<int> counts_;
  int slot_of_[UCHAR_MAX + 1];
  long double sub_bags_ = 0;
  size_t total_ = 0;
  size_t max_count_ = 0;
  std::vector<size_t> ranks_;
};

#endif
