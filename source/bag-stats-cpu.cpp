#include "bag-stats-cpu.h"

#include "bag-stats-impl.h"
#include "mix-bits.h"
#include "sub-bag-index.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr char PROGRAM[] = "bag-stats";
// The most sub-bags PairCountTable numbers, 4 bytes each.
constexpr size_t PAIR_COUNT_TABLE_MAX = size_t(1) << 28;

// A full mixed-radix index makes a remainder's number cheap to compute.
// Unlike SubBagIndex it does not restrict the length of the sub-bag.
// total() is 0 when `bag` has more than `bound` sub-bags.
class RemainderIndex {
  public:
    RemainderIndex(PairsCounts const& bag, size_t bound) {
        for (size_t i = 0; i < 26; ++i) {
            if (bag.values[i] == 0) continue;
            if (total_ > bound / (bag.values[i] + 1)) {
                total_ = 0;
                break;
            }
            digits_.push_back({i, total_});
            total_ *= bag.values[i] + 1;
        }
    }

    size_t total() const { return total_; }
    size_t number(PairsCounts const& bag) const {
        size_t out = 0;
        for (auto const& [letter, stride] : digits_)
            out += bag.values[letter] * stride;
        return out;
    }

  private:
    size_t total_ = 1;
    std::vector<std::pair<size_t, size_t>> digits_;
};

// Lookups of a cache and how many of them hit.
struct CacheCounts {
    uint64_t hits = 0;
    uint64_t lookups = 0;
};

// Pair counts of sub-bags of `bag`, shared by every thread, in at most
// `megabytes` MB, or two slots per sub-bag when that's less. Lossy: a
// bucket holds 8 entries in a cache line, and a full one has its entries
// overwritten in turn, a turn per thread. Each entry is one word, count <<
// key_bits_ | (the sub-bag's number + 1), 0 while empty, so lookups never
// see a torn entry. Counts too big for the bits left over aren't kept.
class PairCountCache {
  public:
    PairCountCache(PairsCounts const& bag, size_t megabytes)
        : index_(bag, UINT64_MAX - 1) {
        if (index_.total() == 0) return;
        size_t const buckets =
            std::min(megabytes * 1024 * 1024 / sizeof(Bucket),
                     2 * index_.total() / WAYS + 1);
        if (megabytes == 0) return;
        key_bits_ = std::bit_width(uint64_t(index_.total()));
        if (key_bits_ > MAX_KEY_BITS) return;
        memory_.reset(static_cast<char*>(
            std::calloc(buckets * sizeof(Bucket) + alignof(Bucket), 1)));
        if (!memory_) return;
        buckets_ = reinterpret_cast<Bucket*>(
            (reinterpret_cast<uintptr_t>(memory_.get()) + alignof(Bucket) - 1) &
            ~uintptr_t(alignof(Bucket) - 1));
        buckets_count_ = buckets;
        max_count_ = (uint64_t(1) << (64 - key_bits_)) - 1;
    }

    bool enabled() const { return buckets_ != nullptr; }
    // Whether sub-bag numbers are too wide to share a word with a count.
    bool too_many_sub_bags() const {
        return index_.total() == 0 ||
            std::bit_width(uint64_t(index_.total())) > MAX_KEY_BITS;
    }
    size_t capacity() const { return buckets_count_ * WAYS; }
    size_t bytes() const { return buckets_count_ * sizeof(Bucket); }

    // The key of `bag`'s entry.
    uint64_t key(PairsCounts const& bag) const {
        return index_.number(bag) + 1;
    }

    bool get(uint64_t key, uint64_t* count) const {
        uint64_t const key_mask = (uint64_t(1) << key_bits_) - 1;
        for (uint64_t& slot : bucket(mix_bits(key)).entries) {
            uint64_t const entry =
                std::atomic_ref<uint64_t>(slot).load(std::memory_order_relaxed);
            if ((entry & key_mask) != key) continue;
            *count = entry >> key_bits_;
            return true;
        }
        return false;
    }

    void put(uint64_t key, uint64_t count) const {
        if (count > max_count_) return;
        uint64_t const key_mask = (uint64_t(1) << key_bits_) - 1;
        uint64_t const entry = count << key_bits_ | key;
        Bucket& home = bucket(mix_bits(key));
        for (uint64_t& slot : home.entries) {
            std::atomic_ref<uint64_t> const ref(slot);
            uint64_t found = ref.load(std::memory_order_relaxed);
            if (found == 0 &&
                ref.compare_exchange_strong(found, entry,
                                            std::memory_order_relaxed))
                return;
            if ((found & key_mask) == key) return;
        }
        thread_local size_t turn = 0;
        std::atomic_ref<uint64_t>(home.entries[turn++ % WAYS])
            .store(entry, std::memory_order_relaxed);
    }

  private:
    static constexpr size_t WAYS = 8;
    static constexpr int MAX_KEY_BITS = 48;

    struct alignas(64) Bucket {
        uint64_t entries[WAYS];
    };
    struct Free {
        void operator()(char* p) const { std::free(p); }
    };

    Bucket& bucket(uint64_t hash) const {
        return buckets_[size_t(__uint128_t(hash) * buckets_count_ >> 64)];
    }

    RemainderIndex index_;
    int key_bits_ = 0;
    uint64_t max_count_ = 0;
    size_t buckets_count_ = 0;
    std::unique_ptr<char[], Free> memory_;
    Bucket* buckets_ = nullptr;
};

// Exact pair counts shared by every thread. Large searches fill a full
// table once by enumerating dictionary group pairs. Otherwise slots hold
// count + 1, or 0 while unknown, and are filled on demand; sub-bags outside
// that bounded-length index go through a PairCountCache of
// `cache_megabytes` MB.
class PairCountTable {
  public:
    PairCountTable(SubBagIndex index, PairsCounts const& bag,
                   PairsExactCounter const& counter, bool prepare,
                   size_t threads, size_t cache_megabytes)
        : index_(std::move(index)), full_(bag, PAIR_COUNT_TABLE_MAX),
          counter_(counter),
          prepared_(prepare && full_.total() != 0),
          cache_(bag, prepared_ ? 0 : cache_megabytes),
          counts_(static_cast<uint32_t*>(
              std::calloc(prepared_ ? full_.total() : index_.total(),
                          sizeof(uint32_t)))) {
        if (!prepared_ || !allocated()) return;
        // Enumerate dictionary group pairs once, accumulating all exact
        // counts. Their mixed-radix numbers add when their letters fit.
        auto const& keys = counter.groups().keys;
        std::vector<size_t> numbers;
        for (PairsCounts const& key : keys)
            numbers.push_back(full_.number(key));
        std::atomic<size_t> next{0};
        auto const work = [&] {
            PairsCounts remaining;
            for (size_t a; (a = next.fetch_add(1)) < keys.size();) {
                pairs_subtract(bag, keys[a], &remaining);
                uint64_t const size = counter.group_size(a);
                for (size_t b = a; b < keys.size(); ++b) {
                    if (!pairs_fits(remaining, keys[b])) continue;
                    uint64_t const count = a == b ? size * (size - 1) / 2
                        : size * counter.group_size(b);
                    if (count == 0) continue;
                    std::atomic_ref<uint32_t> slot(
                        counts_[numbers[a] + numbers[b]]);
                    uint32_t old = slot.load(std::memory_order_relaxed);
                    while (old != UINT32_MAX &&
                           !slot.compare_exchange_weak(old,
                               uint32_t(std::min<uint64_t>(UINT32_MAX,
                                                          old + count)),
                               std::memory_order_relaxed)) {}
                }
            }
        };
        std::vector<std::thread> workers;
        for (size_t t = 1; t < threads; ++t) workers.emplace_back(work);
        work();
        for (auto& worker : workers) worker.join();
    }

    bool allocated() const { return counts_ != nullptr; }
    bool needs_groups() const { return !prepared_; }
    PairCountCache const& cache() const { return cache_; }

    // `exact()`: the count from the inner dictionary groups. Adds this
    // call's PairCountCache lookup, if any, to `cache_counts`.
    template <typename Exact>
    uint64_t count(PairsCounts const& bag, Exact const& exact,
                   CacheCounts* cache_counts) const {
        if (prepared_) {
            uint32_t const count = counts_[full_.number(bag)];
            return count == UINT32_MAX ? counter_.count(bag) : count;
        }
        LetterCounts letters;
        for (int i = 0; i < 26; ++i) letters[i] = bag.values[i];
        size_t number;
        if (!index_.index_of(letters, &number)) {
            if (!cache_.enabled()) return exact();
            uint64_t const key = cache_.key(bag);
            uint64_t count;
            ++cache_counts->lookups;
            if (cache_.get(key, &count)) {
                ++cache_counts->hits;
                return count;
            }
            count = exact();
            cache_.put(key, count);
            return count;
        }
        std::atomic_ref<uint32_t> slot(counts_.get()[number]);
        uint32_t const known = slot.load(std::memory_order_relaxed);
        if (known != 0) return known - 1;
        uint64_t const count = exact();
        if (count < UINT32_MAX)
            slot.store(uint32_t(count + 1), std::memory_order_relaxed);
        return count;
    }

  private:
    struct Free {
        void operator()(uint32_t* p) const { std::free(p); }
    };

    SubBagIndex index_;
    RemainderIndex full_;
    PairsExactCounter const& counter_;
    bool prepared_;
    PairCountCache cache_;
    std::unique_ptr<uint32_t[], Free> counts_;
};

// The index for PairCountTable: the sub-bags of `letters` up to
// `max_length` letters, fewer when that numbers more than
// PAIR_COUNT_TABLE_MAX of them.
SubBagIndex pair_count_index(std::string letters, size_t max_length) {
    std::sort(letters.begin(), letters.end());
    while (true) {
        SubBagIndex index(letters, max_length);
        if (index.fits() && index.total() <= PAIR_COUNT_TABLE_MAX)
            return index;
        --max_length;
    }
}

// The longest leftover a row can ask a pair count for: R less the shortest
// bag of each pair step's list and the shortest word bag per word step.
size_t longest_leftover(
        PairsCounts const& bag,
        std::vector<std::vector<PairBag> const*> const& steps,
        std::vector<PairsCounts> const& word_bags, int words) {
    int left = letter_total(bag);
    for (std::vector<PairBag> const* step : steps) {
        int shortest = left;
        for (PairBag const& pair_bag : *step)
            shortest = std::min(shortest, letter_total(pair_bag.counts));
        left -= shortest;
    }
    int shortest = left;
    for (PairsCounts const& word_bag : word_bags)
        shortest = std::min(shortest, letter_total(word_bag));
    return size_t(std::max(0, left - words * shortest));
}

// The bytes one rest cache entry takes.
constexpr size_t REST_CACHE_ENTRY_BYTES =
    sizeof(BagStatsCounts) + 2 * (sizeof(uint64_t) + sizeof(uint32_t));

// The entries a rest cache of `megabytes` MB holds.
size_t rest_cache_entries(size_t megabytes) {
    return megabytes * 1024 * 1024 / REST_CACHE_ENTRY_BYTES;
}

// Statistics depend only on the remainder, not on which pair picks made
// it. Keep immutable records shared across workers and ranking passes, in
// at most `megabytes` MB: open addressing over remainder numbers + 1, 0
// while empty, each with its row index + 1, 0 until the row is written.
// With `report`, says on stderr when it fills.
class RestTable {
  public:
    RestTable(PairsCounts const& bag, bool enabled, size_t megabytes,
              bool report)
        : index_(bag, UINT64_MAX - 1), report_(report) {
        if (!enabled || index_.total() == 0) return;
        capacity_ = std::min({rest_cache_entries(megabytes), index_.total(),
                              size_t(UINT32_MAX)});
        if (capacity_ == 0) return;
        slots_ = 2 * capacity_;
        keys_.reset(static_cast<uint64_t*>(
            std::calloc(slots_, sizeof(uint64_t))));
        rows_index_.reset(static_cast<uint32_t*>(
            std::calloc(slots_, sizeof(uint32_t))));
        rows_.reset(static_cast<BagStatsCounts*>(
            std::calloc(capacity_, sizeof(BagStatsCounts))));
        if (!keys_ || !rows_index_ || !rows_) {
            keys_.reset();
            rows_index_.reset();
            rows_.reset();
        }
    }

    bool enabled() const { return keys_ != nullptr; }
    size_t capacity() const { return capacity_; }
    size_t remainders() const { return index_.total(); }

    bool get(PairsCounts const& rest, BagStatsCounts* out) const {
        if (!keys_) return false;
        uint64_t const key = index_.number(rest) + 1;
        for (size_t i = home(key);; i = i + 1 == slots_ ? 0 : i + 1) {
            uint64_t const found = std::atomic_ref<uint64_t>(keys_[i])
                .load(std::memory_order_relaxed);
            if (found == 0) return false;
            if (found != key) continue;
            uint32_t const row = std::atomic_ref<uint32_t>(rows_index_[i])
                .load(std::memory_order_acquire);
            if (row == 0) return false;
            *out = rows_[row - 1];
            return true;
        }
    }

    void put(PairsCounts const& rest, BagStatsCounts const& counts) const {
        if (!keys_ || next_.load(std::memory_order_relaxed) >= capacity_)
            return;
        uint64_t const key = index_.number(rest) + 1;
        for (size_t i = home(key);; i = i + 1 == slots_ ? 0 : i + 1) {
            uint64_t found = 0;
            if (!std::atomic_ref<uint64_t>(keys_[i]).compare_exchange_strong(
                    found, key, std::memory_order_relaxed)) {
                if (found == key) return;
                continue;
            }
            size_t const row = next_.fetch_add(1, std::memory_order_relaxed);
            if (row >= capacity_) return;
            if (row + 1 == capacity_ && report_)
                fprintf(stderr, "%s: cache full at %zu entries; later "
                        "remainders go uncached\n", PROGRAM, capacity_);
            rows_[row] = counts;
            std::atomic_ref<uint32_t>(rows_index_[i])
                .store(uint32_t(row + 1), std::memory_order_release);
            return;
        }
    }

  private:
    struct Free {
        template <typename T> void operator()(T* p) const { std::free(p); }
    };

    size_t home(uint64_t key) const {
        return size_t(__uint128_t(mix_bits(key)) * slots_ >> 64);
    }

    RemainderIndex index_;
    bool const report_;
    size_t capacity_ = 0;
    size_t slots_ = 0;
    std::unique_ptr<uint64_t[], Free> keys_;
    std::unique_ptr<uint32_t[], Free> rows_index_;
    std::unique_ptr<BagStatsCounts[], Free> rows_;
    mutable std::atomic<size_t> next_{0};
};

// The inner dictionary groups that fit what is left after each pick step,
// and exact pair counts over one step's.
class InnerGroups {
  public:
    virtual ~InnerGroups() = default;

    // Narrows step `step`'s groups to those of step `step` - 1, or every
    // group for step 0, that fit `rest`.
    virtual void filter(size_t step, PairsCounts const& rest) = 0;

    // Hashes step `step`'s groups for count().
    virtual void hash(size_t step) = 0;

    // How many pairs of distinct words use exactly the letters of `bag`,
    // which must fit the hashed step's rest.
    virtual uint64_t count(PairsCounts const& bag) = 0;

    // Groups that count() from this one's hashed step, without filter() or
    // hash() of their own; this one must outlive them, unchanged.
    virtual std::unique_ptr<InnerGroups> view() const = 0;
};

class ExactInnerGroups : public InnerGroups {
  public:
    ExactInnerGroups(PairsExactCounter const& counter, size_t steps)
        : counter_(counter), groups_(steps) {}

    void filter(size_t step, PairsCounts const& rest) override {
        counter_.filter(step == 0 ? counter_.groups_by_length()
                                  : groups_[step - 1],
                        rest, &groups_[step]);
    }

    void hash(size_t step) override {
        hashed_groups_ = &groups_[step];
        own_hashed_.build(groups_[step]);
        hashed_ = &own_hashed_;
    }

    uint64_t count(PairsCounts const& bag) override {
        return counter_.count(bag, *hashed_groups_, *hashed_);
    }

    std::unique_ptr<InnerGroups> view() const override {
        auto out = std::make_unique<ExactInnerGroups>(counter_, 0);
        out->hashed_groups_ = hashed_groups_;
        out->hashed_ = hashed_;
        return out;
    }

  private:
    PairsExactCounter const& counter_;
    std::vector<PairsExactCounter::Groups> groups_;
    PairsExactCounter::Groups const* hashed_groups_ = nullptr;
    RemainderGroups own_hashed_;
    RemainderGroups const* hashed_ = nullptr;
};

template <typename Key>
class PackedInnerGroups : public InnerGroups {
  public:
    PackedInnerGroups(PackedPairsCounter<Key> const& counter, size_t steps)
        : counter_(counter), groups_(steps) {}

    void filter(size_t step, PairsCounts const& rest) override {
        counter_.filter(step == 0 ? counter_.groups_by_length()
                                  : groups_[step - 1],
                        counter_.pack(rest), &groups_[step]);
    }

    void hash(size_t step) override {
        hashed_groups_ = &groups_[step];
        own_hashed_.build(groups_[step]);
        hashed_ = &own_hashed_;
    }

    uint64_t count(PairsCounts const& bag) override {
        return counter_.count(bag, *hashed_groups_, *hashed_, &scratch_);
    }

    std::unique_ptr<InnerGroups> view() const override {
        auto out = std::make_unique<PackedInnerGroups>(counter_, 0);
        out->hashed_groups_ = hashed_groups_;
        out->hashed_ = hashed_;
        return out;
    }

  private:
    PackedPairsCounter<Key> const& counter_;
    std::vector<PackedGroups<Key>> groups_;
    PackedGroups<Key> const* hashed_groups_ = nullptr;
    PackedRemainderGroups<Key> own_hashed_;
    PackedRemainderGroups<Key> const* hashed_ = nullptr;
    typename PackedPairsCounter<Key>::Scratch scratch_;
};

// The inner dictionary's counters: with `packed`, packed when the bag's
// PairsPacking fits 64 or 128 bits.
class InnerCounters {
  public:
    InnerCounters(PairsExactCounter const& counter, bool packed)
        : counter_(counter), bits_(PairsPacking(counter.bag()).bits()) {
        if (!packed) return;
        if (bits_ <= 64) packed64_.emplace(counter);
        else if (bits_ <= 128) packed128_.emplace(counter);
    }

    // The bits the bag's letter counts pack into.
    int bits() const { return bits_; }

    // The packed key size, 0 when the counts aren't packed.
    int key_bits() const { return packed64_ ? 64 : packed128_ ? 128 : 0; }

    // Groups for `steps` pick steps, over the narrowest counter.
    std::unique_ptr<InnerGroups> groups(size_t steps) const {
        if (packed64_)
            return std::make_unique<PackedInnerGroups<uint64_t>>(*packed64_,
                                                                 steps);
        if (packed128_)
            return std::make_unique<PackedInnerGroups<PairsKey128>>(
                *packed128_, steps);
        return std::make_unique<ExactInnerGroups>(counter_, steps);
    }

  private:
    PairsExactCounter const& counter_;
    int bits_;
    std::optional<PackedPairsCounter<uint64_t>> packed64_;
    std::optional<PackedPairsCounter<PairsKey128>> packed128_;
};

// The PAIRS bags a last pf counts in what is left: with `exact` the one
// whose letters are all of it, otherwise every one that fits in it.
class FinalPairBags {
  public:
    FinalPairBags(std::vector<PairBag> const& bags, bool exact)
        : bags_(bags), exact_(exact) {
        if (!exact) return;
        for (size_t i = 0; i < bags.size(); ++i)
            index_.emplace(bags[i].counts, i);
    }

    std::vector<PairBag> const& bags() const { return bags_; }

    // Calls reach(i) for each bag counted in `rest`, by its index.
    template <typename Reach>
    void each(PairsCounts const& rest, Reach const& reach) const {
        if (exact_) {
            auto const found = index_.find(rest);
            if (found != index_.end()) reach(found->second);
            return;
        }
        for (size_t i = 0; i < bags_.size(); ++i)
            if (pairs_fits(rest, bags_[i].counts)) reach(i);
    }

    // The pairs of the bags counted in `rest`.
    uint64_t count(PairsCounts const& rest) const {
        uint64_t out = 0;
        each(rest, [&](size_t i) { out += bags_[i].pairs; });
        return out;
    }

  private:
    std::vector<PairBag> const& bags_;
    bool exact_;
    std::unordered_map<PairsCounts, size_t, PairsCountsHash> index_;
};

// What a remainder's word bag combinations are handed to, as RestCounter
// counts them: add(chosen, rest, pairs) at each, `chosen` its word bags in
// the fitting list, `rest` what it leaves. With a pf and no word steps,
// add_final(i, pairs) instead for each PAIRS bag counted in a nonempty
// `rest`, by its index.
class CompletionSink {
  public:
    virtual ~CompletionSink() = default;
    virtual void add(std::vector<PairsCounts const*> const& chosen,
                     PairsCounts const& rest, uint64_t pairs) = 0;
    virtual void add_final(size_t bag, uint64_t pairs) = 0;
};

// Fills a remainder's counts over every combination of one word bag per
// word step, word bags in non-decreasing order, the last step's pairs those
// of `table` (p) or `final_bags` (pf), the other null.
class RestCounter {
  public:
    RestCounter(int words, PairCountTable const* table,
                FinalPairBags const* final_bags,
                std::unique_ptr<InnerGroups> groups)
        : words_(words), table_(table), final_bags_(final_bags),
          groups_(std::move(groups)) {}

    bool needs_groups() const { return table_ && table_->needs_groups(); }

    // The inner dictionary groups per pick step.
    InnerGroups& groups() { return *groups_; }

    // `fitting`: the word bags that fit `rest`; groups(): the inner
    // dictionary groups, hashed from a step holding every one that fits
    // `rest`. With `sink`, hands it each combination.
    void count(PairsCounts const& rest,
               std::vector<PairsCounts> const& fitting, BagStatsCounts* out,
               CompletionSink* sink = nullptr) {
        begin(fitting, sink);
        if (words_ == 0) leaf(rest, out);
        else visit(1, 0, fitting.size(), rest, out);
    }

    // As count(), with at least one word step, over only the combinations
    // whose first word bag is fitting[first].
    void count_first(PairsCounts const& rest,
                     std::vector<PairsCounts> const& fitting, size_t first,
                     BagStatsCounts* out, CompletionSink* sink = nullptr) {
        begin(fitting, sink);
        visit(1, first, first + 1, rest, out);
    }

    // The PairCountCache lookups since the last call.
    CacheCounts take_pair_cache_counts() {
        CacheCounts const out = pair_cache_counts_;
        pair_cache_counts_ = {};
        return out;
    }

  private:
    void begin(std::vector<PairsCounts> const& fitting,
               CompletionSink* sink) {
        fitting_ = &fitting;
        sink_ = sink;
        chosen_.clear();
    }

    // Whether the sink takes add_final().
    bool lists_final_bags() const {
        return sink_ && final_bags_ && words_ == 0;
    }

    void visit(int word, size_t start, size_t end, PairsCounts const& rest,
               BagStatsCounts* out) {
        PairsCounts word_rest;
        std::vector<PairsCounts> const& fitting = *fitting_;
        for (size_t i = start; i < end; ++i) {
            PairsCounts const& word_bag = fitting[i];
            if (!pairs_subtract(rest, word_bag, &word_rest)) continue;
            if (sink_) chosen_.push_back(&word_bag);
            if (word < words_)
                visit(word + 1, i, fitting.size(), word_rest, out);
            else
                leaf(word_rest, out);
            if (sink_) chosen_.pop_back();
        }
    }

    // Counts one combination, leaving `rest`.
    void leaf(PairsCounts const& rest, BagStatsCounts* out) {
        ++out->word_bags;
        bool const solo = rest == PairsCounts{};
        uint64_t const count = solo ? 0 : pairs(rest);
        if (sink_ && (solo || !lists_final_bags()))
            sink_->add(chosen_, rest, count);
        if (solo) {
            ++out->solo;
            return;
        }
        if (count == 0) return;
        ++out->with_pairs;
        out->total_pairs += count;
        out->max_pairs = std::max(out->max_pairs, count);
        out->with_min_pairs += count >= BAG_STATS_MIN_PAIRS;
    }

    // The last step's pairs in `rest`.
    uint64_t pairs(PairsCounts const& rest) {
        if (lists_final_bags()) {
            uint64_t out = 0;
            final_bags_->each(rest, [&](size_t i) {
                uint64_t const pairs = final_bags_->bags()[i].pairs;
                sink_->add_final(i, pairs);
                out += pairs;
            });
            return out;
        }
        if (final_bags_) return final_bags_->count(rest);
        if (letter_total(rest) < 2 * INNER_MIN_WORD_LENGTH) return 0;
        return table_->count(rest, [&] { return groups_->count(rest); },
                             &pair_cache_counts_);
    }

    int words_;
    PairCountTable const* table_;
    FinalPairBags const* final_bags_;
    std::vector<PairsCounts> const* fitting_ = nullptr;
    CompletionSink* sink_ = nullptr;
    std::vector<PairsCounts const*> chosen_;
    std::unique_ptr<InnerGroups> groups_;
    CacheCounts pair_cache_counts_;
};

// Walks the rows as PickWalk does. Each pick narrows the word bags and inner
// dictionary groups that fit what is left.
class RowWalker : public PickWalk {
  public:
    RowWalker(std::vector<std::vector<PairBag> const*> const& steps,
              int pair_steps, size_t pair_bags,
              std::vector<PairsCounts> const& word_bags,
              RestCounter rest_counter, RestTable const& rest_table)
        : PickWalk(steps, pair_steps, pair_bags), word_bags_(word_bags),
          fitting_(steps.size()), rest_counter_(std::move(rest_counter)),
          rest_table_(rest_table) {}

    // Calls visit(unit, row, pair_length) for each row of units [first,
    // last), in order, with the row's counts but not its PAIR or PAIRS.
    template <typename Visit>
    void walk(size_t first, size_t last, PairsCounts const& bag,
              Visit const& visit) {
        size_t const last_step = fitting_.size() - 1;
        auto const enter = [this](size_t step, PairsCounts const& rest) {
            filter(step, rest);
        };
        for (size_t unit = first; unit < last; ++unit) {
            PickWalk::walk(unit, bag, enter,
                           [&](PairsCounts const& rest, int letters) {
                BagStatsRow row{};
                ++rest_counts_.lookups;
                if (rest_table_.get(rest, &row)) {
                    ++rest_counts_.hits;
                } else {
                    filter(last_step, rest);
                    if (rest_counter_.needs_groups())
                        rest_counter_.groups().hash(last_step);
                    rest_counter_.count(rest, fitting_[last_step], &row);
                    rest_table_.put(rest, row);
                }
                visit(unit, row, size_t(letters - letter_total(rest)));
            });
        }
    }

    // The RestTable lookups since the last call.
    CacheCounts take_rest_counts() {
        CacheCounts const out = rest_counts_;
        rest_counts_ = {};
        return out;
    }

    // The PairCountCache lookups since the last call.
    CacheCounts take_pair_cache_counts() {
        return rest_counter_.take_pair_cache_counts();
    }

  private:
    void filter(size_t step, PairsCounts const& rest) {
        filter_word_bags(step == 0 ? word_bags_ : fitting_[step - 1],
                          rest, &fitting_[step]);
        if (rest_counter_.needs_groups())
            rest_counter_.groups().filter(step, rest);
    }

    std::vector<PairsCounts> const& word_bags_;
    // The word bags that fit what is left after each pick.
    std::vector<std::vector<PairsCounts>> fitting_;
    RestCounter rest_counter_;
    RestTable const& rest_table_;
    CacheCounts rest_counts_;
};

// Progress hooks: the cache and pair cache lookups of each finished block.
class CacheHooks {
  public:
    // `pair_cache`: null without one.
    CacheHooks(RestTable const& rest_table, PairCountCache const* pair_cache)
        : rest_table_(rest_table), pair_cache_(pair_cache) {}

    void finished(RowWalker* walker) {
        cache_.add(walker->take_rest_counts());
        pair_cache_counts_.add(walker->take_pair_cache_counts());
    }

    // ", cache off" or ", cache hits N/M (P%)", then the pair cache's hits
    // when it is on.
    std::string text() const {
        std::string out = rest_table_.enabled()
            ? cache_.text("cache") : ", cache off";
        if (pair_cache_ && pair_cache_->enabled())
            out += pair_cache_counts_.text("pair cache");
        return out;
    }

  private:
    // CacheCounts summed across workers.
    struct SharedCacheCounts {
        std::atomic<uint64_t> hits{0};
        std::atomic<uint64_t> lookups{0};

        void add(CacheCounts const& counts) {
            hits.fetch_add(counts.hits, std::memory_order_relaxed);
            lookups.fetch_add(counts.lookups, std::memory_order_relaxed);
        }

        // ", NAME hits N/M (P%)".
        std::string text(char const* name) const {
            uint64_t const h = hits.load(std::memory_order_relaxed);
            uint64_t const l = lookups.load(std::memory_order_relaxed);
            char out[96];
            snprintf(out, sizeof out, ", %s hits %llu/%llu (%.1f%%)", name,
                     static_cast<unsigned long long>(h),
                     static_cast<unsigned long long>(l),
                     l == 0 ? 0.0 : 100.0 * double(h) / double(l));
            return out;
        }
    };

    RestTable const& rest_table_;
    PairCountCache const* pair_cache_;
    SharedCacheCounts cache_;
    SharedCacheCounts pair_cache_counts_;
};

struct Walk : WalkBase {
    InnerCounters const& inner;
    // With a last p, the DICT pair counts; with pf, the PAIRS bags; the
    // other null.
    PairCountTable const* table;
    FinalPairBags const* final_bags;
    RestTable const& rest_table;
    // With --completions or --all, each of word_bags' DICT spellings, and
    // with a last pf each final bag's rows.
    std::vector<std::vector<std::string>> const& spellings;
    std::vector<std::vector<std::string>> const& final_rows;
    int completions;
    bool all;

    RowWalker walker() const {
        return RowWalker(steps, args.search.pairs, pair_bags.size(),
                         word_bags,
                         RestCounter(args.search.words, table, final_bags,
                                     inner.groups(steps.size())),
                         rest_table);
    }

    CacheHooks progress_hooks() const {
        return CacheHooks(rest_table, table ? &table->cache() : nullptr);
    }

    // Prints the row whose picks are `picks`, counted from what they leave
    // alone, then its --completions or --all.
    void print_score_row(std::vector<size_t> const& picks) const;

    // Prints --completions or --all for the row whose picks are `picks`.
    void after_score(std::vector<size_t> const& picks) const;
};

bool completion_before(BagStatsCompletion const& a,
                       BagStatsCompletion const& b) {
    return a.pairs != b.pairs ? a.pairs > b.pairs : a.seq < b.seq;
}

// Whether a heap of the `top` best completions takes `completion`.
bool completion_wanted(std::vector<BagStatsCompletion> const& heap, size_t top,
                       BagStatsCompletion const& completion) {
    return heap.size() < top || completion_before(completion, heap.front());
}

// Adds `completion`, which completion_wanted takes, to a heap of the `top`
// best completions, or with `top` SIZE_MAX to the unordered list of all of
// them.
void push_completion(std::vector<BagStatsCompletion>* heap, size_t top,
                     BagStatsCompletion completion) {
    if (top == SIZE_MAX) {
        heap->push_back(std::move(completion));
        return;
    }
    if (heap->size() == top) {
        std::pop_heap(heap->begin(), heap->end(), completion_before);
        heap->pop_back();
    }
    heap->push_back(std::move(completion));
    std::push_heap(heap->begin(), heap->end(), completion_before);
}

// Prints `top`, best first, of `total` completions, their pairs labeled
// `key`.
void print_completion_list(
        std::vector<BagStatsCompletion> const& top, uint64_t total,
        std::vector<std::vector<std::string>> const& spellings,
        char const* key = "PC") {
    printf("completions %zu of %llu by %s\n", top.size(),
           (unsigned long long)total, key);
    int const width = top.empty()
        ? 1 : int(std::to_string(top.front().pairs).size());
    std::string words;
    for (BagStatsCompletion const& completion : top) {
        words.clear();
        for (uint32_t word_bag : completion.word_bags) {
            if (!words.empty()) words += " + ";
            std::vector<std::string> const& word_spellings =
                spellings[word_bag];
            for (size_t k = 0; k < word_spellings.size(); ++k) {
                if (k > 0) words += '/';
                words += word_spellings[k];
            }
        }
        printf("%s %*llu %-4s %s\n", key, width,
               (unsigned long long)completion.pairs,
               completion.solo ? "SOLO" : "",
               words.c_str());
    }
}

// One thread's share of a remainder's `top` completions with the most
// pairs, counted as RestCounter counts them, of `fitting`, the word bags
// that fit it, whose indices in the walk's word bags are `indices`; with
// `with_pairs`, only those with pairs, or SOLO.
class CompletionHeap : public CompletionSink {
  public:
    CompletionHeap(std::vector<PairsCounts> const& fitting,
                   std::vector<uint32_t> const& indices, size_t top,
                   bool with_pairs)
        : fitting_(fitting), indices_(indices), top_(top),
          with_pairs_(with_pairs) {}

    // Numbers the completions that follow from `seq`, in walk order.
    void start(uint64_t seq) { seq_ = seq; }

    void add(std::vector<PairsCounts const*> const& chosen,
             PairsCounts const& rest, uint64_t pairs) override {
        bool const solo = rest == PairsCounts{};
        ++total_;
        BagStatsCompletion completion{{}, pairs, seq_++, solo};
        if ((with_pairs_ && pairs == 0 && !solo) ||
            !completion_wanted(heap_, top_, completion))
            return;
        for (PairsCounts const* word_bag : chosen)
            completion.word_bags.push_back(
                indices_[size_t(word_bag - fitting_.data())]);
        push_completion(&heap_, top_, std::move(completion));
    }

    void add_final(size_t bag, uint64_t pairs) override {
        ++total_;
        BagStatsCompletion completion{{uint32_t(bag)}, pairs, seq_++, false};
        if (completion_wanted(heap_, top_, completion))
            push_completion(&heap_, top_, std::move(completion));
    }

    std::vector<BagStatsCompletion>& heap() { return heap_; }
    uint64_t total() const { return total_; }

  private:
    std::vector<PairsCounts> const& fitting_;
    std::vector<uint32_t> const& indices_;
    size_t top_;
    bool with_pairs_;
    std::vector<BagStatsCompletion> heap_;
    uint64_t seq_ = 0;
    uint64_t total_ = 0;
};

// A remainder's counts, as RestCounter counts them, and with `top` > 0 its
// `top` completions with the most pairs, best first, ties in DICT order, of
// `total`.
struct RemainderCount {
    BagStatsCounts counts{};
    std::vector<BagStatsCompletion> best;
    uint64_t total = 0;
};

// One thread's share of count_remainder, on cache lines of its own.
struct alignas(64) RemainderPart {
    BagStatsCounts counts;
    CompletionHeap heap;
};

// Counts `rest` on the walk's threads, split by first word bag; with `top`
// and `with_pairs` as CompletionHeap takes them. Once `stop` is requested,
// quits early, leaving the counts partial.
RemainderCount count_remainder(Walk const& walk, PairsCounts const& rest,
                               size_t top, bool with_pairs,
                               std::stop_token const& stop = {}) {
    std::vector<PairsCounts> fitting;
    std::vector<uint32_t> indices;
    for (size_t i = 0; i < walk.word_bags.size(); ++i) {
        if (!pairs_fits(rest, walk.word_bags[i])) continue;
        fitting.push_back(walk.word_bags[i]);
        indices.push_back(uint32_t(i));
    }
    int const words = walk.args.search.words;
    size_t const threads = words == 0
        ? 1 : std::max<size_t>(1, std::min(walk.threads, fitting.size()));
    std::vector<RemainderPart> parts;
    for (size_t t = 0; t < threads; ++t)
        parts.push_back({{}, {fitting, indices, top, with_pairs}});
    std::unique_ptr<InnerGroups> const groups = walk.inner.groups(1);
    if (walk.table && walk.table->needs_groups()) {
        groups->filter(0, rest);
        groups->hash(0);
    }
    std::atomic<size_t> next{0};
    run_threads(threads, [&](size_t thread) {
        RemainderPart& part = parts[thread];
        RestCounter counter(words, walk.table, walk.final_bags,
                            groups->view());
        CompletionSink* const sink = top > 0 ? &part.heap : nullptr;
        if (words == 0) {
            counter.count(rest, fitting, &part.counts, sink);
            return;
        }
        for (size_t i; !stop.stop_requested() &&
                       (i = next.fetch_add(1)) < fitting.size();) {
            part.heap.start(uint64_t(i) << 40);
            counter.count_first(rest, fitting, i, &part.counts, sink);
        }
    });
    RemainderCount out;
    for (RemainderPart& part : parts) {
        out.counts.add(part.counts);
        out.total += part.heap.total();
        std::vector<BagStatsCompletion>& heap = part.heap.heap();
        std::move(heap.begin(), heap.end(), std::back_inserter(out.best));
    }
    std::sort(out.best.begin(), out.best.end(), completion_before);
    if (out.best.size() > top) out.best.resize(top);
    return out;
}

// The completions --completions or --all lists.
size_t listed_completions(Walk const& walk) {
    return walk.all ? SIZE_MAX : size_t(walk.completions);
}

// Prints `count`'s completions as --completions and --all list them.
void print_completions(Walk const& walk, RemainderCount const& count) {
    print_completion_list(count.best, count.total,
                          walk.args.search.words > 0 ? walk.spellings
                                                     : walk.final_rows);
}

// A row of `counts`, without its PAIR or PAIRS.
BagStatsRow counts_row(BagStatsCounts const& counts) {
    BagStatsRow row{};
    static_cast<BagStatsCounts&>(row) = counts;
    return row;
}

void Walk::print_score_row(std::vector<size_t> const& picks) const {
    RemainderCount const count = count_remainder(
        *this, picks_rest(*this, picks), listed_completions(*this), all);
    BagStatsRow row = counts_row(count.counts);
    name_picks(*this, picks, &row);
    write_bag_stats_row(stdout, row);
    if (listed_completions(*this) != 0) print_completions(*this, count);
}

void Walk::after_score(std::vector<size_t> const& picks) const {
    if (listed_completions(*this) == 0) return;
    print_completions(*this, count_remainder(*this, picks_rest(*this, picks),
                                             listed_completions(*this), all));
}

// Sets up the counting for `options` over `inputs` and calls f(walk,
// rankers), returning what it returns, or 1 when the pair count table can't
// be allocated.
template <typename F>
int with_walk(BagStatsOptions const& options, std::string const& letters,
              BagStatsInputs const& inputs,
              std::vector<std::vector<std::string>> const& spellings,
              F const& f) {
    BagStatsArgs const& common = options.common;
    PairsCounts const& bag = inputs.bag;
    std::vector<std::vector<PairBag> const*> const& steps = inputs.steps;
    PairsExactCounter const& counter = inputs.counter;
    size_t const threads = bag_stats_threads(common);
    RankArgs const& rank = common.rank;
    bool const one_row = !rank.score.empty() && rank.sorts.empty();

    std::optional<PairCountTable> table;
    std::optional<FinalPairBags> final_bags;
    bool pair_cache_wanted = false;
    if (common.search.pair_filter) {
        final_bags.emplace(inputs.final_bags, options.exact);
    } else {
        size_t const longest = longest_leftover(bag, steps, inputs.word_bags,
                                                common.search.words);
        SubBagIndex index = pair_count_index(letters, longest);
        // Only leftovers longer than the index reach the pair cache.
        pair_cache_wanted =
            options.pair_cache_size_mb != 0 && index.max_length() < longest;
        table.emplace(
            std::move(index), bag, counter, steps.size() > 1 && !one_row,
            threads,
            pair_cache_wanted ? size_t(options.pair_cache_size_mb) : 0);
        if (!table->allocated()) {
            fprintf(stderr, "%s: can't allocate the pair count table\n",
                    PROGRAM);
            return 1;
        }
    }

    BagStatsRankers rankers(common, inputs);
    bool const passes = rankers.passes;
    RestTable const rest_table(bag,
                               (steps.size() > 1 || passes) && !one_row,
                               size_t(options.cache_size_mb),
                               common.progress != 0);
    if (common.progress != 0 && rest_table.enabled())
        fprintf(stderr, "%s: cache holds %zu entries (%.1f MB) of %zu "
                "remainders\n", PROGRAM, rest_table.capacity(),
                double(rest_table.capacity() * REST_CACHE_ENTRY_BYTES) /
                    (1024 * 1024),
                rest_table.remainders());
    bool const needs_groups = table && table->needs_groups();
    if (table && common.progress != 0) {
        PairCountCache const& pair_cache = table->cache();
        if (pair_cache.enabled())
            fprintf(stderr, "%s: pair cache holds %zu entries (%.1f MB)\n",
                    PROGRAM, pair_cache.capacity(),
                    double(pair_cache.bytes()) / (1024 * 1024));
        else if (pair_cache_wanted && needs_groups)
            fprintf(stderr, "%s: pair cache off: %s\n", PROGRAM,
                    pair_cache.too_many_sub_bags()
                        ? "the letters have too many sub-bags to key"
                        : "can't allocate it");
    }
    InnerCounters const inner(counter, needs_groups);
    if (common.progress != 0 && needs_groups) {
        if (inner.key_bits() != 0)
            fprintf(stderr, "%s: pair counts use %d-bit keys, %d bits "
                    "packed\n", PROGRAM, inner.key_bits(), inner.bits());
        else
            fprintf(stderr, "%s: pair counts use unpacked keys, %d bits, "
                    "past 128\n", PROGRAM, inner.bits());
    }
    Walk const walk{{PROGRAM, steps, inputs.pair_bags, inputs.prospect_bags,
                     bag, inputs.word_bags, common, threads, passes ? 2 : 1,
                     unit_blocks(walk_units(steps))},
                    inner, table ? &*table : nullptr,
                    final_bags ? &*final_bags : nullptr, rest_table,
                    spellings, inputs.final_rows, options.completions,
                    options.all};
    return f(walk, &rankers);
}

}  // namespace

bool list_bag_stats(BagStatsOptions const& options,
                    std::string const& letters, BagStatsInputs const& inputs,
                    std::stop_token stop,
                    std::vector<BagStatsCompletion>* out) {
    out->clear();
    std::vector<std::vector<std::string>> const spellings;
    return with_walk(options, letters, inputs, spellings,
              [&](Walk const& walk, BagStatsRankers*) {
        if (walk.steps.empty()) {
            *out = count_remainder(walk, walk.bag, SIZE_MAX, true, stop).best;
            return 0;
        }
        std::atomic<size_t> next{0};
        std::vector<std::vector<BagStatsCompletion>> parts(walk.threads);
        run_threads(walk.threads, [&](size_t thread) {
            RowWalker walker = walk.walker();
            for (size_t unit; !stop.stop_requested() &&
                              (unit = next.fetch_add(1)) < walk.units();) {
                uint64_t seq = uint64_t(unit) << 32;
                walker.walk(unit, unit + 1, walk.bag,
                            [&](size_t, BagStatsRow const& row, size_t) {
                    uint64_t const order = seq++;
                    if (row.total_pairs == 0 && row.solo == 0) return;
                    BagStatsCompletion completion{
                        {}, row.total_pairs, order, row.total_pairs == 0};
                    walker.picks(&completion.word_bags);
                    parts[thread].push_back(std::move(completion));
                });
            }
        });
        for (std::vector<BagStatsCompletion>& part : parts)
            std::move(part.begin(), part.end(), std::back_inserter(*out));
        std::sort(out->begin(), out->end(), completion_before);
        return 0;
    }) == 0;
}

int bag_stats(BagStatsOptions const& options, std::string const& letters) {
    BagStatsArgs const& common = options.common;
    BagStatsInputs inputs(pairs_letter_counts(letters.c_str()));
    std::vector<std::vector<std::string>> spellings;
    bool const listed = options.completions > 0 || options.all;
    if (!load_bag_stats_inputs(PROGRAM, common, &inputs,
                               listed ? &spellings : nullptr))
        return 1;
    return with_walk(options, letters, inputs, spellings,
                     [&](Walk const& walk, BagStatsRankers* rankers) {
        if (!walk.steps.empty()) return run_bag_stats(walk, letters, rankers);
        RemainderCount const count =
            count_remainder(walk, walk.bag, listed_completions(walk), true);
        write_walk_header(walk, letters);
        BagStatsRow row = counts_row(count.counts);
        row.pairs = 1;
        write_bag_stats_row(stdout, row);
        if (listed) print_completions(walk, count);
        return fflush(stdout) == 0 ? 0 : 1;
    });
}
