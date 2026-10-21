// bag-stats-cuda.cpp - bag-stats with every row's counts computed on the
// GPU.

#include "bag-stats-cache.h"
#include "bag-stats-gpu.h"
#include "bag-stats-impl.h"
#include "bag-stats-rank.h"
#include "bag-stats-run.h"
#include "log.h"
#include "optparse.h"
#include "pairs-impl.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr char PROGRAM[] = "bag-stats-cuda";

static_assert(sizeof(PairsCounts) == GPU_LETTERS_BYTES);
static_assert(sizeof(GpuCounts) == sizeof(BagStatsCounts));

void usage(std::string_view program, FILE* out) {
    fprintf(out,
            "usage: %.*s [-u LETTERS] [-s N | --no-nopairs] [--min-wb-len N]\n"
            "       [--min-pb-len N]\n"
            "       [--ss STEPS] [--pp FILE] [--threads N] [--cache-size N]\n"
            "       [--progress N]\n"
            "       [-n N SORT... [--by-length | --by-pick]]\n"
            "       [--score W1,W2... [--score-detail]\n"
            "        [SORT... [--by-length | --by-pick]]]\n"
            "       [--best-pairs appear|lead]\n"
            "       [--prior FILE | [--p1 INDEX] [--clues CLUEDATA]\n"
            "        [--prior-weights SPEC]] [--top-segments]\n"
            "       LETTERS PAIRS DICT\n",
            int(program.size()), program.data());
    if (out != stdout) return;

    fputs("  bag-stats, counting every row on the GPU\n\n", stdout);
    help_common_options(false);
    help_common_tail();
}

bool parse_args(char* argv[], BagStatsArgs* out, bool* help) {
    static struct optparse_long const long_options[] = {
        BAG_STATS_COMMON_LONG_OPTIONS,
        { "help", 'h', OPTPARSE_NONE },
        { NULL, 0, OPTPARSE_NONE },
    };

    struct optparse options;
    optparse_init(&options, argv);

    int option;
    while ((option = optparse_long(&options, long_options, NULL)) != -1) {
        RankOption const common =
            parse_common_option(PROGRAM, option, options.optarg, out);
        if (common == RankOption::BAD) return false;
        if (common == RankOption::PARSED) continue;
        switch (option) {
          case 'h':
            *help = true;
            return true;
          default:
            fprintf(stderr, "error: %s\n", options.errmsg);
            return false;
        }
    }

    if (!check_common_args(PROGRAM, &options, out)) return false;
    if (out->search.pair_filter) {
        report(PROGRAM, "a last pf is bag-stats only");
        return false;
    }
    if (out->search.prospects + out->search.pairs == 0 ||
        out->search.words == 0) {
        report(PROGRAM, "--ss needs a ppb or pb step and a wb step");
        return false;
    }
    if (out->search.words > GPU_MAX_WORDS) {
        report(PROGRAM, "--ss takes at most %d wb steps", GPU_MAX_WORDS);
        return false;
    }
    return true;
}

// The groups and word bags of `inputs`, as the device takes them.
GpuInputs gpu_inputs(BagStatsInputs const& inputs, BagStatsArgs const& args) {
    GpuInputs out;
    std::copy(inputs.bag.values.begin(), inputs.bag.values.end(), out.bag);
    PairsExactCounter const& counter = inputs.counter;
    PairsExactCounter::Groups const& groups = counter.groups_by_length();
    for (size_t i = 0; i < groups.keys.size(); ++i) {
        out.group_letters.insert(out.group_letters.end(),
                                 groups.keys[i].values.begin(),
                                 groups.keys[i].values.end());
        out.group_lengths.push_back(
            uint8_t(counter.group_length(groups.numbers[i])));
        out.group_sizes.push_back(counter.group_size(groups.numbers[i]));
    }
    for (PairsCounts const& word_bag : inputs.word_bags) {
        out.word_letters.insert(out.word_letters.end(),
                                word_bag.values.begin(),
                                word_bag.values.end());
        out.word_lengths.push_back(uint8_t(letter_total(word_bag)));
    }
    out.min_group_length = INNER_MIN_WORD_LENGTH;
    out.words = args.search.words;
    out.min_pairs = BAG_STATS_MIN_PAIRS;
    return out;
}

BagStatsCounts gpu_counts(GpuCounts const& counts) {
    BagStatsCounts out{};
    out.word_bags = counts.word_bags;
    out.with_pairs = counts.with_pairs;
    out.total_pairs = counts.total_pairs;
    out.max_pairs = counts.max_pairs;
    out.solo = counts.solo;
    out.with_min_pairs = counts.with_min_pairs;
    return out;
}

BagStatsRow counts_row(BagStatsCounts const& counts) {
    BagStatsRow row{};
    static_cast<BagStatsCounts&>(row) = counts;
    return row;
}

// What a walk does with the RestTable: nothing, put every row's counts in
// it, or look each rest up there first.
enum class RestMode { off, fill, lookup };

// Walks the rows as PickWalk does, buffering each row's picks and rest, and
// counts the buffer on the GPU when it holds GPU_BATCH_ROWS rows and at the
// end of a walk. Uses `rest_table` as `mode` says, counting only the
// lookup misses.
class GpuRowWalker : public PickWalk {
  public:
    GpuRowWalker(std::vector<std::vector<PairBag> const*> const& steps,
                 int pair_steps, size_t pair_bags,
                 GpuRowCounter const& counter, RestTable const& rest_table,
                 RestMode mode)
        : PickWalk(steps, pair_steps, pair_bags), counter_(counter),
          rest_table_(rest_table), mode_(mode) {}

    // The RestTable lookups since the last call.
    CacheCounts take_rest_counts() {
        CacheCounts const out = rest_counts_;
        rest_counts_ = {};
        return out;
    }

    // Calls visit(unit, row, pair_length) for each row of units [first,
    // last), in order, with the row's counts but not its PAIR or PAIRS.
    template <typename Visit>
    void walk(size_t first, size_t last, PairsCounts const& bag,
              Visit const& visit) {
        auto const enter = [](size_t, PairsCounts const&) {};
        for (size_t unit = first; unit < last; ++unit) {
            PickWalk::walk(unit, bag, enter,
                           [&](PairsCounts const& rest, int letters) {
                units_.push_back(unit);
                picks_.insert(picks_.end(), chosen_.begin(), chosen_.end());
                lengths_.push_back(size_t(letters - letter_total(rest)));
                rows_.emplace_back();
                if (!lookup(rest, &rows_.back())) {
                    misses_.push_back(rows_.size() - 1);
                    rests_.insert(rests_.end(), rest.values.begin(),
                                  rest.values.end());
                }
                if (units_.size() == GPU_BATCH_ROWS) flush(visit);
            });
        }
        flush(visit);
    }

  private:
    bool lookup(PairsCounts const& rest, BagStatsCounts* out) {
        if (mode_ != RestMode::lookup) return false;
        ++rest_counts_.lookups;
        if (!rest_table_.get(rest, out)) return false;
        ++rest_counts_.hits;
        return true;
    }

    template <typename Visit>
    void flush(Visit const& visit) {
        size_t const rows = units_.size();
        if (rows == 0) return;
        size_t const misses = misses_.size();
        if (misses != 0) {
            counts_.resize(misses);
            counter_.count(rests_.data(), misses, counts_.data());
            for (size_t m = 0; m < misses; ++m) {
                BagStatsCounts& row = rows_[misses_[m]];
                row = gpu_counts(counts_[m]);
                if (mode_ != RestMode::fill) continue;
                PairsCounts rest;
                std::copy_n(rests_.begin() + m * GPU_LETTERS_BYTES,
                            GPU_LETTERS_BYTES, rest.values.begin());
                rest_table_.put(rest, row);
            }
        }
        saved_ = chosen_;
        size_t const steps = chosen_.size();
        for (size_t r = 0; r < rows; ++r) {
            std::copy_n(picks_.begin() + r * steps, steps, chosen_.begin());
            BagStatsRow row = counts_row(rows_[r]);
            visit(units_[r], row, lengths_[r]);
        }
        chosen_ = saved_;
        units_.clear();
        picks_.clear();
        lengths_.clear();
        rows_.clear();
        misses_.clear();
        rests_.clear();
    }

    GpuRowCounter const& counter_;
    RestTable const& rest_table_;
    RestMode const mode_;
    CacheCounts rest_counts_;
    // The buffered rows: each one's unit, picks, pair letters and counts;
    // the misses' row indices and rests.
    std::vector<size_t> units_;
    std::vector<size_t> picks_;
    std::vector<size_t> lengths_;
    std::vector<BagStatsCounts> rows_;
    std::vector<size_t> misses_;
    std::vector<uint8_t> rests_;
    std::vector<GpuCounts> counts_;
    std::vector<size_t> saved_;
};

// Progress hooks: the cache lookups of each finished block.
class GpuCacheHooks {
  public:
    GpuCacheHooks(RestTable const& rest_table, int passes)
        : rest_table_(rest_table), passes_(passes) {}

    void finished(GpuRowWalker* walker) {
        cache_.add(walker->take_rest_counts());
    }

    // ", cache hits N/M (P%)" once there have been lookups, ", cache off"
    // when the table is off in a two-pass run, else nothing.
    std::string text() const {
        if (cache_.lookups.load(std::memory_order_relaxed) != 0)
            return cache_.text("cache");
        if (passes_ > 1 && !rest_table_.enabled()) return ", cache off";
        return {};
    }

  private:
    RestTable const& rest_table_;
    int const passes_;
    SharedCacheCounts cache_;
};

struct Walk : WalkBase {
    GpuRowCounter const& counter;
    RestTable const& rest_table;

    GpuRowWalker walker(int pass) const {
        return GpuRowWalker(steps, args.search.pairs, pair_bags.size(),
                            counter, rest_table,
                            !rest_table.enabled() ? RestMode::off
                            : pass < passes       ? RestMode::fill
                                                  : RestMode::lookup);
    }

    GpuCacheHooks progress_hooks() const {
        return GpuCacheHooks(rest_table, passes);
    }

    // Prints the row whose picks are `picks`, counted from what they leave
    // alone.
    void print_score_row(std::vector<size_t> const& picks) const {
        PairsCounts const rest = picks_rest(*this, picks);
        GpuCounts counts;
        counter.count(rest.values.data(), 1, &counts);
        BagStatsRow row = counts_row(gpu_counts(counts));
        name_picks(*this, picks, &row);
        write_bag_stats_row(stdout, row);
    }

    void after_score(std::vector<size_t> const&) const {}
};

}  // namespace

int main(int argc, char* argv[]) {
    BagStatsArgs args;
    bool help = false;
    std::string letters;
    if (!parse_args(argv, &args, &help) ||
        (!help && !remaining_letters(args, &letters))) {
        usage(argv[0], stderr);
        return 2;
    }
    if (help) {
        usage(argv[0], stdout);
        return 0;
    }

    BagStatsInputs inputs(pairs_letter_counts(letters.c_str()));
    if (letter_total(inputs.bag) > GPU_MAX_LETTERS) {
        report(PROGRAM, "LETTERS has more than %d letters", GPU_MAX_LETTERS);
        return 1;
    }
    if (!load_bag_stats_inputs(PROGRAM, args, &inputs, nullptr)) return 1;
    GpuRowCounter const counter(PROGRAM, gpu_inputs(inputs, args));
    if (args.progress != 0)
        report(PROGRAM, "%s, %d-bit bag keys, %.1f MB of GPU memory",
               counter.device().c_str(), counter.key_bits(),
               double(counter.bytes()) / (1024 * 1024));

    size_t const threads = bag_stats_threads(args);
    BagStatsRankers rankers(args, inputs);
    std::vector<std::vector<PairBag> const*> const& steps = inputs.steps;
    bool const one_row = !args.rank.score.empty() && args.rank.sorts.empty();
    RestTable const rest_table(PROGRAM, inputs.bag, rankers.passes && !one_row,
                               size_t(args.cache_size_mb),
                               args.progress != 0);
    if (args.progress != 0) report_rest_table(PROGRAM, rest_table);
    Walk const walk{{PROGRAM, steps, inputs.pair_bags, inputs.prospect_bags,
                     inputs.bag, inputs.word_bags, args, threads,
                     rankers.passes ? 2 : 1,
                     one_row ? unit_blocks(steps[0]->size())
                             : census_blocks(steps, args.search.pairs,
                                             inputs.bag, threads,
                                             GPU_BATCH_ROWS)},
                    counter, rest_table};
    return run_bag_stats(walk, letters, &rankers);
}
