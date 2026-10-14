#ifndef NUTRIMATIC_PAIRS_IMPL_H
#define NUTRIMATIC_PAIRS_IMPL_H

#include <smmintrin.h>
#include <stdio.h>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

inline constexpr int PAIRS_DEFAULT_MIN_WORD_LENGTH = 4;
inline constexpr int PAIRS_MAX_WORDS = 2;

// Which lines pairs prints: combinations of min_words to max_words (1 to
// PAIRS_MAX_WORDS) distinct words of at least min_word_length letters (0 for
// no minimum) that fit together within letters less used_letters, or that use
// all of them when exact. With count, pairs prints how many lines there are
// instead of the lines.
struct PairsOptions {
  std::string letters;
  std::string used_letters;
  int min_word_length = PAIRS_DEFAULT_MIN_WORD_LENGTH;
  bool exact = false;
  int min_words = 2;
  int max_words = 2;
  bool count = false;
};

// A word's a-z letter counts, zero-padded to two SSE lanes.
struct alignas(32) PairsCounts {
  std::array<uint8_t, 32> values{};

  bool operator==(PairsCounts const&) const = default;
};

struct PairsCountsHash {
  size_t operator()(PairsCounts const& counts) const;
};

// The a-z letter counts of `s`, case-insensitive; other characters are
// ignored.
PairsCounts pairs_letter_counts(char const* s);

// Lanes where `word` has more of a letter than `pool`, nonzero when `word`
// doesn't fit.
inline __m128i pairs_excess(__m128i p0, __m128i p1, __m128i w0, __m128i w1) {
  return _mm_or_si128(_mm_subs_epu8(w0, p0), _mm_subs_epu8(w1, p1));
}

// Whether `word` fits within `pool`.
inline bool pairs_fits(PairsCounts const& pool, PairsCounts const& word) {
  __m128i const* p = reinterpret_cast<__m128i const*>(pool.values.data());
  __m128i const* w = reinterpret_cast<__m128i const*>(word.values.data());
  __m128i const excess =
      pairs_excess(_mm_load_si128(p), _mm_load_si128(p + 1),
                   _mm_load_si128(w), _mm_load_si128(w + 1));
  return _mm_testz_si128(excess, excess);
}

// Writes `pool` less `word` to `out` and returns true, or returns false when
// `word` doesn't fit `pool`. `out` may alias `pool`.
inline bool pairs_subtract(PairsCounts const& pool, PairsCounts const& word,
                           PairsCounts* out) {
  __m128i const* p = reinterpret_cast<__m128i const*>(pool.values.data());
  __m128i const* w = reinterpret_cast<__m128i const*>(word.values.data());
  __m128i const p0 = _mm_load_si128(p), p1 = _mm_load_si128(p + 1);
  __m128i const w0 = _mm_load_si128(w), w1 = _mm_load_si128(w + 1);
  __m128i const excess = pairs_excess(p0, p1, w0, w1);
  if (!_mm_testz_si128(excess, excess)) return false;
  __m128i* result = reinterpret_cast<__m128i*>(out->values.data());
  _mm_store_si128(result, _mm_sub_epi8(p0, w0));
  _mm_store_si128(result + 1, _mm_sub_epi8(p1, w1));
  return true;
}

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

  // Writes the single words, then the pairs, one per line, or, with
  // options.count, how many lines that is.
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

// Counts the lines `pairs -x -c` prints for many bags within one: built once
// from the dictionary words that fit `bag` and are at least
// `min_word_length` letters long, then queried per sub-bag.
class PairsExactCounter {
 public:
  PairsExactCounter(PairsCounts const& bag, int min_word_length);

  // Adds `word` under the same rules as Pairs::add().
  void add(std::string const& word);

  // Groups of words with the same letters: each one's letters and number.
  struct Groups {
    std::vector<PairsCounts> keys;
    std::vector<uint32_t> numbers;
  };

  // Every group.
  Groups const& groups() const { return all_; }

  // The number of distinct words in one of groups().
  uint32_t group_size(size_t group) const { return sizes_[group]; }

  // Writes the groups of `from` whose letters fit `bag` to `out`.
  void filter(Groups const& from, PairsCounts const& bag, Groups* out) const;

  // How many pairs of distinct words use exactly the letters of `bag`, which
  // must fit the constructor's bag.
  uint64_t count(PairsCounts const& bag) const { return count(bag, all_); }

  // As count(bag), looking only at `groups`, which must hold every group
  // that fits `bag`.
  uint64_t count(PairsCounts const& bag, Groups const& groups) const;

 private:
  static uint32_t const NO_GROUP = UINT32_MAX;

  // The number of the group whose letters are `key`, or NO_GROUP.
  uint32_t find(PairsCounts const& key) const;
  void insert(PairsCounts const& key, uint32_t group);

  PairsCounts bag_;
  int min_word_length_;
  Groups all_;
  std::vector<uint32_t> sizes_;
  std::vector<uint8_t> lengths_;
  std::vector<std::vector<std::string>> groups_;
  // 16 hash tags per bucket, probed at once, over slots_' group numbers.
  // Zero is empty; occupied tags have their high bit set. No entries are
  // erased, so a bucket with an empty slot ends the probe chain.
  struct alignas(16) Tags {
    uint8_t values[16]{};
  };
  std::vector<Tags> tags_;
  std::vector<uint32_t> slots_;
};

#endif
