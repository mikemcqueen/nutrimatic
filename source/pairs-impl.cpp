#include "pairs-impl.h"

#include <inttypes.h>
#include <string.h>

#include <algorithm>

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

namespace {

// A hash of `key` for PairsExactCounter's slots, cheaper than
// PairsCountsHash: its four words multiply independently.
uint64_t exact_counter_hash(PairsCounts const& key) {
  uint64_t words[4];
  memcpy(words, key.values.data(), sizeof words);
  uint64_t hash = words[0] * UINT64_C(0x9e3779b97f4a7c15) ^
                  words[1] * UINT64_C(0xc2b2ae3d27d4eb4f) ^
                  words[2] * UINT64_C(0x165667b19e3779f9) ^
                  words[3] * UINT64_C(0xd6e8feb86659fd93);
  hash ^= hash >> 31;
  hash *= UINT64_C(0x9e3779b97f4a7c15);
  return hash ^ (hash >> 29);
}

}  // namespace

uint32_t PairsExactCounter::find(PairsCounts const& key) const {
  if (slots_.empty()) return NO_GROUP;
  uint64_t const hash = exact_counter_hash(key);
  uint64_t const tag = hash >> 32 << 32;
  size_t const mask = slots_.size() - 1;
  for (size_t i = hash & mask;; i = (i + 1) & mask) {
    uint64_t const slot = slots_[i];
    if (slot == 0) return NO_GROUP;
    if ((slot >> 32 << 32) != tag) continue;
    uint32_t const group = uint32_t(slot) - 1;
    if (all_.keys[group] == key) return group;
  }
}

void PairsExactCounter::insert(PairsCounts const& key, uint32_t group) {
  if (2 * (size_t(group) + 1) > slots_.size()) {
    slots_.assign(std::max<size_t>(1024, 2 * slots_.size()), 0);
    for (uint32_t g = 0; g < group; ++g) insert(all_.keys[g], g);
  }
  uint64_t const hash = exact_counter_hash(key);
  size_t const mask = slots_.size() - 1;
  size_t i = hash & mask;
  while (slots_[i] != 0) i = (i + 1) & mask;
  slots_[i] = (hash >> 32 << 32) | (uint64_t(group) + 1);
}

void PairsExactCounter::add(std::string const& word) {
  if ((int) word.size() < min_word_length_) return;
  PairsCounts const counts = pairs_letter_counts(word.c_str());
  if (counts == PairsCounts{} || !pairs_fits(bag_, counts)) return;

  uint32_t group = find(counts);
  if (group == NO_GROUP) {
    group = uint32_t(groups_.size());
    all_.keys.push_back(counts);
    all_.numbers.push_back(group);
    sizes_.push_back(0);
    int length = 0;
    for (uint8_t value : counts.values) length += value;
    lengths_.push_back(uint8_t(length));
    groups_.push_back({});
    insert(counts, group);
  }
  std::vector<std::string>& words = groups_[group];
  for (std::string const& existing : words)
    if (existing == word) return;
  words.push_back(word);
  ++sizes_[group];
}

void PairsExactCounter::filter(Groups const& from, PairsCounts const& bag,
                               Groups* out) const {
  out->keys.clear();
  out->numbers.clear();
  for (size_t i = 0; i < from.keys.size(); ++i) {
    if (!pairs_fits(bag, from.keys[i])) continue;
    out->keys.push_back(from.keys[i]);
    out->numbers.push_back(from.numbers[i]);
  }
}

// Each pair is counted from its shorter word, or from the lower-numbered one
// when both are as long.
uint64_t PairsExactCounter::count(PairsCounts const& bag,
                                  Groups const& groups) const {
  int length = 0;
  for (uint8_t value : bag.values) length += value;
  uint64_t total = 0;
  PairsCounts remaining;
  for (size_t i = 0; i < groups.keys.size(); ++i) {
    uint32_t const a = groups.numbers[i];
    int const twice = 2 * lengths_[a];
    if (twice > length) continue;
    PairsCounts const& key = groups.keys[i];
    if (!pairs_fits(bag, key)) continue;
    pairs_subtract(bag, key, &remaining);
    uint64_t const size = sizes_[a];
    if (twice == length && remaining == key) {
      total += size * (size - 1) / 2;
      continue;
    }
    uint32_t const b = find(remaining);
    if (b != NO_GROUP && (twice < length || b > a)) total += size * sizes_[b];
  }
  return total;
}
