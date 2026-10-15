// bag-stats-cuda.cpp - bag-stats with every row's counts computed on the
// GPU.

#include "bag-stats-gpu.h"
#include "bag-stats-impl.h"
#include "bag-stats-rank.h"
#include "bag-stats-run.h"
#include "optparse.h"
#include "pairs-impl.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr char PROGRAM[] = "bag-stats-cuda";

static_assert(sizeof(PairsCounts) == GPU_LETTERS_BYTES);
static_assert(sizeof(GpuCounts) == sizeof(BagStatsCounts));

void usage(char const* program, FILE* out) {
    fprintf(out,
            "usage: %s [-u LETTERS] [-s N] [--min-wb-len N] [--ss STEPS] [--pp FILE]\n"
            "       [--threads N] [--progress N]\n"
            "       [-n N SORT... [--by-length | --by-pick]]\n"
            "       [--score W1,W2... [--score-detail]\n"
            "        [SORT... [--by-length | --by-pick]]]\n"
            "       [--best-pairs appear|lead]\n"
            "       [--prior FILE | [--p1 INDEX] [--clues CLUEDATA]\n"
            "        [--prior-weights SPEC]] [--top-segments]\n"
            "       LETTERS PAIRS DICT\n",
            program);
    if (out != stdout) return;

    fputs("  bag-stats, counting every row on the GPU\n\n", stdout);
    help_common_options();
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
    if (out->search.words > GPU_MAX_WORDS) {
        fprintf(stderr, "%s: --ss takes at most %d wb steps\n", PROGRAM,
                GPU_MAX_WORDS);
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
    out.min_word_bag_length = args.min_word_bag_length;
    out.min_word_bag_pairs = BAG_STATS_MIN_WB_PAIRS;
    return out;
}

BagStatsRow gpu_row(GpuCounts const& counts) {
    BagStatsRow row{};
    row.word_bags = counts.word_bags;
    row.with_pairs = counts.with_pairs;
    row.total_pairs = counts.total_pairs;
    row.max_pairs = counts.max_pairs;
    row.solo = counts.solo;
    row.min_word_bags = counts.min_word_bags;
    row.min_with_pairs = counts.min_with_pairs;
    return row;
}

// Progress hooks: the GPU walk has no stats to report.
struct NoHooks {
    template <typename Walker>
    void finished(Walker*) {}

    std::string text() const { return {}; }
};

// Walks the rows as PickWalk does, buffering each row's picks and rest, and
// counts the buffer on the GPU when it holds GPU_BATCH_ROWS rows and at the
// end of a walk.
class GpuRowWalker : public PickWalk {
  public:
    GpuRowWalker(std::vector<std::vector<PairBag> const*> const& steps,
                 int pair_steps, GpuRowCounter const& counter)
        : PickWalk(steps, pair_steps), counter_(counter) {}

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
                rests_.insert(rests_.end(), rest.values.begin(),
                              rest.values.end());
                if (units_.size() == GPU_BATCH_ROWS) flush(visit);
            });
        }
        flush(visit);
    }

  private:
    template <typename Visit>
    void flush(Visit const& visit) {
        size_t const rows = units_.size();
        if (rows == 0) return;
        counts_.resize(rows);
        counter_.count(rests_.data(), rows, counts_.data());
        saved_ = chosen_;
        size_t const steps = chosen_.size();
        for (size_t r = 0; r < rows; ++r) {
            std::copy_n(picks_.begin() + r * steps, steps, chosen_.begin());
            BagStatsRow row = gpu_row(counts_[r]);
            visit(units_[r], row, lengths_[r]);
        }
        chosen_ = saved_;
        units_.clear();
        picks_.clear();
        lengths_.clear();
        rests_.clear();
    }

    GpuRowCounter const& counter_;
    // The buffered rows: each one's unit, picks, pair letters and rest.
    std::vector<size_t> units_;
    std::vector<size_t> picks_;
    std::vector<size_t> lengths_;
    std::vector<uint8_t> rests_;
    std::vector<GpuCounts> counts_;
    std::vector<size_t> saved_;
};

struct Walk : WalkBase {
    GpuRowCounter const& counter;

    GpuRowWalker walker() const {
        return GpuRowWalker(steps, args.search.pairs, counter);
    }

    NoHooks progress_hooks() const { return {}; }

    // The row whose picks are `picks`, counted from what they leave alone.
    BagStatsRow score_row(std::vector<size_t> const& picks) const {
        PairsCounts const rest = picks_rest(*this, picks);
        GpuCounts counts;
        counter.count(rest.values.data(), 1, &counts);
        BagStatsRow row = gpu_row(counts);
        name_picks(*this, picks, &row);
        return row;
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
        fprintf(stderr, "%s: LETTERS has more than %d letters\n", PROGRAM,
                GPU_MAX_LETTERS);
        return 1;
    }
    if (!load_bag_stats_inputs(PROGRAM, args, &inputs, nullptr)) return 1;
    GpuRowCounter const counter(PROGRAM, gpu_inputs(inputs, args));
    if (args.progress != 0)
        fprintf(stderr, "%s: %s, %d-bit bag keys, %.1f MB of GPU memory\n",
                PROGRAM, counter.device().c_str(), counter.key_bits(),
                double(counter.bytes()) / (1024 * 1024));

    size_t const threads = bag_stats_threads(args);
    BagStatsRankers rankers(args, inputs);
    std::vector<std::vector<PairBag> const*> const& steps = inputs.steps;
    bool const one_row = !args.rank.score.empty() && args.rank.sorts.empty();
    Walk const walk{{PROGRAM, steps, inputs.prospect_bags, inputs.bag,
                     inputs.word_bags, args, threads,
                     rankers.passes ? 2 : 1,
                     one_row ? unit_blocks(steps[0]->size())
                             : census_blocks(steps, args.search.pairs,
                                             inputs.bag, threads,
                                             GPU_BATCH_ROWS)},
                    counter};
    return run_bag_stats(walk, letters, &rankers);
}
