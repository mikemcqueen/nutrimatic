#include "pairs-impl.h"

#include <smmintrin.h>

#include "dfs-cli-args.h"

namespace {

constexpr size_t ALPHA = 26;

static_assert(sizeof(PairsCounts) == 32);

PairsCounts letter_counts(char const* s) {
  PairsCounts out{};
  for (; *s; ++s) {
    char c = *s;
    if (c >= 'A' && c <= 'Z') c += 32;
    if (c >= 'a' && c <= 'z') ++out.values[c - 'a'];
  }
  return out;
}

bool subtract(PairsCounts const& pool, PairsCounts const& word,
              PairsCounts* out) {
  PairsCounts result{};
  for (size_t i = 0; i < ALPHA; ++i) {
    if (word.values[i] > pool.values[i]) return false;
    result.values[i] = pool.values[i] - word.values[i];
  }
  *out = result;
  return true;
}

bool fits(PairsCounts const& pool, PairsCounts const& word) {
  __m128i const* p = reinterpret_cast<__m128i const*>(pool.values.data());
  __m128i const* w = reinterpret_cast<__m128i const*>(word.values.data());
  __m128i const excess = _mm_or_si128(
      _mm_subs_epu8(_mm_load_si128(w), _mm_load_si128(p)),
      _mm_subs_epu8(_mm_load_si128(w + 1), _mm_load_si128(p + 1)));
  return _mm_testz_si128(excess, excess);
}

}  // namespace

size_t PairsCountsHash::operator()(PairsCounts const& counts) const {
  uint64_t hash = UINT64_C(14695981039346656037);
  for (uint8_t value : counts.values) {
    hash ^= value;
    hash *= UINT64_C(1099511628211);
  }
  return static_cast<size_t>(hash);
}

bool Pairs::load(PairsOptions const& options) {
  options_ = options;
  keys_.clear();
  groups_.clear();
  group_indices_.clear();

  std::string bag;
  std::string remove;
  std::string letters;
  if (!clean_letters(options_.letters.c_str(), "letters", &bag) ||
      !clean_letters(options_.used_letters.c_str(), "used letters", &remove) ||
      !subtract_letters(bag, remove, &letters))
    return false;
  bag_ = letter_counts(letters.c_str());
  return true;
}

void Pairs::add(std::string const& word) {
  if ((int) word.size() < options_.min_word_length) return;
  PairsCounts const counts = letter_counts(word.c_str());
  if (counts == PairsCounts{}) return;
  PairsCounts remaining;
  if (!subtract(bag_, counts, &remaining)) return;

  auto const inserted = group_indices_.emplace(counts, groups_.size());
  if (inserted.second) {
    keys_.push_back(counts);
    groups_.push_back({});
  }
  std::vector<std::string>& words = groups_[inserted.first->second];
  for (std::string const& existing : words)
    if (existing == word) return;
  words.push_back(word);
}

template <typename Emit>
void Pairs::each_line(Emit emit) const {
  if (options_.exact && options_.allow_solo) {
    auto const solo = group_indices_.find(bag_);
    if (solo != group_indices_.end())
      for (std::string const& word : groups_[solo->second]) emit(word, nullptr);
  }

  auto const emit_pair = [&emit](std::string const& x, std::string const& y) {
    if (x < y)
      emit(x, &y);
    else
      emit(y, &x);
  };

  PairsCounts remaining;
  std::vector<size_t> matching_groups(groups_.size());

  for (size_t a = 0; a < groups_.size(); ++a) {
    std::vector<std::string> const& first_group = groups_[a];
    subtract(bag_, keys_[a], &remaining);

    bool const same_group_fits = options_.exact
        ? remaining == keys_[a] : fits(remaining, keys_[a]);
    if (first_group.size() >= 2 && same_group_fits) {
      for (size_t i = 0; i < first_group.size(); ++i)
        for (size_t j = i + 1; j < first_group.size(); ++j)
          emit_pair(first_group[i], first_group[j]);
    }

    size_t matching_count = 0;
    if (options_.exact) {
      auto const partner = group_indices_.find(remaining);
      if (partner != group_indices_.end() && partner->second > a)
        matching_groups[matching_count++] = partner->second;
    } else {
      for (size_t b = a + 1; b < groups_.size(); ++b) {
        matching_groups[matching_count] = b;
        matching_count += fits(remaining, keys_[b]);
      }
    }

    for (size_t match = 0; match < matching_count; ++match) {
      for (std::string const& first : first_group)
        for (std::string const& second : groups_[matching_groups[match]])
          emit_pair(first, second);
    }
  }
}

void Pairs::write(FILE* out) const {
  each_line([out](std::string const& left, std::string const* right) {
    if (right == nullptr)
      fprintf(out, "%s\n", left.c_str());
    else
      fprintf(out, "%s,%s\n", left.c_str(), right->c_str());
  });
}

std::vector<std::string> Pairs::lines() const {
  std::vector<std::string> out;
  each_line([&out](std::string const& left, std::string const* right) {
    out.push_back(right == nullptr ? left : left + "," + *right);
  });
  return out;
}
