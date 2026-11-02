#ifndef NUTRIMATIC_ANAGRAM_SAMPLER_IMPL_H
#define NUTRIMATIC_ANAGRAM_SAMPLER_IMPL_H

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pairs-impl.h"

inline constexpr uint32_t SAMPLER_NO_WORD = UINT32_MAX;

struct SamplerKeyHash {
    size_t operator()(PairsKey128 key) const {
        uint64_t hash = uint64_t(key) ^
                        uint64_t(key >> 64) * UINT64_C(0x165667b19e3779f9);
        hash *= UINT64_C(0x9e3779b97f4a7c15);
        return hash ^ (hash >> 29);
    }
};

// How units are weighed, in log form: a pair unit
// beta * (SCORE - pair_penalty), a solo unit
// beta * (solo_scale * ln(COUNT) - solo_penalty).
struct SamplerWeights {
    double beta = 1;
    double pair_penalty = 4.5;
    double solo_scale = 0.5;
    double solo_penalty = 4;
};

// A pair `a,b` or a solo word `a`, as read.
struct SamplerUnit {
    std::string text;
    uint32_t words[2];
    uint32_t group;
    double log_weight;
    double weight;

    bool solo() const { return words[1] == SAMPLER_NO_WORD; }
};

// Units whose letters fit one bag, grouped by letters, groups shortest
// first.
class SamplerModel {
  public:
    using Key = PairsKey128;

    // Takes the bag, already az_letters(). Returns false, with `*error` set,
    // when its packing takes more than 128 bits.
    bool load(std::string_view letters, SamplerWeights const& weights,
              std::string* error);

    // Add units, in input order; ones that don't fit the bag, have no
    // letters, have a weight that rounds to 0 or, for a pair, two identical
    // words are skipped.
    void add_pair(std::string_view a, std::string_view b, double score);
    void add_solo(std::string_view word, double count);

    // Groups the units; call once after the last add. Returns false, with
    // `*error` set, when a group's weight passes MAX_GROUP_WEIGHT.
    bool finish(std::string* error);

    // Keeps a fill's weight, a product of two group weights, and the sum of
    // a fill list's weights finite.
    static constexpr double MAX_GROUP_WEIGHT = 1e147;

    Key bag() const { return bag_; }
    PairsCounts const& bag_counts() const { return bag_counts_; }
    int bag_length() const { return bag_length_; }
    Key guards() const { return guards_; }

    std::vector<SamplerUnit> const& units() const { return units_; }
    size_t word_count() const { return words_.size(); }
    // The number of `word`, or SAMPLER_NO_WORD.
    uint32_t find_word(std::string_view word) const;

    size_t group_count() const { return groups_.keys.size(); }
    Key group_key(size_t g) const { return groups_.keys[g]; }
    PairsCounts const& group_counts(size_t g) const { return counts_[g]; }
    int group_length(size_t g) const { return lengths_[g]; }
    // Sum of the group's unit weights, and of the products of its unordered
    // pairs of distinct units.
    double group_weight(size_t g) const { return sum_[g]; }
    double group_pair_weight(size_t g) const { return pair_sum_[g]; }
    uint32_t const* group_begin(size_t g) const {
        return members_.data() + offsets_[g];
    }
    uint32_t const* group_end(size_t g) const {
        return members_.data() + offsets_[g + 1];
    }
    size_t group_size(size_t g) const { return offsets_[g + 1] - offsets_[g]; }

    // The first group position at least `length` letters long.
    size_t length_start(int length) const {
        return length_starts_[std::min(length, bag_length_ + 1)];
    }

    PackedGroups<Key> const& packed_groups() const { return groups_; }
    PackedRemainderGroups<Key> const& hashed() const { return hashed_; }

  private:
    uint32_t word(std::string_view word);
    void add(PairsCounts const& counts, uint32_t a, uint32_t b,
             std::string text, double log_weight);

    SamplerWeights weights_;
    PairsCounts bag_counts_;
    PairsPacking packing_{PairsCounts{}};
    Key bag_ = 0;
    Key guards_ = 0;
    int bag_length_ = 0;
    std::vector<std::string> words_;
    std::unordered_map<std::string, uint32_t> word_numbers_;
    std::vector<SamplerUnit> units_;
    std::vector<PairsCounts> unit_counts_;
    PackedGroups<Key> groups_;
    std::vector<PairsCounts> counts_;
    std::vector<uint8_t> lengths_;
    std::vector<double> sum_;
    std::vector<double> pair_sum_;
    std::vector<uint32_t> offsets_;
    std::vector<uint32_t> members_;
    std::vector<size_t> length_starts_;
    PackedRemainderGroups<Key> hashed_;
};

// The chain's random numbers.
class SamplerRng {
  public:
    SamplerRng(uint64_t seed, uint64_t chain);
    // Uniform in [0, 1).
    double uniform() { return double(engine_() >> 11) * 0x1p-53; }
    // Uniform in [0, n).
    size_t below(size_t n) { return size_t(uniform() * double(n)); }
    // Standard Gumbel.
    double gumbel();

  private:
    std::mt19937_64 engine_;
};

// A way to make up a sub-bag from one group (second NO) or two.
struct SamplerFill {
    static uint32_t const NO = UINT32_MAX;
    uint32_t first;
    uint32_t second;
    double weight;
};

// The fills of one sub-bag, with their running total weights.
struct SamplerFillList {
    std::vector<SamplerFill> fills;
    std::vector<double> totals;
};

// Lists the fills of `bag`, sizes 1 and 2, with their weights, and draws
// from them.
class SamplerFills {
  public:
    explicit SamplerFills(SamplerModel const& model) : model_(model) {}

    // The fills of `bag`, `length` letters, valid until the next list().
    SamplerFillList const& list(SamplerModel::Key bag, int length);

    // Draws one of `list` by weight, then its units, to `out`.
    void draw(SamplerFillList const& list, SamplerRng* rng,
              std::vector<uint32_t>* out) const;

  private:
    uint32_t draw_unit(size_t group, double total, uint32_t skip,
                       SamplerRng* rng) const;

    SamplerModel const& model_;
    SamplerFillList list_;
    PackedPairsScratch<SamplerModel::Key> scratch_;
};

// The first full anagram a full-anagram search finds in the model's bag,
// units tried in order of log weight plus Gumbel noise. Returns false when
// there is none.
bool sampler_start(SamplerModel const& model, SamplerRng* rng,
                   std::vector<uint32_t>* out);

// One chain of full anagrams.
class SamplerChain {
  public:
    SamplerChain(SamplerModel const& model, std::vector<uint32_t> start,
                 SamplerRng* rng);

    // One move; the state after it is one draw.
    void move();

    std::vector<uint32_t> const& units() const { return units_; }
    bool has_word(uint32_t word) const {
        return word < word_uses_.size() && word_uses_[word] != 0;
    }
    int pair_units() const { return pair_units_; }
    int solo_units() const { return int(units_.size()) - pair_units_; }

    // Moves proposed and accepted, by r - 1 and s - 1.
    uint64_t proposed[2][2] = {};
    uint64_t accepted[2][2] = {};

  private:
    void use(uint32_t unit, int delta);

    SamplerModel const& model_;
    SamplerRng* rng_;
    SamplerFills fills_;
    std::vector<uint32_t> units_;
    std::vector<uint8_t> word_uses_;
    int pair_units_ = 0;
    std::vector<uint32_t> drawn_;
};

#endif
