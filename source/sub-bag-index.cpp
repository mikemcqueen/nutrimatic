#include "sub-bag-index.h"

#include <algorithm>

SubBagIndex::SubBagIndex(std::string const& letters, size_t max_length)
    : max_length_(std::min(max_length, letters.size())) {
  for (char ch : letters) {
    if (symbols_.empty() || symbols_.back() != ch) {
      symbols_.push_back(ch);
      counts_.push_back(0);
    }
    ++counts_.back();
  }
  for (int ch = 0; ch <= UCHAR_MAX; ++ch) slot_of_[ch] = SLOT_MISSING;
  for (size_t i = 0; i < symbols_.size(); ++i)
    slot_of_[(unsigned char) symbols_[i]] = int(i);

  size_t const width = symbols_.size();
  std::vector<long double> below((width + 1) * (max_length_ + 1), 1);
  for (size_t i = width; i-- > 0;)
    for (size_t r = 0; r <= max_length_; ++r) {
      long double sum = 0;
      for (size_t d = 0; d <= std::min(size_t(counts_[i]), r); ++d)
        sum += below[(i + 1) * (max_length_ + 1) + r - d];
      below[i * (max_length_ + 1) + r] = sum;
    }
  sub_bags_ = below[max_length_];
  if (!fits()) return;

  for (int count : counts_) max_count_ = std::max(max_count_, size_t(count));
  ranks_.assign(width * (max_length_ + 1) * (max_count_ + 1), 0);
  for (size_t i = 0; i < width; ++i)
    for (size_t r = 0; r <= max_length_; ++r) {
      size_t sum = 0;
      for (size_t d = 0; d <= std::min(size_t(counts_[i]), r); ++d) {
        ranks_[(i * (max_length_ + 1) + r) * (max_count_ + 1) + d] = sum;
        sum += size_t(below[(i + 1) * (max_length_ + 1) + r - d]);
      }
    }
  total_ = size_t(sub_bags_);
}

size_t SubBagIndex::number(std::span<int const> digits, size_t first) const {
  size_t sum = 0;
  size_t budget = max_length_;
  for (size_t i = first; i < symbols_.size(); ++i) {
    sum += rank(i, budget, size_t(digits[i]));
    budget -= size_t(digits[i]);
  }
  return sum;
}

bool SubBagIndex::index_of(LetterCounts const& letter_counts,
                           size_t* index) const {
  int digits[UCHAR_MAX + 1];
  std::fill_n(digits, symbols_.size(), 0);
  size_t length = 0;
  for (int j = 0; j < 26; ++j) {
    if (letter_counts[j] == 0) continue;
    int const slot = slot_of(char('a' + j));
    if (slot == SLOT_MISSING || letter_counts[j] > counts_[size_t(slot)])
      return false;
    digits[slot] = letter_counts[j];
    length += size_t(letter_counts[j]);
  }
  if (length > max_length_) return false;
  *index = number(std::span<int const>(digits, symbols_.size()));
  return true;
}
