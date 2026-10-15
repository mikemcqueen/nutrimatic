// bag-stats.cpp - Rank combinations of pair letter bags by the exact word +
// pair completions they leave.

#include "bag-stats-impl.h"
#include "bag-stats-rank.h"
#include "bag-stats-run.h"
#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "mix-bits.h"
#include "optparse.h"
#include "pairs-impl.h"

#include "sub-bag-index.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr char PROGRAM[] = "bag-stats";
constexpr int OPT_CACHE_SIZE = OPT_COMMON_END;
constexpr int OPT_PAIR_CACHE_SIZE = OPT_COMMON_END + 1;
constexpr int OPT_COMPLETIONS = OPT_COMMON_END + 2;
constexpr int DEFAULT_CACHE_SIZE_MB = 1000;
constexpr int DEFAULT_PAIR_CACHE_SIZE_MB = 1000;
// The most sub-bags PairCountTable numbers, 4 bytes each.
constexpr size_t PAIR_COUNT_TABLE_MAX = size_t(1) << 28;

struct Args {
    BagStatsArgs common;
    int cache_size_mb = DEFAULT_CACHE_SIZE_MB;
    int pair_cache_size_mb = DEFAULT_PAIR_CACHE_SIZE_MB;
    int completions = 0;
};

void usage(char const* program, FILE* out) {
    fprintf(out,
            "usage: %s [-u LETTERS] [-s N] [--min-wb-len N] [--ss STEPS] [--pp FILE]\n"
            "       [--threads N] [--cache-size N] [--pair-cache-size N]\n"
            "       [--progress N]\n"
            "       [-n N SORT... [--by-length | --by-pick]]\n"
            "       [--score W1,W2... [--score-detail] [--completions N]\n"
            "        [SORT... [--by-length | --by-pick]]]\n"
            "       [--best-pairs appear|lead]\n"
            "       [--prior FILE | [--p1 INDEX] [--clues CLUEDATA]\n"
            "        [--prior-weights SPEC]] [--top-segments]\n"
            "       LETTERS PAIRS DICT\n",
            program);
    if (out != stdout) return;

    help_common_options();
    dfs_help_option("--cache-size N",
                    "hold at most N MB of cached remainder counts, 0 for "
                    "none (default: %d)", DEFAULT_CACHE_SIZE_MB);
    dfs_help_option("--pair-cache-size N",
                    "hold at most N MB of cached pair counts for leftovers "
                    "too long for the pair count table, 0 for none "
                    "(default: %d)", DEFAULT_PAIR_CACHE_SIZE_MB);
    dfs_help_option("--completions N",
                    "with --score, below the rank print the row's N "
                    "completions with the most exact pairs, ties in DICT "
                    "order: each one's PC, MIN when its every word bag has "
                    "at least --min-wb-len letters and PC is at least %d "
                    "(MIN_WB_WITH_PAIRS counts it), SOLO when it leaves "
                    "nothing, and its word bags, each as its DICT "
                    "spellings; --score without SORT options prints the row "
                    "without its rank, counted from what its picks leave "
                    "alone", int(BAG_STATS_MIN_WB_PAIRS));
    help_common_tail();
}

bool parse_args(char* argv[], Args* out, bool* help) {
    static struct optparse_long const long_options[] = {
        BAG_STATS_COMMON_LONG_OPTIONS,
        { "cache-size", OPT_CACHE_SIZE, OPTPARSE_REQUIRED },
        { "pair-cache-size", OPT_PAIR_CACHE_SIZE, OPTPARSE_REQUIRED },
        { "completions", OPT_COMPLETIONS, OPTPARSE_REQUIRED },
        { "help", 'h', OPTPARSE_NONE },
        { NULL, 0, OPTPARSE_NONE },
    };

    struct optparse options;
    optparse_init(&options, argv);

    int option;
    while ((option = optparse_long(&options, long_options, NULL)) != -1) {
        RankOption const common = parse_common_option(
            PROGRAM, option, options.optarg, &out->common);
        if (common == RankOption::BAD) return false;
        if (common == RankOption::PARSED) continue;
        switch (option) {
          case OPT_CACHE_SIZE:
            if (!parse_count(options.optarg, "--cache-size",
                             &out->cache_size_mb))
                return false;
            break;
          case OPT_PAIR_CACHE_SIZE:
            if (!parse_count(options.optarg, "--pair-cache-size",
                             &out->pair_cache_size_mb))
                return false;
            break;
          case OPT_COMPLETIONS:
            if (!parse_count(options.optarg, "--completions",
                             &out->completions))
                return false;
            break;
          case 'h':
            *help = true;
            return true;
          default:
            fprintf(stderr, "error: %s\n", options.errmsg);
            return false;
        }
    }

    if (!check_common_args(PROGRAM, &options, &out->common)) return false;
    if (out->completions > 0 && out->common.rank.score.empty()) {
        fprintf(stderr, "%s: --completions goes with --score\n", PROGRAM);
        return false;
    }
    return true;
}


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
        hashed_.build(groups_[step]);
    }

    uint64_t count(PairsCounts const& bag) override {
        return counter_.count(bag, *hashed_groups_, hashed_);
    }

  private:
    PairsExactCounter const& counter_;
    std::vector<PairsExactCounter::Groups> groups_;
    PairsExactCounter::Groups const* hashed_groups_ = nullptr;
    RemainderGroups hashed_;
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
        hashed_.build(groups_[step]);
    }

    uint64_t count(PairsCounts const& bag) override {
        return counter_.count(bag, *hashed_groups_, hashed_, &scratch_);
    }

  private:
    PackedPairsCounter<Key> const& counter_;
    std::vector<PackedGroups<Key>> groups_;
    PackedGroups<Key> const* hashed_groups_ = nullptr;
    PackedRemainderGroups<Key> hashed_;
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

// Fills a remainder's counts over every combination of one word bag per
// word step, word bags in non-decreasing order.
class RestCounter {
  public:
    RestCounter(int words, int min_word_bag_length,
                PairCountTable const& table,
                std::unique_ptr<InnerGroups> groups)
        : words_(words), min_word_bag_length_(min_word_bag_length),
          table_(table), groups_(std::move(groups)) {}

    bool needs_groups() const { return table_.needs_groups(); }

    // The inner dictionary groups per pick step.
    InnerGroups& groups() { return *groups_; }

    // `fitting`: the word bags that fit `rest`; groups() step `step`: the
    // inner dictionary groups, holding every one that fits `rest`.
    void count(PairsCounts const& rest,
               std::vector<PairsCounts> const& fitting, size_t step,
               BagStatsCounts* out) {
        fitting_ = &fitting;
        if (needs_groups()) groups_->hash(step);
        visit(1, 0, rest, true, out);
    }

    // The PairCountCache lookups since the last call.
    CacheCounts take_pair_cache_counts() {
        CacheCounts const out = pair_cache_counts_;
        pair_cache_counts_ = {};
        return out;
    }

  private:
    void visit(int word, size_t start, PairsCounts const& rest, bool is_min,
               BagStatsCounts* out) {
        PairsCounts word_rest;
        std::vector<PairsCounts> const& fitting = *fitting_;
        for (size_t i = start; i < fitting.size(); ++i) {
            PairsCounts const& word_bag = fitting[i];
            if (!pairs_subtract(rest, word_bag, &word_rest)) continue;
            bool const min = is_min &&
                letter_total(word_bag) >= min_word_bag_length_;
            if (word < words_) {
                visit(word + 1, i, word_rest, min, out);
                continue;
            }
            ++out->word_bags;
            out->min_word_bags += min;
            if (word_rest == PairsCounts{}) {
                ++out->solo;
                continue;
            }
            if (letter_total(word_rest) < 2 * INNER_MIN_WORD_LENGTH) continue;
            uint64_t const count = table_.count(
                word_rest, [&] { return groups_->count(word_rest); },
                &pair_cache_counts_);
            if (count == 0) continue;
            ++out->with_pairs;
            out->total_pairs += count;
            out->max_pairs = std::max(out->max_pairs, count);
            out->min_with_pairs += min && count >= BAG_STATS_MIN_WB_PAIRS;
        }
    }

    int words_;
    int min_word_bag_length_;
    PairCountTable const& table_;
    std::vector<PairsCounts> const* fitting_ = nullptr;
    std::unique_ptr<InnerGroups> groups_;
    CacheCounts pair_cache_counts_;
};

// Walks the rows as PickWalk does. Each pick narrows the word bags and inner
// dictionary groups that fit what is left.
class RowWalker : public PickWalk {
  public:
    RowWalker(std::vector<std::vector<PairBag> const*> const& steps,
              int pair_steps, std::vector<PairsCounts> const& word_bags,
              RestCounter rest_counter, RestTable const& rest_table)
        : PickWalk(steps, pair_steps), word_bags_(word_bags),
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
                    rest_counter_.count(rest, fitting_[last_step], last_step,
                                        &row);
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
    CacheHooks(RestTable const& rest_table, PairCountCache const& pair_cache)
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
        if (pair_cache_.enabled()) out += pair_cache_counts_.text("pair cache");
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
    PairCountCache const& pair_cache_;
    SharedCacheCounts cache_;
    SharedCacheCounts pair_cache_counts_;
};

struct Walk : WalkBase {
    InnerCounters const& inner;
    PairCountTable const& table;
    RestTable const& rest_table;
    PairsExactCounter const& counter;
    // With --completions, each of word_bags' DICT spellings.
    std::vector<std::vector<std::string>> const& spellings;
    int completions;

    RowWalker walker() const {
        return RowWalker(steps, args.search.pairs, word_bags,
                         RestCounter(args.search.words,
                                     args.min_word_bag_length, table,
                                     inner.groups(steps.size())),
                         rest_table);
    }

    CacheHooks progress_hooks() const {
        return CacheHooks(rest_table, table.cache());
    }

    // The row whose picks are `picks`, counted from what they leave alone.
    BagStatsRow score_row(std::vector<size_t> const& picks) const;

    // Prints --completions for the --score row whose picks are `picks`.
    void after_score(std::vector<size_t> const& picks) const;
};

// One combination of one word bag per word step that fits a remainder.
struct Completion {
    // Indices into Walk::word_bags, non-decreasing.
    std::vector<uint32_t> word_bags;
    uint64_t pairs;
    uint64_t seq;
    bool min;
    bool solo;
};

bool completion_before(Completion const& a, Completion const& b) {
    return a.pairs != b.pairs ? a.pairs > b.pairs : a.seq < b.seq;
}

// A remainder's `top` completions with the most exact pairs, counted as
// RestCounter counts them.
class CompletionCollector {
  public:
    CompletionCollector(Walk const& walk, PairsCounts const& rest, size_t top)
        : walk_(walk), top_(top) {
        for (size_t i = 0; i < walk.word_bags.size(); ++i)
            if (pairs_fits(rest, walk.word_bags[i]))
                fitting_.push_back(uint32_t(i));
        walk.counter.filter(walk.counter.groups_by_length(), rest, &groups_);
        hashed_.build(groups_);
        visit(1, 0, rest, true);
        std::sort(heap_.begin(), heap_.end(), completion_before);
    }

    // The completions, best first.
    std::vector<Completion> const& top() const { return heap_; }

    // Every completion of the remainder.
    uint64_t total() const { return seq_; }

  private:
    void visit(int word, size_t start, PairsCounts const& rest, bool is_min) {
        PairsCounts word_rest;
        for (size_t i = start; i < fitting_.size(); ++i) {
            PairsCounts const& word_bag = walk_.word_bags[fitting_[i]];
            if (!pairs_subtract(rest, word_bag, &word_rest)) continue;
            bool const min = is_min &&
                letter_total(word_bag) >= walk_.args.min_word_bag_length;
            chosen_.push_back(fitting_[i]);
            if (word < walk_.args.search.words)
                visit(word + 1, i, word_rest, min);
            else
                add(word_rest, min);
            chosen_.pop_back();
        }
    }

    void add(PairsCounts const& word_rest, bool min) {
        bool const solo = word_rest == PairsCounts{};
        uint64_t const count =
            solo || letter_total(word_rest) < 2 * INNER_MIN_WORD_LENGTH
                ? 0 : walk_.counter.count(word_rest, groups_, hashed_);
        Completion completion{{}, count, seq_++,
                              min && count >= BAG_STATS_MIN_WB_PAIRS, solo};
        if (heap_.size() == top_ &&
            !completion_before(completion, heap_.front()))
            return;
        completion.word_bags = chosen_;
        if (heap_.size() == top_) {
            std::pop_heap(heap_.begin(), heap_.end(), completion_before);
            heap_.pop_back();
        }
        heap_.push_back(std::move(completion));
        std::push_heap(heap_.begin(), heap_.end(), completion_before);
    }

    Walk const& walk_;
    size_t top_;
    std::vector<uint32_t> fitting_;
    PairsExactCounter::Groups groups_;
    RemainderGroups hashed_;
    std::vector<uint32_t> chosen_;
    std::vector<Completion> heap_;
    uint64_t seq_ = 0;
};

BagStatsRow Walk::score_row(std::vector<size_t> const& picks) const {
    Walk const& walk = *this;
    PairsCounts const rest = picks_rest(walk, picks);
    std::vector<PairsCounts> fitting;
    filter_word_bags(walk.word_bags, rest, &fitting);
    RestCounter counter(walk.args.search.words, walk.args.min_word_bag_length,
                        walk.table, walk.inner.groups(1));
    if (counter.needs_groups()) counter.groups().filter(0, rest);
    BagStatsRow row{};
    counter.count(rest, fitting, 0, &row);
    name_picks(walk, picks, &row);
    return row;
}

// Prints --completions for the row whose picks are `picks`.
void print_completions(Walk const& walk, std::vector<size_t> const& picks) {
    PairsCounts const rest = picks_rest(walk, picks);
    CompletionCollector const collector(walk, rest,
                                        size_t(walk.completions));
    std::vector<Completion> const& top = collector.top();
    printf("completions %zu of %llu by PC\n", top.size(),
           (unsigned long long)collector.total());
    int const width = top.empty()
        ? 1 : int(std::to_string(top.front().pairs).size());
    std::string words;
    for (Completion const& completion : top) {
        words.clear();
        for (uint32_t word_bag : completion.word_bags) {
            if (!words.empty()) words += " + ";
            std::vector<std::string> const& spellings =
                walk.spellings[word_bag];
            for (size_t k = 0; k < spellings.size(); ++k) {
                if (k > 0) words += '/';
                words += spellings[k];
            }
        }
        printf("PC %*llu %-4s %s\n", width,
               (unsigned long long)completion.pairs,
               completion.min ? "MIN" : completion.solo ? "SOLO" : "",
               words.c_str());
    }
}

void Walk::after_score(std::vector<size_t> const& picks) const {
    if (completions > 0) print_completions(*this, picks);
}

}  // namespace

int main(int argc, char* argv[]) {
    Args args;
    bool help = false;
    std::string letters;
    if (!parse_args(argv, &args, &help) ||
        (!help && !remaining_letters(args.common, &letters))) {
        usage(argv[0], stderr);
        return 2;
    }
    if (help) {
        usage(argv[0], stdout);
        return 0;
    }

    BagStatsArgs const& common = args.common;
    BagStatsInputs inputs(pairs_letter_counts(letters.c_str()));
    std::vector<std::vector<std::string>> spellings;
    if (!load_bag_stats_inputs(PROGRAM, common, &inputs,
                               args.completions > 0 ? &spellings : nullptr))
        return 1;
    PairsCounts const& bag = inputs.bag;
    std::vector<std::vector<PairBag> const*> const& steps = inputs.steps;
    PairsExactCounter const& counter = inputs.counter;
    size_t const threads = bag_stats_threads(common);
    size_t const longest = longest_leftover(bag, steps, inputs.word_bags,
                                            common.search.words);
    SubBagIndex index = pair_count_index(letters, longest);
    // Only leftovers longer than the index reach the pair cache.
    bool const pair_cache_wanted =
        args.pair_cache_size_mb != 0 && index.max_length() < longest;
    RankArgs const& rank = common.rank;
    bool const one_row = !rank.score.empty() && rank.sorts.empty();
    PairCountTable const table(
        std::move(index), bag, counter, steps.size() > 1 && !one_row,
        threads,
        pair_cache_wanted ? size_t(args.pair_cache_size_mb) : 0);
    if (!table.allocated()) {
        fprintf(stderr, "%s: can't allocate the pair count table\n", PROGRAM);
        return 1;
    }

    BagStatsRankers rankers(common, inputs);
    bool const passes = rankers.passes;
    RestTable const rest_table(bag,
                               (steps.size() > 1 || passes) && !one_row,
                               size_t(args.cache_size_mb),
                               common.progress != 0);
    if (common.progress != 0 && rest_table.enabled())
        fprintf(stderr, "%s: cache holds %zu entries (%.1f MB) of %zu "
                "remainders\n", PROGRAM, rest_table.capacity(),
                double(rest_table.capacity() * REST_CACHE_ENTRY_BYTES) /
                    (1024 * 1024),
                rest_table.remainders());
    PairCountCache const& pair_cache = table.cache();
    if (common.progress != 0 && pair_cache.enabled())
        fprintf(stderr, "%s: pair cache holds %zu entries (%.1f MB)\n",
                PROGRAM, pair_cache.capacity(),
                double(pair_cache.bytes()) / (1024 * 1024));
    else if (common.progress != 0 && pair_cache_wanted && table.needs_groups())
        fprintf(stderr, "%s: pair cache off: %s\n", PROGRAM,
                pair_cache.too_many_sub_bags()
                    ? "the letters have too many sub-bags to key"
                    : "can't allocate it");
    InnerCounters const inner(counter, table.needs_groups());
    if (common.progress != 0 && table.needs_groups()) {
        if (inner.key_bits() != 0)
            fprintf(stderr, "%s: pair counts use %d-bit keys, %d bits "
                    "packed\n", PROGRAM, inner.key_bits(), inner.bits());
        else
            fprintf(stderr, "%s: pair counts use unpacked keys, %d bits, "
                    "past 128\n", PROGRAM, inner.bits());
    }
    Walk const walk{{PROGRAM, steps, inputs.prospect_bags, bag,
                     inputs.word_bags, common, threads, passes ? 2 : 1,
                     unit_blocks(steps[0]->size())},
                    inner, table, rest_table, counter, spellings,
                    args.completions};
    return run_bag_stats(walk, letters, &rankers);
}
