#include "anagram-sampler-impl.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_set>

namespace {

using Key = SamplerModel::Key;

}  // namespace

bool SamplerModel::load(std::string_view letters,
                        SamplerWeights const& weights, std::string* error) {
    weights_ = weights;
    bag_counts_ = pairs_letter_counts(letters);
    packing_ = PairsPacking(bag_counts_);
    if (packing_.bits() > 128) {
        *error = "the letters pack into " + std::to_string(packing_.bits()) +
                 " bits, more than 128";
        return false;
    }
    bag_ = packing_.pack<Key>(bag_counts_);
    guards_ = packing_.guards<Key>();
    bag_length_ = letter_total(bag_counts_);
    return true;
}

uint32_t SamplerModel::word(std::string_view word) {
    auto const [it, added] =
        word_numbers_.emplace(std::string(word), uint32_t(words_.size()));
    if (added) words_.emplace_back(word);
    return it->second;
}

uint32_t SamplerModel::find_word(std::string_view word) const {
    auto const it = word_numbers_.find(std::string(word));
    return it == word_numbers_.end() ? SAMPLER_NO_WORD : it->second;
}

void SamplerModel::add(PairsCounts const& counts, uint32_t a, uint32_t b,
                       std::string text, double log_weight) {
    double const weight = std::exp(log_weight);
    if (weight == 0) return;
    units_.push_back({std::move(text), {a, b}, 0, log_weight, weight});
    unit_counts_.push_back(counts);
}

void SamplerModel::add_pair(std::string_view a, std::string_view b,
                            double score) {
    if (a == b) return;
    PairsCounts const ca = pairs_letter_counts(a);
    PairsCounts const cb = pairs_letter_counts(b);
    if (letter_total(ca) == 0 || letter_total(cb) == 0) return;
    PairsCounts const counts = pairs_add(ca, cb);
    if (!pairs_fits(bag_counts_, counts)) return;
    std::string text(a);
    text += ',';
    text += b;
    add(counts, word(a), word(b), std::move(text),
        weights_.beta * (score - weights_.pair_penalty));
}

void SamplerModel::add_solo(std::string_view solo, double count) {
    PairsCounts const counts = pairs_letter_counts(solo);
    if (letter_total(counts) == 0 || !pairs_fits(bag_counts_, counts)) return;
    add(counts, word(solo), SAMPLER_NO_WORD, std::string(solo),
        weights_.beta * (weights_.solo_scale * std::log(count) -
                         weights_.solo_penalty));
}

bool SamplerModel::finish(std::string* error) {
    std::unordered_map<Key, uint32_t, SamplerKeyHash> by_key;
    std::vector<Key> keys;
    std::vector<uint32_t> first_unit;
    std::vector<uint32_t> unit_group(units_.size());
    for (size_t u = 0; u < units_.size(); ++u) {
        Key const key = packing_.pack<Key>(unit_counts_[u]);
        auto const [it, added] = by_key.emplace(key, uint32_t(keys.size()));
        if (added) {
            keys.push_back(key);
            first_unit.push_back(uint32_t(u));
        }
        unit_group[u] = it->second;
    }
    std::vector<uint32_t> order(keys.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](uint32_t x, uint32_t y) {
        return letter_total(unit_counts_[first_unit[x]]) <
               letter_total(unit_counts_[first_unit[y]]);
    });
    std::vector<uint32_t> position(keys.size());
    for (size_t p = 0; p < order.size(); ++p) position[order[p]] = uint32_t(p);
    size_t const n = keys.size();
    groups_.keys.resize(n);
    groups_.numbers.resize(n);
    counts_.resize(n);
    lengths_.resize(n);
    sum_.assign(n, 0);
    pair_sum_.assign(n, 0);
    offsets_.assign(n + 1, 0);
    for (size_t p = 0; p < n; ++p) {
        groups_.keys[p] = keys[order[p]];
        groups_.numbers[p] = uint32_t(p);
        counts_[p] = unit_counts_[first_unit[order[p]]];
        lengths_[p] = uint8_t(letter_total(counts_[p]));
    }
    for (size_t u = 0; u < units_.size(); ++u) {
        uint32_t const g = position[unit_group[u]];
        units_[u].group = g;
        sum_[g] += units_[u].weight;
        ++offsets_[g + 1];
    }
    for (SamplerUnit const& unit : units_)
        pair_sum_[unit.group] +=
            unit.weight * (sum_[unit.group] - unit.weight) / 2;
    for (size_t g = 0; g < n; ++g) offsets_[g + 1] += offsets_[g];
    members_.resize(units_.size());
    std::vector<uint32_t> next(offsets_.begin(), offsets_.end() - 1);
    for (size_t u = 0; u < units_.size(); ++u)
        members_[next[units_[u].group]++] = uint32_t(u);
    length_starts_.assign(bag_length_ + 2, n);
    for (int length = bag_length_ + 1; length >= 0; --length) {
        size_t p = length == bag_length_ + 1 ? n : length_starts_[length + 1];
        while (p > 0 && lengths_[p - 1] >= length) --p;
        length_starts_[length] = p;
    }
    hashed_.build(groups_);
    unit_counts_.clear();
    unit_counts_.shrink_to_fit();
    double const largest =
        sum_.empty() ? 0 : *std::max_element(sum_.begin(), sum_.end());
    if (!(largest <= MAX_GROUP_WEIGHT)) {
        *error = "a group's weight is too large; lower --beta";
        return false;
    }
    return true;
}

SamplerRng::SamplerRng(uint64_t seed, uint64_t chain) {
    std::seed_seq sequence{uint32_t(seed), uint32_t(seed >> 32),
                           uint32_t(chain), uint32_t(chain >> 32)};
    engine_.seed(sequence);
}

double SamplerRng::gumbel() {
    double u;
    do u = uniform();
    while (u == 0);
    return -std::log(-std::log(u));
}

SamplerFillList const& SamplerFills::list(Key bag, int length) {
    std::vector<SamplerFill>& fills = list_.fills;
    std::vector<double>& totals = list_.totals;
    fills.clear();
    totals.clear();
    double total = 0;
    uint32_t const whole = model_.hashed().find(bag);
    if (whole != PairsExactCounter::NO_GROUP) {
        double const w = model_.group_weight(whole);
        fills.push_back({whole, SamplerFill::NO, w});
        total += w;
        totals.push_back(total);
    }
    PackedGroups<Key> const& groups = model_.packed_groups();
    packed_pairs_scan(
        groups.keys.data(), groups.numbers.data(),
        model_.length_start((length + 1) / 2),
        model_.length_start(length / 2 + 1), bag | model_.guards(),
        model_.guards(), model_.hashed(), &scratch_,
        [&](size_t i, uint32_t b) {
            uint32_t const a = uint32_t(i);
            double w;
            if (a == b) {
                if (model_.group_size(a) < 2) return;
                w = model_.group_pair_weight(a);
            } else {
                w = model_.group_weight(a) * model_.group_weight(b);
            }
            fills.push_back({a, b, w});
            total += w;
            totals.push_back(total);
        });
    return list_;
}

uint32_t SamplerFills::draw_unit(size_t group, double total, uint32_t skip,
                                 SamplerRng* rng) const {
    std::vector<SamplerUnit> const& units = model_.units();
    double target = rng->uniform() * total;
    uint32_t last = SamplerFill::NO;
    for (uint32_t const* u = model_.group_begin(group);
         u != model_.group_end(group); ++u) {
        if (*u == skip) continue;
        last = *u;
        target -= units[*u].weight;
        if (target < 0) return *u;
    }
    return last;
}

void SamplerFills::draw(SamplerFillList const& list, SamplerRng* rng,
                        std::vector<uint32_t>* out) const {
    out->clear();
    std::vector<double> const& totals = list.totals;
    double const target = rng->uniform() * totals.back();
    size_t const f = std::min(
        size_t(std::upper_bound(totals.begin(), totals.end(), target) -
               totals.begin()),
        totals.size() - 1);
    SamplerFill const& fill = list.fills[f];
    double const sum = model_.group_weight(fill.first);
    if (fill.second == SamplerFill::NO) {
        out->push_back(draw_unit(fill.first, sum, SamplerFill::NO, rng));
        return;
    }
    if (fill.second != fill.first) {
        out->push_back(draw_unit(fill.first, sum, SamplerFill::NO, rng));
        out->push_back(draw_unit(fill.second, model_.group_weight(fill.second),
                                 SamplerFill::NO, rng));
        return;
    }
    std::vector<SamplerUnit> const& units = model_.units();
    double target_first = rng->uniform() * fill.weight * 2;
    uint32_t first = SamplerFill::NO;
    for (uint32_t const* u = model_.group_begin(fill.first);
         u != model_.group_end(fill.first); ++u) {
        first = *u;
        target_first -= units[*u].weight * (sum - units[*u].weight);
        if (target_first < 0) break;
    }
    out->push_back(first);
    out->push_back(
        draw_unit(fill.first, sum - units[first].weight, first, rng));
}

namespace {

enum class Search { FOUND, DEAD, BLOCKED };

class StartSearch {
  public:
    StartSearch(SamplerModel const& model, SamplerRng* rng)
        : model_(model), uses_(model.word_count(), 0) {
        for (SamplerUnit const& unit : model.units())
            keys_.push_back(unit.log_weight + rng->gumbel());
    }

    bool run(std::vector<uint32_t>* out) {
        std::vector<uint32_t> all(model_.group_count());
        std::iota(all.begin(), all.end(), 0);
        if (search(model_.bag(), model_.bag_counts(), all) != Search::FOUND)
            return false;
        *out = placed_;
        return true;
    }

  private:
    bool free(SamplerUnit const& unit) const {
        return uses_[unit.words[0]] == 0 &&
               (unit.solo() || uses_[unit.words[1]] == 0);
    }

    void use(SamplerUnit const& unit, int delta) {
        uses_[unit.words[0]] += delta;
        if (!unit.solo()) uses_[unit.words[1]] += delta;
    }

    Search search(Key bag, PairsCounts const& counts,
                  std::vector<uint32_t> const& fitting) {
        if (bag == 0) return Search::FOUND;
        if (dead_.count(bag)) return Search::DEAD;
        Key const pool = bag | model_.guards();
        Key const guards = model_.guards();
        std::vector<uint32_t> fit;
        std::array<uint32_t, 26> holding{};
        for (uint32_t const g : fitting) {
            if (((pool - model_.group_key(g)) & guards) != guards) continue;
            fit.push_back(g);
            PairsCounts const& c = model_.group_counts(g);
            for (int i = 0; i < 26; ++i) holding[i] += c.values[i] != 0;
        }
        int letter = -1;
        for (int i = 0; i < 26; ++i)
            if (counts.values[i] != 0 &&
                (letter < 0 || holding[i] < holding[letter]))
                letter = i;
        std::vector<uint32_t> candidates;
        for (uint32_t const g : fit)
            if (model_.group_counts(g).values[letter] != 0)
                candidates.insert(candidates.end(), model_.group_begin(g),
                                  model_.group_end(g));
        std::sort(candidates.begin(), candidates.end(),
                  [&](uint32_t x, uint32_t y) { return keys_[x] > keys_[y]; });
        bool blocked = false;
        std::vector<SamplerUnit> const& units = model_.units();
        for (uint32_t const u : candidates) {
            SamplerUnit const& unit = units[u];
            if (!free(unit)) {
                blocked = true;
                continue;
            }
            PairsCounts rest;
            pairs_subtract(counts, model_.group_counts(unit.group), &rest);
            use(unit, 1);
            placed_.push_back(u);
            Search const found =
                search(bag - model_.group_key(unit.group), rest, fit);
            if (found == Search::FOUND) return found;
            blocked |= found == Search::BLOCKED;
            placed_.pop_back();
            use(unit, -1);
        }
        if (blocked) return Search::BLOCKED;
        dead_.insert(bag);
        return Search::DEAD;
    }

    SamplerModel const& model_;
    std::vector<double> keys_;
    std::vector<uint8_t> uses_;
    std::vector<uint32_t> placed_;
    std::unordered_set<Key, SamplerKeyHash> dead_;
};

}  // namespace

bool sampler_start(SamplerModel const& model, SamplerRng* rng,
                   std::vector<uint32_t>* out) {
    return StartSearch(model, rng).run(out);
}

SamplerChain::SamplerChain(SamplerModel const& model,
                           std::vector<uint32_t> start, SamplerRng* rng)
    : model_(model), rng_(rng), fills_(model), units_(std::move(start)),
      word_uses_(model.word_count(), 0) {
    for (uint32_t const u : units_) use(u, 1);
}

void SamplerChain::use(uint32_t unit, int delta) {
    SamplerUnit const& u = model_.units()[unit];
    word_uses_[u.words[0]] += delta;
    if (u.solo()) return;
    word_uses_[u.words[1]] += delta;
    pair_units_ += delta;
}

namespace {

double choose(size_t n, size_t r) {
    return r == 1 ? double(n) : double(n) * double(n - 1) / 2;
}

double pick_odds(size_t r, size_t k) {
    if (k == 1) return 1;
    return r == 1 ? 0.2 : 0.8;
}

}  // namespace

void SamplerChain::move() {
    size_t const k = units_.size();
    size_t const r = k == 1 || rng_->uniform() < 0.2 ? 1 : 2;
    size_t picked[2];
    picked[0] = rng_->below(k);
    if (r == 2) {
        picked[1] = rng_->below(k - 1);
        if (picked[1] >= picked[0]) ++picked[1];
        if (picked[1] < picked[0]) std::swap(picked[0], picked[1]);
    }
    std::vector<SamplerUnit> const& units = model_.units();
    Key bag = 0;
    int length = 0;
    for (size_t i = 0; i < r; ++i) {
        uint32_t const g = units[units_[picked[i]]].group;
        bag += model_.group_key(g);
        length += model_.group_length(g);
    }
    fills_.draw(fills_.list(bag, length), rng_, &drawn_);
    size_t const s = drawn_.size();
    ++proposed[r - 1][s - 1];
    for (size_t i = 0; i < r; ++i) use(units_[picked[i]], -1);
    bool clash = false;
    for (uint32_t const u : drawn_) {
        SamplerUnit const& unit = units[u];
        clash |= word_uses_[unit.words[0]] != 0;
        if (!unit.solo()) clash |= word_uses_[unit.words[1]] != 0;
        use(u, 1);
    }
    if (!clash) {
        size_t const after = k - r + s;
        double const odds = pick_odds(s, after) * choose(k, r) /
                            (pick_odds(r, k) * choose(after, s));
        clash = odds < 1 && rng_->uniform() >= odds;
    }
    if (clash) {
        for (uint32_t const u : drawn_) use(u, -1);
        for (size_t i = 0; i < r; ++i) use(units_[picked[i]], 1);
        return;
    }
    ++accepted[r - 1][s - 1];
    for (size_t i = r; i-- > 0;) {
        units_[picked[i]] = units_.back();
        units_.pop_back();
    }
    units_.insert(units_.end(), drawn_.begin(), drawn_.end());
}
