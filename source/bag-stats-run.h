#ifndef NUTRIMATIC_BAG_STATS_RUN_H
#define NUTRIMATIC_BAG_STATS_RUN_H

// The command line, input loading, row walk and passes bag-stats and
// bag-stats-cuda share. Each tool supplies a Walk deriving from WalkBase
// that adds walker(pass), progress_hooks(), print_score_row() and
// after_score().

#include <stdint.h>
#include <stdio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "bag-stats-impl.h"
#include "bag-stats-rank.h"
#include "classified.h"
#include "optparse.h"
#include "pairs-impl.h"

inline constexpr int INNER_MIN_WORD_LENGTH = 3;
inline constexpr int DEFAULT_MIN_WB_LENGTH = 5;
inline constexpr int DEFAULT_MIN_PB_LENGTH = 9;
inline constexpr char DEFAULT_SEARCH[] = "pb,wb,p";
inline constexpr int DEFAULT_PROGRESS_SECONDS = 60;
inline constexpr int BAG_STATS_DEFAULT_CACHE_SIZE_MB = 128;
// How many counted rows may wait to be printed, per thread, before workers
// stop starting blocks.
inline constexpr size_t PRINT_WINDOW_ROWS_PER_THREAD = 1 << 16;

// The options both tools take.
struct BagStatsArgs {
    std::string letters;
    std::string used_letters;
    int sentence = CLASSIFIED_NO_SENTENCE;
    // Whether PAIRS and --pp drop the classified no.pairs.
    bool drop_no = true;
    int min_word_bag_length = DEFAULT_MIN_WB_LENGTH;
    int min_pair_bag_length = DEFAULT_MIN_PB_LENGTH;
    char const* search_text = DEFAULT_SEARCH;
    Search search;
    char const* prospects_file = NULL;
    int threads = 1;
    // At most this many MB of cached remainder counts, 0 for none.
    int cache_size_mb = BAG_STATS_DEFAULT_CACHE_SIZE_MB;
    int progress = DEFAULT_PROGRESS_SECONDS;
    RankArgs rank;
    char const* pairs_file = NULL;
    char const* dictionary_file = NULL;
};

enum {
    OPT_COMMON_MIN_WB_LENGTH = 256,
    OPT_COMMON_MIN_PB_LENGTH,
    OPT_COMMON_SEARCH,
    OPT_COMMON_PROSPECTS,
    OPT_COMMON_THREADS,
    OPT_COMMON_CACHE_SIZE,
    OPT_COMMON_PROGRESS,
    OPT_COMMON_NO_NOPAIRS,
    // The first option id free for a tool's own options.
    OPT_COMMON_END,
};

// The optparse_long entries of BagStatsArgs' options.
#define BAG_STATS_COMMON_LONG_OPTIONS \
    { "used-letters", 'u', OPTPARSE_REQUIRED }, \
    CLASSIFIED_SENTENCE_LONG_OPTION, \
    { "min-wb-len", OPT_COMMON_MIN_WB_LENGTH, OPTPARSE_REQUIRED }, \
    { "min-pb-len", OPT_COMMON_MIN_PB_LENGTH, OPTPARSE_REQUIRED }, \
    { "ss", OPT_COMMON_SEARCH, OPTPARSE_REQUIRED }, \
    { "pp", OPT_COMMON_PROSPECTS, OPTPARSE_REQUIRED }, \
    { "threads", OPT_COMMON_THREADS, OPTPARSE_REQUIRED }, \
    { "cache-size", OPT_COMMON_CACHE_SIZE, OPTPARSE_REQUIRED }, \
    { "progress", OPT_COMMON_PROGRESS, OPTPARSE_REQUIRED }, \
    { "no-nopairs", OPT_COMMON_NO_NOPAIRS, OPTPARSE_NONE }, \
    BAG_STATS_RANK_LONG_OPTIONS

// Applies `option`, with `arg`, to `out` when it is one of
// BAG_STATS_COMMON_LONG_OPTIONS. A bad argument is diagnosed, prefixed by
// `program`.
RankOption parse_common_option(std::string_view program, int option,
                               char const* arg, BagStatsArgs* out);

// Reads LETTERS PAIRS DICT from `options` once the options are parsed, then
// checks `out`. False, diagnosed prefixed by `program` unless the operands
// are wrong, when a check fails.
bool check_common_args(std::string_view program, struct optparse* options,
                       BagStatsArgs* out);

// Prints what both tools do and the --help lines of their options through
// the rank output options; `pair_filter` adds a last pf.
void help_common_options(bool pair_filter);

// Prints the --help lines of -h and the SORT options.
void help_common_tail();

// The no.pairs `args` drops from PAIRS and --pp: the global ones and -s's,
// or none with --no-nopairs.
bool load_rejected_pairs(std::string_view program, BagStatsArgs const& args,
                         DfsPairSet* out);

// LETTERS less -u's letters.
bool remaining_letters(BagStatsArgs const& args, std::string* out);

// Collects word bags into `out`: the distinct letter bags of the lines added
// that fit `bag` and have at least `min_length` letters, in order of their
// first line. With `spellings`, also each bag's distinct lines, in order.
class WordBagLoader {
  public:
    WordBagLoader(PairsCounts const& bag, int min_length,
                  std::vector<PairsCounts>* out,
                  std::vector<std::vector<std::string>>* spellings)
        : bag_(bag), min_length_(min_length), out_(out),
          spellings_(spellings) {}

    void add(std::string const& line);

  private:
    PairsCounts bag_;
    int min_length_;
    std::vector<PairsCounts>* out_;
    std::vector<std::vector<std::string>>* spellings_;
    std::unordered_map<PairsCounts, size_t, PairsCountsHash> seen_;
};

// The word bags of DICT's lines, as WordBagLoader collects them.
bool load_word_bags(std::string_view program, char const* path,
                    PairsCounts const& bag, int min_length,
                    std::vector<PairsCounts>* out,
                    std::vector<std::vector<std::string>>* spellings);

// Adds the workflow dictionary's words to `out` and finishes it.
bool load_inner_dictionary(std::string_view program, PairsExactCounter* out);

// Writes the bags of `from` that fit `rest` to `out`.
void filter_word_bags(std::vector<PairsCounts> const& from,
                      PairsCounts const& rest, std::vector<PairsCounts>* out);

// What both tools load: the pick lists, the word bags, and the inner
// dictionary a last p counts over or the PAIRS bags a last pf counts, with
// their rows when they are listed; and the steps the walk picks from.
struct BagStatsInputs {
    explicit BagStatsInputs(PairsCounts const& bag)
        : bag(bag), counter(bag, INNER_MIN_WORD_LENGTH) {}

    PairsCounts bag;
    std::vector<PairBag> prospect_bags;
    std::vector<PairBag> pair_bags;
    std::vector<PairsCounts> word_bags;
    PairsExactCounter counter;
    std::vector<PairBag> final_bags;
    std::vector<std::vector<std::string>> final_rows;
    bool has_prior = false;
    // Each pick step's list: the pb steps, then the ppb steps.
    std::vector<std::vector<PairBag> const*> steps;
};

// Loads `out` for `args`, DICT only with wb steps and PAIRS only with pb
// steps or a last pf, with `spellings` as load_word_bags() takes it; with
// `spellings` and a last pf, the final bags' rows too.
bool load_bag_stats_inputs(std::string_view program, BagStatsArgs const& args,
                           BagStatsInputs* out,
                           std::vector<std::vector<std::string>>* spellings);

// --threads, at most the hardware threads.
size_t bag_stats_threads(BagStatsArgs const& args);

// Runs work(thread) on `threads` threads, the calling thread 0.
template <typename Work>
void run_threads(size_t threads, Work const& work) {
    std::vector<std::thread> workers;
    for (size_t t = 1; t < threads; ++t) workers.emplace_back(work, t);
    work(size_t(0));
    for (std::thread& worker : workers) worker.join();
}

// "1h02m", "34m10s" or "12s".
std::string format_duration(double seconds);

// The row walk: every combination of one pick per pair step that fits, with
// picks from one list in increasing order and no bag picked twice. The pb
// steps come first, so a row's first pick, its unit, is a PAIRS bag unless
// there are none. `pair_bags`: how many PAIRS bags pb picks from.
class PickWalk {
  public:
    PickWalk(std::vector<std::vector<PairBag> const*> const& steps,
             int pair_steps, size_t pair_bags)
        : chosen_(steps.size()), steps_(steps),
          pair_steps_(size_t(pair_steps)), pair_bags_(pair_bags) {}

    // Walks the rows whose first pick is `unit`, calling enter(step, rest)
    // before each step's picks past the first, `rest` what the picks of
    // steps 0 to `step` leave, and leaf(rest, letters) at each row,
    // `letters` the letters in `bag`.
    template <typename Enter, typename Leaf>
    void walk(size_t unit, PairsCounts const& bag, Enter const& enter,
              Leaf const& leaf) {
        PairsCounts rest;
        if (!pairs_subtract(bag, (*steps_[0])[unit].counts, &rest)) return;
        chosen_[0] = unit;
        descend(1, rest, letter_total(bag), enter, leaf);
    }

    // The current row's PAIR: its ppb picks, then its pb picks.
    std::string pair() const;

    // Whether the current row's picks, one list index per pair step, are
    // `picks`.
    bool at(std::vector<size_t> const& picks) const {
        return chosen_ == picks;
    }

    // The current row's picks, one per pair step: a PAIRS bag's list index,
    // or a --pp bag's past every PAIRS bag.
    void picks(std::vector<uint32_t>* out) const;

    // The current row's PAIRS.
    uint64_t pairs() const;

  protected:
    // The current row's list index per pair step.
    std::vector<size_t> chosen_;

  private:
    template <typename Enter, typename Leaf>
    void descend(size_t step, PairsCounts const& rest, int letters,
                 Enter const& enter, Leaf const& leaf) {
        if (step == steps_.size()) {
            leaf(rest, letters);
            return;
        }
        enter(step - 1, rest);
        std::vector<PairBag> const& bags = *steps_[step];
        size_t const start = steps_[step - 1] == steps_[step]
            ? chosen_[step - 1] + 1 : 0;
        PairsCounts next;
        for (size_t i = start; i < bags.size(); ++i) {
            if (!pairs_subtract(rest, bags[i].counts, &next) ||
                repeats_bag(step, bags[i].counts))
                continue;
            chosen_[step] = i;
            descend(step + 1, next, letters, enter, leaf);
        }
    }

    bool repeats_bag(size_t step, PairsCounts const& counts) const;

    std::vector<std::vector<PairBag> const*> const& steps_;
    size_t pair_steps_;
    size_t pair_bags_;
};

// How many units PickWalk walks over `steps`: the first step's picks, or 1
// without steps, where nothing walks them.
inline size_t walk_units(
        std::vector<std::vector<PairBag> const*> const& steps) {
    return steps.empty() ? 1 : steps[0]->size();
}

// Block boundaries of one unit each: block b is units [b, b + 1).
std::vector<size_t> unit_blocks(size_t units);

// Block boundaries from a census of each unit's rows, on `threads` threads:
// consecutive units of about `rows` rows a block. Block b is units
// [out[b], out[b + 1]).
std::vector<size_t> census_blocks(
    std::vector<std::vector<PairBag> const*> const& steps, int pair_steps,
    PairsCounts const& bag, size_t threads, uint64_t rows);

// Reports pass `pass` of `walk`'s finished units to stderr every
// --progress seconds, 0 for never, and when it ends, its time. The hooks
// from walk.progress_hooks() collect a walker's stats with
// finished(walker), called from any thread, and give the line's trailing
// text with text().
template <typename Walk>
class Progress {
  public:
    using Clock = std::chrono::steady_clock;

    Progress(Walk const& walk, int pass)
        : program_(walk.program),
          interval_(std::chrono::seconds(walk.args.progress)), pass_(pass),
          passes_(walk.passes), units_(walk.units()),
          hooks_(walk.progress_hooks()), start_(Clock::now()),
          next_report_(walk.args.progress > 0
                           ? (start_ + interval_).time_since_epoch().count()
                           : INT64_MAX) {}

    ~Progress() {
        if (interval_.count() == 0) return;
        double const elapsed = seconds_since_start(Clock::now());
        fprintf(stderr, "%.*s: pass %d/%d: done in %s%s\n",
                int(program_.size()), program_.data(), pass_,
                passes_, format_duration(elapsed).c_str(),
                hooks_.text().c_str());
    }

    // Counts `units` finished units, and hands `walker` to the hooks.
    template <typename Walker>
    void finished(Walker* walker, size_t units) {
        hooks_.finished(walker);
        size_t const done =
            done_.fetch_add(units, std::memory_order_relaxed) + units;
        Clock::time_point const now = Clock::now();
        int64_t due = next_report_.load(std::memory_order_relaxed);
        if (now.time_since_epoch().count() < due ||
            !next_report_.compare_exchange_strong(
                due, (now + interval_).time_since_epoch().count(),
                std::memory_order_relaxed))
            return;
        double const elapsed = seconds_since_start(now);
        fprintf(stderr, "%.*s: pass %d/%d: %zu/%zu pair bags (%.1f%%), %s, "
                "~%s left%s\n", int(program_.size()), program_.data(), pass_,
                passes_, done, units_, 100.0 * double(done) / double(units_),
                format_duration(elapsed).c_str(),
                format_duration(elapsed * double(units_ - done) /
                                double(done)).c_str(),
                hooks_.text().c_str());
    }

  private:
    double seconds_since_start(Clock::time_point now) const {
        return std::chrono::duration<double>(now - start_).count();
    }

    std::string program_;
    Clock::duration const interval_;
    int const pass_;
    int const passes_;
    size_t const units_;
    decltype(std::declval<Walk const&>().progress_hooks()) hooks_;
    Clock::time_point const start_;
    std::atomic<size_t> done_{0};
    std::atomic<int64_t> next_report_;
};

// What every pass shares.
struct WalkBase {
    std::string program;
    std::vector<std::vector<PairBag> const*> const& steps;
    // The PAIRS bags pb picks from and the --pp bags ppb picks from.
    std::vector<PairBag> const& pair_bags;
    std::vector<PairBag> const& prospect_bags;
    PairsCounts const& bag;
    std::vector<PairsCounts> const& word_bags;
    BagStatsArgs const& args;
    size_t threads;
    // How many passes walk the rows: 2 when ranking needs the sort value
    // counts first.
    int passes;
    // Block b is units [blocks[b], blocks[b + 1]).
    std::vector<size_t> blocks;

    size_t units() const { return walk_units(steps); }
    size_t block_count() const { return blocks.size() - 1; }
    size_t block_units(size_t block) const {
        return blocks[block + 1] - blocks[block];
    }
};

// What the picks `picks`, list indices in walk step order, leave of
// `walk`'s bag.
PairsCounts picks_rest(WalkBase const& walk,
                       std::vector<size_t> const& picks);

// Sets `row`'s PAIR and PAIRS to those of the picks `picks`.
void name_picks(WalkBase const& walk, std::vector<size_t> const& picks,
                BagStatsRow* row);

// Prints every row, in walk order, while later blocks are counted.
template <typename Walk>
void print_rows(Walk const& walk) {
    size_t const blocks = walk.block_count();
    size_t const window = PRINT_WINDOW_ROWS_PER_THREAD * walk.threads;
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> lines(blocks);
    std::vector<size_t> block_rows(blocks, 0);
    std::vector<char> done(blocks, 0);
    size_t next = 0;
    size_t waiting = 0;
    Progress progress(walk, 1);

    auto const work = [&](size_t) {
        auto walker = walk.walker(1);
        std::string buffer;
        while (true) {
            size_t block;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [&] {
                    return next >= blocks || waiting < window;
                });
                if (next >= blocks) return;
                block = next++;
            }
            buffer.clear();
            size_t rows = 0;
            walker.walk(walk.blocks[block], walk.blocks[block + 1], walk.bag,
                        [&](size_t, BagStatsRow& row, size_t) {
                row.pair = walker.pair();
                row.pairs = walker.pairs();
                append_row(&buffer, row, walk.args.rank.top_segments);
                ++rows;
            });
            {
                std::lock_guard<std::mutex> lock(mutex);
                lines[block] = std::move(buffer);
                block_rows[block] = rows;
                waiting += rows;
                done[block] = 1;
            }
            changed.notify_all();
            progress.finished(&walker, walk.block_units(block));
        }
    };
    std::vector<std::thread> workers;
    for (size_t t = 0; t < walk.threads; ++t) workers.emplace_back(work, t);
    for (size_t block = 0; block < blocks; ++block) {
        std::string line;
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [&] { return done[block] != 0; });
            line = std::move(lines[block]);
        }
        fwrite(line.data(), 1, line.size(), stdout);
        {
            std::lock_guard<std::mutex> lock(mutex);
            waiting -= block_rows[block];
        }
        changed.notify_all();
    }
    for (std::thread& worker : workers) worker.join();
}

// Fills `ranker` with how many rows have each sort value.
template <typename Walk, typename R>
void count_sort_values(Walk const& walk, R* ranker) {
    std::atomic<size_t> next{0};
    std::vector<typename R::Part> parts(walk.threads, ranker->part());
    {
        Progress progress(walk, 1);
        run_threads(walk.threads, [&](size_t t) {
            auto walker = walk.walker(1);
            std::vector<uint32_t> picks;
            for (size_t block;
                 (block = next.fetch_add(1)) < walk.block_count();) {
                walker.walk(walk.blocks[block], walk.blocks[block + 1],
                            walk.bag,
                            [&](size_t, BagStatsRow const& row,
                                size_t length) {
                    if constexpr (R::NEEDS_PICKS) walker.picks(&picks);
                    ranker->add(&parts[t], row, length, picks);
                });
                progress.finished(&walker, walk.block_units(block));
            }
        });
    }
    ranker->finish(parts);
}

// The `top` rows that rank first, in order.
template <typename Walk, typename R>
std::vector<RankedRow> top_rows(Walk const& walk, R const& ranker,
                                size_t top) {
    std::atomic<size_t> next{0};
    std::vector<std::vector<RankedRow>> heaps(walk.threads);
    Progress progress(walk, walk.passes);
    run_threads(walk.threads, [&](size_t t) {
        auto walker = walk.walker(walk.passes);
        std::vector<RankedRow>& heap = heaps[t];
        std::vector<uint32_t> picks;
        for (size_t block;
             (block = next.fetch_add(1)) < walk.block_count();) {
            size_t current = SIZE_MAX;
            uint64_t seq = 0;
            walker.walk(walk.blocks[block], walk.blocks[block + 1], walk.bag,
                        [&](size_t unit, BagStatsRow& row, size_t length) {
                if (unit != current) {
                    current = unit;
                    seq = 0;
                }
                if constexpr (R::NEEDS_PICKS) walker.picks(&picks);
                RankedRow ranked{ranker.key(row, length, picks), unit, seq++,
                                 {}, {}};
                if (heap.size() == top && !ranks_before(ranked, heap.front()))
                    return;
                if (walk.args.rank.best_pairs == BestPairs::APPEAR) {
                    walker.picks(&ranked.best);
                } else if constexpr (R::NEEDS_PICKS) {
                    if (walk.args.rank.best_pairs == BestPairs::LEAD)
                        ranked.best = ranker.leaders(row, picks);
                }
                row.pair = walker.pair();
                row.pairs = walker.pairs();
                ranked.row = std::move(row);
                if (heap.size() == top) {
                    std::pop_heap(heap.begin(), heap.end(), ranks_before);
                    heap.pop_back();
                }
                heap.push_back(std::move(ranked));
                std::push_heap(heap.begin(), heap.end(), ranks_before);
            });
            progress.finished(&walker, walk.block_units(block));
        }
    });
    std::vector<RankedRow> out;
    for (std::vector<RankedRow>& heap : heaps)
        std::move(heap.begin(), heap.end(), std::back_inserter(out));
    std::sort(out.begin(), out.end(), ranks_before);
    if (out.size() > top) out.resize(top);
    return out;
}

// The row whose picks are `picks`, with its unit and walk sequence, its
// pair picks' letter count and its picks as PickWalk::picks() numbers them;
// false when they don't make a row.
template <typename Walk>
bool find_row(Walk const& walk, std::vector<size_t> const& picks,
              RankedRow* out, size_t* out_length,
              std::vector<uint32_t>* out_picks) {
    auto walker = walk.walker(1);
    uint64_t seq = 0;
    bool found = false;
    walker.walk(picks[0], picks[0] + 1, walk.bag,
                [&](size_t, BagStatsRow& row, size_t length) {
        if (!found && walker.at(picks)) {
            row.pair = walker.pair();
            row.pairs = walker.pairs();
            *out = {{}, picks[0], seq, std::move(row), {}};
            *out_length = length;
            walker.picks(out_picks);
            found = true;
        }
        ++seq;
    });
    return found;
}

// Where `target`, its key set, comes among all rows.
template <typename Walk, typename R>
ScoreRank score_rank(Walk const& walk, R const& ranker,
                     RankedRow const& target) {
    std::atomic<size_t> next{0};
    std::vector<ScoreRank> parts(walk.threads);
    Progress progress(walk, walk.passes);
    run_threads(walk.threads, [&](size_t t) {
        auto walker = walk.walker(walk.passes);
        ScoreRank& part = parts[t];
        std::vector<uint32_t> picks;
        for (size_t block;
             (block = next.fetch_add(1)) < walk.block_count();) {
            size_t current = SIZE_MAX;
            uint64_t seq = 0;
            walker.walk(walk.blocks[block], walk.blocks[block + 1], walk.bag,
                        [&](size_t unit, BagStatsRow const& row,
                            size_t length) {
                if (unit != current) {
                    current = unit;
                    seq = 0;
                }
                if constexpr (R::NEEDS_PICKS) walker.picks(&picks);
                RankedRow const ranked{ranker.key(row, length, picks), unit,
                                       seq++, {}, {}};
                add_score_rank(ranked, target, &part);
            });
            progress.finished(&walker, walk.block_units(block));
        }
    });
    ScoreRank out;
    for (ScoreRank const& part : parts) {
        out.before += part.before;
        out.larger += part.larger;
        out.tied += part.tied;
        out.rows += part.rows;
    }
    return out;
}

// Prints the --score row, without SORT options by the walk's
// print_score_row(), else with its rank, then the walk's after_score().
template <typename Walk, typename R>
int print_score(Walk const& walk, R* ranker, bool passes) {
    std::vector<size_t> picks;
    if (!score_picks(walk.program, walk.args.rank.score, walk.args.search,
                     walk.steps, walk.bag, &picks))
        return 1;
    if (walk.args.rank.sorts.empty()) {
        walk.print_score_row(picks);
        return 0;
    }
    RankedRow target;
    size_t length;
    std::vector<uint32_t> target_picks;
    if (!find_row(walk, picks, &target, &length, &target_picks)) {
        fprintf(stderr, "%.*s: no row has the --score picks\n",
                int(walk.program.size()), walk.program.data());
        return 1;
    }
    if (passes) count_sort_values(walk, ranker);
    target.key = ranker->key(target.row, length, target_picks);
    ScoreRank const rank = score_rank(walk, *ranker, target);
    print_score_rank(target.row, rank);
    if constexpr (R::NEEDS_PICKS) {
        if (walk.args.rank.score_detail) {
            std::vector<PairBag const*> bags;
            for (size_t j = 0; j < picks.size(); ++j)
                bags.push_back(&(*walk.steps[j])[picks[j]]);
            print_score_detail(walk.args.rank.sorts, *ranker, target, bags,
                               target_picks);
        }
    }
    walk.after_score(picks);
    return 0;
}

// Prints the -n rows, or with --best-pairs each recognized pick the first
// time a row recognizes it.
template <typename Walk, typename R>
void print_top(Walk const& walk, R* ranker, bool passes) {
    if (passes) count_sort_values(walk, ranker);
    std::string line;
    std::unordered_set<uint32_t> seen;
    for (RankedRow const& ranked :
         top_rows(walk, *ranker, size_t(walk.args.rank.top))) {
        line.clear();
        if (walk.args.rank.best_pairs == BestPairs::NONE)
            append_row(&line, ranked.row, walk.args.rank.top_segments);
        append_best_pairs(&line, &seen, ranked.best, walk.pair_bags,
                          walk.prospect_bags);
        fwrite(line.data(), 1, line.size(), stdout);
    }
}

// The rankers for `inputs`' rows under `args`.
struct BagStatsRankers {
    BagStatsRankers(BagStatsArgs const& args, BagStatsInputs const& inputs);

    Ranker ranker;
    PickRanker pick_ranker;
    // Whether ranking walks the rows twice, counting sort values first.
    bool passes;
};

// Writes the header of `walk`'s rows on `letters` to standard output.
void write_walk_header(WalkBase const& walk, std::string const& letters);

// The output for `walk`: the --score row, the -n rows, or every row.
template <typename Walk>
int run_bag_stats(Walk const& walk, std::string const& letters,
                  BagStatsRankers* rankers) {
    BagStatsArgs const& args = walk.args;
    RankArgs const& rank = args.rank;
    bool const passes = rankers->passes;
    setvbuf(stdout, nullptr, _IOFBF, 1 << 22);
    if (!rank.score.empty())
        return rank.by_pick
            ? print_score(walk, &rankers->pick_ranker, passes)
            : print_score(walk, &rankers->ranker, passes);
    if (!rank.top_segments && rank.best_pairs == BestPairs::NONE)
        write_walk_header(walk, letters);
    if (rank.top == 0) {
        print_rows(walk);
        return 0;
    }
    if (rank.by_pick) print_top(walk, &rankers->pick_ranker, passes);
    else print_top(walk, &rankers->ranker, passes);
    return 0;
}

#endif
