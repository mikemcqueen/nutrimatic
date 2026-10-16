#ifndef NUTRIMATIC_BAG_STATS_CPU_H
#define NUTRIMATIC_BAG_STATS_CPU_H

// bag-stats on the CPU: what the bag-stats command line runs, and the --ss pf
// count over bags already loaded, which pgui runs too.

#include <stdint.h>

#include <algorithm>
#include <string>
#include <vector>

#include "bag-stats-rank.h"
#include "bag-stats-run.h"
#include "pairs-impl.h"

inline constexpr int BAG_STATS_DEFAULT_CACHE_SIZE_MB = 1000;
inline constexpr int BAG_STATS_DEFAULT_PAIR_CACHE_SIZE_MB = 1000;

// What --completions and --all sort by and show (--sort): total pairs,
// counted once for each way of reaching them, or unique pairs, counted once.
enum class PairFilterSort { TP, UP };

// How bag-stats runs: the options it shares with bag-stats-cuda; at most
// cache_size_mb MB of cached remainder counts and pair_cache_size_mb MB of
// cached pair counts, 0 for none (--cache-size, --pair-cache-size); with
// --score or --ss pf, how many completions to list (--completions); and with
// --ss pf, whether to count only the PAIRS rows that use all of what's left
// (--exact), whether to list every completion with pairs, or SOLO, instead of
// the row (--all), and what the list sorts by (--sort).
struct BagStatsOptions {
    BagStatsArgs common;
    int cache_size_mb = BAG_STATS_DEFAULT_CACHE_SIZE_MB;
    int pair_cache_size_mb = BAG_STATS_DEFAULT_PAIR_CACHE_SIZE_MB;
    int completions = 0;
    bool exact = false;
    bool all = false;
    PairFilterSort sort = PairFilterSort::TP;
};

// One combination of one word bag per word step that fits a remainder, or
// with --ss pf one word bag or, with pb,wb,pf, one pair bag.
struct BagStatsCompletion {
    // Indices into the word bags, non-decreasing, or with --ss pf the one
    // index into the bags the count runs over.
    std::vector<uint32_t> word_bags;
    uint64_t pairs;
    uint64_t seq;
    bool solo;
};

// The --ss pf row: W_ columns count DICT words, WB_ columns word bags.
struct PairFilterCounts {
    uint64_t words = 0;
    uint64_t word_bags = 0;
    uint64_t words_with_pairs = 0;
    uint64_t word_bags_with_pairs = 0;
    uint64_t words_with_min_pairs = 0;
    uint64_t word_bags_with_min_pairs = 0;
    uint64_t words_solo = 0;
    uint64_t word_bags_solo = 0;
    uint64_t total_pairs = 0;
    uint64_t total_pair_bags = 0;
    uint64_t max_pairs = 0;
    uint64_t max_pair_bags = 0;
    uint64_t total_unique_pairs = 0;
    uint64_t max_unique_pairs = 0;

    void add(PairFilterCounts const& other) {
        words += other.words;
        word_bags += other.word_bags;
        words_with_pairs += other.words_with_pairs;
        word_bags_with_pairs += other.word_bags_with_pairs;
        words_with_min_pairs += other.words_with_min_pairs;
        word_bags_with_min_pairs += other.word_bags_with_min_pairs;
        words_solo += other.words_solo;
        word_bags_solo += other.word_bags_solo;
        total_pairs += other.total_pairs;
        total_pair_bags += other.total_pair_bags;
        max_pairs = std::max(max_pairs, other.max_pairs);
        max_pair_bags = std::max(max_pair_bags, other.max_pair_bags);
        total_unique_pairs += other.total_unique_pairs;
        max_unique_pairs = std::max(max_unique_pairs, other.max_unique_pairs);
    }
};

// The --ss pf row and the bags with the most pairs.
struct PairFilterResult {
    PairFilterCounts counts;
    // The `top` bags with the most pairs, best first, ties in bag order, less
    // those with no pairs that aren't SOLO; each completion is one bag.
    std::vector<BagStatsCompletion> best;
};

// The bags a --ss pf count runs over, loaded by the caller: the pair bags and
// word bags, and with pair_step (pb,wb,pf) each pair bag's rows, otherwise
// each word bag's spellings.
struct PairFilterInputs {
    explicit PairFilterInputs(bool pair_step) : pair_step(pair_step) {}

    // Where to load the pair bags' rows, null when they aren't wanted.
    std::vector<std::vector<std::string>>* pair_rows_out() {
        return pair_step ? &pair_rows : nullptr;
    }

    // Where to load the word bags' spellings, null when they aren't wanted.
    std::vector<std::vector<std::string>>* word_spellings_out() {
        return pair_step ? nullptr : &word_spellings;
    }

    // The spellings of each bag the count runs over.
    std::vector<std::vector<std::string>> const& spellings() const {
        return pair_step ? pair_rows : word_spellings;
    }

    bool pair_step;
    std::vector<PairBag> pair_bags;
    std::vector<std::vector<std::string>> pair_rows;
    std::vector<PairsCounts> word_bags;
    std::vector<std::vector<std::string>> word_spellings;
};

// Counts, on `threads` threads, for each word bag of `inputs`, or with
// pair_step each pair bag, what it leaves of `bag` after each sequence of
// `words` word bags (with pair_step, one more), each fitting what the ones
// before it leave, and the pair bags that fit what that leaves, or with
// `exact` the one that uses all of it, and their pairs. The best `top` with
// pairs, or SOLO, go by `sort`. Word steps need `exact`.
PairFilterResult count_pair_filter(
    PairsCounts const& bag, PairFilterInputs const& inputs, bool exact,
    int words, PairFilterSort sort, size_t threads, size_t top);

// Runs bag-stats with `options` on `letters`, its LETTERS less its used
// letters, printing to standard output. Returns the exit status; errors are
// diagnosed.
int bag_stats(BagStatsOptions const& options, std::string const& letters);

#endif
