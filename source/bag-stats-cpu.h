#ifndef NUTRIMATIC_BAG_STATS_CPU_H
#define NUTRIMATIC_BAG_STATS_CPU_H

// bag-stats on the CPU: what the bag-stats command line runs, and the listing
// over bags already loaded that pgui runs.

#include <stdint.h>

#include <algorithm>
#include <stop_token>
#include <string>
#include <vector>

#include "bag-stats-rank.h"
#include "bag-stats-run.h"
#include "pairs-impl.h"

inline constexpr int BAG_STATS_DEFAULT_PAIR_CACHE_SIZE_MB = 1000;

// How bag-stats runs: the options it shares with bag-stats-cuda; at most
// pair_cache_size_mb MB of cached pair counts, 0 for none
// (--pair-cache-size); with
// --score or without pick steps, how many completions to list
// (--completions), or whether to list every one with pairs, or SOLO (--all);
// and with a last pf, whether it counts only the PAIRS rows that use all of
// what's left (--exact).
struct BagStatsOptions {
    BagStatsArgs common;
    int pair_cache_size_mb = BAG_STATS_DEFAULT_PAIR_CACHE_SIZE_MB;
    int completions = 0;
    bool exact = false;
    bool all = false;
};

// One combination of one word bag per word step that fits a remainder; with
// a last pf and no word steps, one final bag the pf reaches; or, listed for
// pgui with pick steps, one row.
struct BagStatsCompletion {
    // Indices into the word bags, non-decreasing; or the one index into the
    // final bags; or the row's picks as PickWalk::picks() numbers them.
    std::vector<uint32_t> word_bags;
    uint64_t pairs;
    uint64_t seq;
    bool solo;
};

// What pgui's bag-stats column lists of `inputs`, loaded by the caller with
// its steps set, for `options` on `letters`: without pick steps, the one
// row's completions with pairs, or SOLO, best first, as --all lists them;
// with them, each row with pairs, or WB_SOLO, by TOTAL_PAIRS, ties in walk
// order, `solo` when it has only WB_SOLO. Once `stop` is requested, quits
// early, leaving the list partial. False, diagnosed, when the counting
// can't be set up.
bool list_bag_stats(BagStatsOptions const& options, std::string const& letters,
                    BagStatsInputs const& inputs, std::stop_token stop,
                    std::vector<BagStatsCompletion>* out);

// Runs bag-stats with `options` on `letters`, its LETTERS less its used
// letters, printing to standard output. Returns the exit status; errors are
// diagnosed.
int bag_stats(BagStatsOptions const& options, std::string const& letters);

#endif
