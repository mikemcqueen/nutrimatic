#ifndef NUTRIMATIC_PAIRS_IMPL_H
#define NUTRIMATIC_PAIRS_IMPL_H

#include <stdio.h>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

inline constexpr int PAIRS_DEFAULT_MIN_WORD_LENGTH = 4;

// Which pairs pairs prints: those of distinct words of at least
// min_word_length letters (0 for no minimum) that fit together within letters
// less used_letters, or that use all of them when exact. With exact and
// allow_solo, single words that use all of them are printed too.
struct PairsOptions {
  std::string letters;
  std::string used_letters;
  int min_word_length = PAIRS_DEFAULT_MIN_WORD_LENGTH;
  bool exact = false;
  bool allow_solo = false;
};

// A word's a-z letter counts, padded to two SSE lanes.
struct alignas(32) PairsCounts {
  std::array<uint8_t, 32> values{};

  bool operator==(PairsCounts const&) const = default;
};

struct PairsCountsHash {
  size_t operator()(PairsCounts const& counts) const;
};

// Word pairs found under PairsOptions among the words added, each written
// left,right with left < right.
class Pairs {
 public:
  // Takes `options` and the letters left once its used_letters are removed.
  // Returns false, with the error diagnosed, when they aren't all available
  // or nothing is left.
  bool load(PairsOptions const& options);

  // Adds `word` when it's long enough, has a letter, and fits the letters. Only valid after
  // load() succeeds.
  void add(std::string const& word);

  // Writes the solo words, then the pairs, one per line.
  void write(FILE* out) const;

  // The lines write() would write.
  std::vector<std::string> lines() const;

 private:
  template <typename Emit>
  void each_line(Emit emit) const;

  PairsOptions options_;
  PairsCounts bag_;
  std::vector<PairsCounts> keys_;
  std::vector<std::vector<std::string>> groups_;
  std::unordered_map<PairsCounts, size_t, PairsCountsHash> group_indices_;
};

#endif
