#include "pairs-impl.h"

#include <inttypes.h>

#include "dfs-cli-args.h"
#include "letter-bag.h"

namespace {

constexpr size_t ALPHA = 26;

static_assert(sizeof(PairsCounts) == 32);

}  // namespace

PairsCounts pairs_letter_counts(char const* s) {
  PairsCounts out{};
  for (; *s; ++s) {
    char c = *s;
    if (c >= 'A' && c <= 'Z') c += 32;
    if (c >= 'a' && c <= 'z') ++out.values[c - 'a'];
  }
  return out;
}

bool pairs_subtract(PairsCounts const& pool, PairsCounts const& word,
                    PairsCounts* out) {
  PairsCounts result{};
  for (size_t i = 0; i < ALPHA; ++i) {
    if (word.values[i] > pool.values[i]) return false;
    result.values[i] = pool.values[i] - word.values[i];
  }
  *out = result;
  return true;
}

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
  bag_ = pairs_letter_counts(letters.c_str());
  return true;
}

void Pairs::add(std::string const& word) {
  if ((int) word.size() < options_.min_word_length) return;
  PairsCounts const counts = pairs_letter_counts(word.c_str());
  if (counts == PairsCounts{}) return;
  PairsCounts remaining;
  if (!pairs_subtract(bag_, counts, &remaining)) return;

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
  if (options_.min_words <= 1) {
    if (options_.exact) {
      auto const solo = group_indices_.find(bag_);
      if (solo != group_indices_.end())
        for (std::string const& word : groups_[solo->second])
          emit(word, nullptr);
    } else {
      for (std::vector<std::string> const& group : groups_)
        for (std::string const& word : group) emit(word, nullptr);
    }
  }
  if (options_.max_words < 2) return;

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
    pairs_subtract(bag_, keys_[a], &remaining);

    bool const same_group_fits = options_.exact
        ? remaining == keys_[a] : pairs_fits(remaining, keys_[a]);
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
        matching_count += pairs_fits(remaining, keys_[b]);
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
  if (options_.count) {
    uint64_t lines = 0;
    each_line([&lines](std::string const&, std::string const*) { ++lines; });
    fprintf(out, "%" PRIu64 "\n", lines);
    return;
  }
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

PairsExactCounter::PairsExactCounter(PairsCounts const& bag,
                                     int min_word_length)
    : bag_(bag), min_word_length_(min_word_length) {}

void PairsExactCounter::add(std::string const& word) {
  if ((int) word.size() < min_word_length_) return;
  PairsCounts const counts = pairs_letter_counts(word.c_str());
  if (counts == PairsCounts{} || !pairs_fits(bag_, counts)) return;

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

uint64_t PairsExactCounter::count(PairsCounts const& bag) const {
  uint64_t total = 0;
  PairsCounts remaining;
  for (size_t a = 0; a < keys_.size(); ++a) {
    if (!pairs_fits(bag, keys_[a])) continue;
    pairs_subtract(bag, keys_[a], &remaining);
    uint64_t const size = groups_[a].size();
    if (remaining == keys_[a]) {
      total += size * (size - 1) / 2;
      continue;
    }
    auto const partner = group_indices_.find(remaining);
    if (partner != group_indices_.end() && partner->second > a)
      total += size * groups_[partner->second].size();
  }
  return total;
}
