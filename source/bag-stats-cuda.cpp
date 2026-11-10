// bag-stats-cuda.cpp - bag-stats with every row's counts computed on the
// GPU.

#include "bag-stats-cache.h"
#include "bag-stats-gpu.h"
#include "bag-stats-impl.h"
#include "bag-stats-rank.h"
#include "bag-stats-run.h"
#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "log.h"
#include "optparse.h"
#include "pairs-impl.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>
#include <mutex>
#include <memory>
#include <condition_variable>
#include <numeric>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

constexpr int CELL_BITS = 18;
constexpr int UNARY_MOST = 3;
namespace {

constexpr char PROGRAM[] = "bag-stats-cuda";
constexpr int OPT_STREAMS = OPT_COMMON_END;
constexpr int OPT_LEFTOVERS = OPT_COMMON_END + 1;
constexpr int OPT_HLL = OPT_COMMON_END + 2;
constexpr int OPT_UNIQUE = OPT_COMMON_END + 3;
constexpr int OPT_COUNT = OPT_COMMON_END + 4;
constexpr int OPT_UNIQUE_COUNT = OPT_COMMON_END + 5;
constexpr size_t PREFIX_RESTS = 1000;
constexpr size_t PREFIX_DRAWS = 1000000;

static_assert(sizeof(PairsCounts) == GPU_LETTERS_BYTES);
static_assert(sizeof(GpuCounts) == sizeof(BagStatsCounts));
static_assert(GPU_HLL_BITS == LeftoverSketch::HLL_BITS);

// What the command line asks for beyond BagStatsArgs: --streams; with
// --leftovers, --hll; --unique.
struct CudaOptions {
    int streams = 1;
    bool hll = false;
    bool unique = false;
};

void usage(std::string_view program, FILE* out) {
    fprintf(out,
            "usage: %.*s [-u LETTERS] [-s N | --no-nopairs] [--min-wb-len N]\n"
            "       [--min-pb-len N]\n"
            "       [--ss STEPS] [--pp FILE] [--threads N] [--streams N]\n"
            "       [--cache-size N] [--progress N]\n"
            "       [-n N SORT... [--by-length | --by-pick]]\n"
            "       [--score W1,W2... [--score-detail]\n"
            "        [SORT... [--by-length | --by-pick]]]\n"
            "       [--best-pairs appear|lead]\n"
            "       [--prior FILE | --no-prior |\n"
            "        [--p1 INDEX] [--clues CLUEDATA] [--prior-weights SPEC]]\n"
            "       [--top-segments]\n"
            "       LETTERS PAIRS DICT\n"
            "       %.*s --leftovers --hll --ss pb[,pb...]\n"
            "       [-u LETTERS] [-s N | --no-nopairs] [--min-pb-len N]\n"
            "       [--progress N] LETTERS PAIRS DICT\n"
            "       %.*s --unique [--count] --ss [ppb,...][pb,...] [-n N]\n"
            "       [--pp FILE] [-u LETTERS] [-s N | --no-nopairs]\n"
            "       [--min-pb-len N] [--progress N] LETTERS PAIRS DICT\n",
            SV_ARG(program), SV_ARG(program), SV_ARG(program));
    if (out != stdout) return;

    fputs("  bag-stats, counting every row on the GPU\n\n", stdout);
    help_common_options(false);
    dfs_help_option("--streams N",
                    "count on N GPU streams, each fed by its own thread; "
                    "--threads N is for the CPU-only work (default: 1)");
    dfs_help_option("--leftovers",
                    "with --ss of pb steps alone and --hll, count the "
                    "combinations that fit, by how many picks they make, "
                    "and those of every pb step, their PAIRS rows and their "
                    "distinct leftovers by how many letters they leave, all "
                    "on the GPU");
    dfs_help_option("--hll, --hyper-log-log",
                    "with --leftovers, estimate the distinct leftovers by "
                    "HyperLogLog, within about 1%%, as LEFTOVERS_HLL");
    dfs_help_option("--unique",
                    "with --ss of ppb and pb steps alone, print each PAIRS "
                    "and --pp row the combinations use as left,right, by how "
                    "many pair combinations it's in, most first, ties by "
                    "pair, the first N with -n N, all on the GPU");
    dfs_help_option("--count",
                    "with --unique, print each row as \"COUNT left right\", "
                    "COUNT the pair combinations it's in");
    dfs_help_option("--uc, --unique-count", "--unique --count");
    help_common_tail();
}

bool parse_args(char* argv[], BagStatsArgs* out, CudaOptions* cuda,
                bool* help) {
    static struct optparse_long const long_options[] = {
        BAG_STATS_COMMON_LONG_OPTIONS,
        { "streams", OPT_STREAMS, OPTPARSE_REQUIRED },
        { "leftovers", OPT_LEFTOVERS, OPTPARSE_NONE },
        { "hll", OPT_HLL, OPTPARSE_NONE },
        { "hyper-log-log", OPT_HLL, OPTPARSE_NONE },
        { "unique", OPT_UNIQUE, OPTPARSE_NONE },
        { "count", OPT_COUNT, OPTPARSE_NONE },
        { "uc", OPT_UNIQUE_COUNT, OPTPARSE_NONE },
        { "unique-count", OPT_UNIQUE_COUNT, OPTPARSE_NONE },
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
          case OPT_STREAMS:
            if (!parse_count(options.optarg, "--streams", &cuda->streams))
                return false;
            if (cuda->streams == 0) {
                report(PROGRAM, "--streams must be at least 1");
                return false;
            }
            break;
          case OPT_LEFTOVERS:
            out->leftovers = true;
            break;
          case OPT_HLL:
            cuda->hll = true;
            break;
          case OPT_UNIQUE:
            cuda->unique = true;
            break;
          case OPT_COUNT:
            out->pair_counts = true;
            break;
          case OPT_UNIQUE_COUNT:
            cuda->unique = true;
            out->pair_counts = true;
            break;
          case 'h':
            *help = true;
            return true;
          default:
            fprintf(stderr, "error: %s\n", options.errmsg);
            return false;
        }
    }

    if (cuda->hll && !out->leftovers) {
        report(PROGRAM, "--hll goes with --leftovers");
        return false;
    }
    if (out->leftovers && !cuda->hll) {
        report(PROGRAM, "--leftovers needs --hll");
        return false;
    }
    if (cuda->unique && out->leftovers) {
        report(PROGRAM, "--unique doesn't go with --leftovers");
        return false;
    }
    if (out->pair_counts && !cuda->unique) {
        report(PROGRAM, "--count goes with --unique");
        return false;
    }
    out->pair_usage = cuda->unique;
    if (!check_common_args(PROGRAM, &options, out)) return false;
    if (out->leftovers || out->pair_usage) {
        if (out->search.prospects + out->search.pairs > GPU_MAX_PAIR_STEPS) {
            report(PROGRAM, "%s takes at most %d %s steps",
                   out->leftovers ? "--leftovers" : "--unique",
                   GPU_MAX_PAIR_STEPS, out->leftovers ? "pb" : "ppb and pb");
            return false;
        }
        return true;
    }
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

// Progress hooks with nothing to add.
struct LeftoverHooks {
    template <typename Walker>
    void finished(Walker*) {}

    std::string text() const { return {}; }
};

struct LeftoverWalk : WalkBase {
    LeftoverHooks progress_hooks() const { return {}; }
};

// The bag's letters by the share of pair bags whose count of each is at
// most a drawn rest's, averaged over rests of `picks` pair bags that fit the
// bag together, smallest first.
std::vector<size_t> prefix_order(BagStatsInputs const& inputs, size_t picks) {
    PairsCounts const& bag = inputs.bag;
    std::vector<PairBag> const& bags = inputs.pair_bags;
    std::vector<size_t> letters;
    for (size_t letter = 0; letter < 26; ++letter)
        if (bag.values[letter] != 0) letters.push_back(letter);
    std::vector<std::vector<uint64_t>> at_most(26);
    for (size_t const letter : letters)
        at_most[letter].assign(bag.values[letter] + 1, 0);
    for (PairBag const& pair_bag : bags)
        for (size_t const letter : letters)
            ++at_most[letter][pair_bag.counts.values[letter]];
    for (size_t const letter : letters)
        std::partial_sum(at_most[letter].begin(), at_most[letter].end(),
                         at_most[letter].begin());

    std::vector<uint64_t> shares(26, 0);
    std::mt19937_64 random(1);
    std::uniform_int_distribution<size_t> draw(0, bags.size() - 1);
    std::vector<size_t> chosen(picks);
    size_t rests = 0;
    for (size_t draws = 0;
         !bags.empty() && rests < PREFIX_RESTS && draws < PREFIX_DRAWS;
         ++draws) {
        PairsCounts rest = bag;
        size_t k = 0;
        for (; k < picks; ++k) {
            chosen[k] = draw(random);
            if (std::find(chosen.begin(), chosen.begin() + k, chosen[k]) !=
                    chosen.begin() + k ||
                !pairs_fits(rest, bags[chosen[k]].counts))
                break;
            pairs_subtract(rest, bags[chosen[k]].counts, &rest);
        }
        if (k < picks) continue;
        ++rests;
        for (size_t const letter : letters)
            shares[letter] += at_most[letter][rest.values[letter]];
    }
    std::stable_sort(letters.begin(), letters.end(),
                     [&](size_t a, size_t b) { return shares[a] < shares[b]; });
    return letters;
}

// The run of `counts` in `gpu`'s prefix cells: its prefix letters' counts
// in unary, each at its place.
uint32_t cell_run(GpuLeftoverInputs const& gpu, PairsCounts const& counts) {
    uint32_t run = 0;
    for (auto const& [letter, place] : gpu.prefix)
        run |= ((uint32_t(1) << counts.values[letter]) - 1) << place;
    return run;
}

// Appends `bags` to `gpu`'s pair bags sorted by their prefix cells, and
// writes the list's offsets to `offsets` and the bag at each of their sorted
// positions to `sorted`.
void add_leftover_bags(std::vector<PairBag> const& bags, int run_bits,
                       GpuLeftoverInputs* gpu, std::vector<int>* offsets,
                       std::vector<size_t>* sorted) {
    std::vector<uint32_t> cells(bags.size());
    for (size_t i = 0; i < bags.size(); ++i) {
        PairsCounts const& counts = bags[i].counts;
        cells[i] = cell_run(*gpu, counts) << gpu->last_bits;
        if (gpu->last_letter >= 0) cells[i] |= counts.values[gpu->last_letter];
    }
    sorted->resize(bags.size());
    std::iota(sorted->begin(), sorted->end(), size_t(0));
    std::stable_sort(sorted->begin(), sorted->end(), [&](size_t a, size_t b) {
        return cells[a] < cells[b];
    });
    offsets->assign((size_t(1) << (run_bits + gpu->last_bits)) + 1, 0);
    (*offsets)[0] = int(gpu->pairs.size());
    for (uint32_t const cell : cells) ++(*offsets)[cell + 1];
    std::partial_sum(offsets->begin(), offsets->end(), offsets->begin());
    for (size_t const i : *sorted) {
        PairBag const& pair_bag = bags[i];
        gpu->pair_letters.insert(gpu->pair_letters.end(),
                                 pair_bag.counts.values.begin(),
                                 pair_bag.counts.values.end());
        gpu->pairs.push_back(pair_bag.pairs);
        gpu->lengths.push_back(uint8_t(letter_total(pair_bag.counts)));
        gpu->runs.push_back(cells[i] >> gpu->last_bits);
        gpu->places.push_back(uint32_t(i));
    }
}

// Writes the walk inputs of `inputs` for `args` to `gpu`, leaving the
// numbers empty and the bag's 0, and to `sorted` the PAIRS bag at each of
// the first sorted positions and the --pp bag at each of the rest.
void leftover_inputs(BagStatsInputs const& inputs, BagStatsArgs const& args,
                     GpuLeftoverInputs* out,
                     std::vector<size_t>* sorted_out) {
    size_t const steps = inputs.steps.size();
    std::vector<size_t> const order =
        steps > 1 ? prefix_order(inputs, steps - 1) : std::vector<size_t>{};
    GpuLeftoverInputs& gpu = *out;
    int const CELL_BITS = getenv("BSC_CELL_BITS") ? atoi(getenv("BSC_CELL_BITS")) : ::CELL_BITS;
    int const UNARY_MOST = getenv("BSC_UNARY") ? atoi(getenv("BSC_UNARY")) : ::UNARY_MOST;
    std::copy(inputs.bag.values.begin(), inputs.bag.values.end(), gpu.bag);
    for (size_t const letter : order) {
        if (inputs.bag.values[letter] <= UNARY_MOST) continue;
        gpu.last_letter = int(letter);
        while ((inputs.bag.values[letter] >> gpu.last_bits) != 0)
            ++gpu.last_bits;
        break;
    }
    int run_bits = 0;
    for (size_t const letter : order) {
        int const count = inputs.bag.values[letter];
        if (count > UNARY_MOST || run_bits + count + gpu.last_bits > CELL_BITS)
            continue;
        gpu.prefix.push_back({int(letter), run_bits});
        run_bits += count;
    }
    std::vector<size_t>& sorted = *sorted_out;
    add_leftover_bags(inputs.pair_bags, run_bits, &gpu, &gpu.offsets,
                      &sorted);
    gpu.pair_bags = int(sorted.size());
    if (args.search.prospects > 0) {
        std::vector<size_t> prospects;
        add_leftover_bags(inputs.prospect_bags, run_bits, &gpu,
                          &gpu.prospect_offsets, &prospects);
        sorted.insert(sorted.end(), prospects.begin(), prospects.end());
    }
    gpu.steps = int(steps);
    gpu.pair_steps = args.search.pairs;
    size_t const first = args.search.pairs > 0 ? 0 : size_t(gpu.pair_bags);
    gpu.units.resize(inputs.steps[0]->size());
    for (size_t position = first; position < first + gpu.units.size();
         ++position)
        gpu.units[gpu.places[position]] = int(position);
}

// Calls run(progress) with the progress a GPU walk of `inputs` for `args`
// reports to.
template <typename Run>
void with_walk_progress(BagStatsArgs const& args, BagStatsInputs const& inputs,
                        Run const& run) {
    LeftoverWalk const walk{{PROGRAM, inputs.steps, inputs.pair_bags,
                             inputs.prospect_bags, inputs.bag,
                             inputs.word_bags, args, 1, 1, {}, {}}};
    Progress progress(walk, 1);
    size_t reported = 0;
    run(GpuWalkProgress{[&] { return progress.due(); },
                        [&](size_t done, double share) {
        progress.measured(share);
        progress.finished(&walk, done - reported);
        reported = done;
    }});
}

// The --leftovers output of `inputs` for `args` on `letters`, walked on the
// GPU.
int run_leftovers(BagStatsArgs const& args, std::string const& letters,
                  BagStatsInputs const& inputs) {
    RemainderIndex const index(inputs.bag, UINT64_MAX - 1);
    if (index.total() == 0) {
        report(PROGRAM, "the letters have too many sub-bags to number "
               "leftovers");
        return 1;
    }
    GpuLeftoverInputs gpu;
    std::vector<size_t> sorted;
    leftover_inputs(inputs, args, &gpu, &sorted);
    gpu.bag_number = index.number(inputs.bag);
    for (size_t const i : sorted)
        gpu.numbers.push_back(index.number(inputs.pair_bags[i].counts));
    GpuLeftoverCounts counts;
    with_walk_progress(args, inputs, [&](GpuWalkProgress const& progress) {
        run_leftovers_gpu(PROGRAM, gpu, progress, &counts);
    });
    size_t const lengths = counts.combinations.size();
    LeftoverCounts total{std::move(counts.by_picks),
                         std::move(counts.combinations),
                         std::move(counts.rows),
                         std::vector<uint64_t>(lengths, 0)};
    LeftoverSketch const sketch(counts.registers);
    return print_leftovers(args, letters, inputs.pair_bags.size(),
                           std::move(total), &sketch);
}

// Writes to `gpu` the numbers in `index` of the bag and of its pair bags,
// `sorted` as leftover_inputs() wrote it.
void number_inputs(BagStatsInputs const& inputs, RemainderIndex const& index,
                   std::vector<size_t> const& sorted,
                   GpuLeftoverInputs* gpu) {
    size_t const pair_bags = size_t(gpu->pair_bags);
    gpu->bag_number = index.number(inputs.bag);
    gpu->number_count = index.total();
    for (size_t position = 0; position < sorted.size(); ++position)
        gpu->numbers.push_back(index.number(
            position < pair_bags
                ? inputs.pair_bags[sorted[position]].counts
                : inputs.prospect_bags[sorted[position]].counts));
}

// The rest `index` numbers `number`.
PairsCounts numbered_rest(RemainderIndex const& index, uint64_t number) {
    PairsCounts rest{};
    auto const& digits = index.digits();
    for (size_t k = digits.size(); k-- > 0;) {
        rest.values[digits[k].first] = uint8_t(number / digits[k].second);
        number %= digits[k].second;
    }
    return rest;
}

// Counts the rests `index` numbers `rests` on `counter`, each of `feeders`
// threads a batch at a time, into `rest_table`.
void count_rests(GpuRowCounter const& counter, RemainderIndex const& index,
                 std::vector<uint64_t> const& rests, size_t feeders,
                 RestTable const& rest_table) {
    std::atomic<size_t> next{0};
    run_threads(feeders, [&](size_t) {
        std::vector<uint8_t> letters(GPU_BATCH_ROWS * GPU_LETTERS_BYTES);
        std::vector<GpuCounts> counts(GPU_BATCH_ROWS);
        for (size_t first;
             (first = next.fetch_add(GPU_BATCH_ROWS)) < rests.size();) {
            size_t const rows = std::min(GPU_BATCH_ROWS, rests.size() - first);
            for (size_t r = 0; r < rows; ++r) {
                PairsCounts const rest = numbered_rest(index, rests[first + r]);
                std::copy(rest.values.begin(), rest.values.end(),
                          letters.begin() + r * GPU_LETTERS_BYTES);
            }
            counter.count(letters.data(), rows, counts.data());
            for (size_t r = 0; r < rows; ++r)
                rest_table.put(numbered_rest(index, rests[first + r]),
                               gpu_counts(counts[r]));
        }
    });
}

// Keeps the RestTables the row walk looks rests up in filled ahead of it,
// on a thread of its own: finds each range of blocks' distinct rests on
// the GPU and counts them there into a table before the walkers reach the
// range. One table holds every rest when they fit it; else two half tables
// take turns, a range the blocks of at most a table's rows, and every pass
// counts each range again.
class RestFeeder {
  public:
    // A table a walker holds until release().
    struct Lease {
        RestTable const* table = nullptr;
        size_t slot = 0;
    };

    RestFeeder(BagStatsArgs const& args, BagStatsInputs const& inputs,
               bool enabled, size_t megabytes, size_t streams)
        : args_(args), inputs_(inputs),
          index_(inputs.bag, UINT64_MAX - 1), streams_(streams),
          megabytes_(megabytes) {
        if (!enabled || megabytes == 0 || index_.total() == 0) return;
        if (inputs.steps.size() > 1) {
            leftover_inputs(inputs, args, &gpu_, &sorted_);
            number_inputs(inputs, index_, sorted_, &gpu_);
        }
        size_t const units = inputs.steps[0]->size();
        whole_ = distinct_rests(0, units, rest_cache_entries(megabytes),
                                &rests_);
        enabled_ = true;
    }

    ~RestFeeder() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        changed_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    bool enabled() const { return enabled_; }

    // Whether one table holds every rest.
    bool whole() const { return whole_; }

    // The distinct rests found, when whole().
    size_t rests() const { return rests_.size(); }

    // How many ranges the blocks are in.
    size_t ranges() const { return ranges_.size() - 1; }

    // Starts filling the tables for `blocks`, `unit_work` as census_blocks()
    // gives it, counting on `counter`.
    void start(GpuRowCounter const& counter,
               std::vector<size_t> const& blocks,
               std::vector<uint64_t> const& unit_work, bool report_full) {
        counter_ = &counter;
        size_t const tables = whole_ ? 1 : 2;
        for (size_t t = 0; t < tables; ++t)
            tables_.push_back(std::make_unique<RestTable>(
                PROGRAM, inputs_.bag, true, megabytes_ / tables,
                report_full && whole_));
        size_t const capacity = tables_[0]->capacity();
        ranges_ = {blocks.front()};
        if (!whole_) {
            uint64_t rows = 0;
            for (size_t b = 0; b + 1 < blocks.size(); ++b) {
                uint64_t const more =
                    unit_work[blocks[b + 1]] - unit_work[blocks[b]];
                if (rows > 0 && rows + more > capacity) {
                    ranges_.push_back(blocks[b]);
                    rows = 0;
                }
                rows += more;
            }
        }
        ranges_.push_back(blocks.back());
        slots_.resize(tables);
        if (whole_) {
            count_rests(counter, index_, rests_, streams_, *tables_[0]);
            std::vector<uint64_t>().swap(rests_);
            slots_[0] = {0, true, 0};
            return;
        }
        thread_ = std::thread([this] { feed(); });
    }

    // The table with the rests of the rows whose first pick is `unit`,
    // waiting for it to fill.
    Lease acquire(size_t unit) {
        if (!enabled_) return {};
        size_t const range = size_t(
            std::upper_bound(ranges_.begin(), ranges_.end(), unit) -
            ranges_.begin()) - 1;
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            for (size_t s = 0; s < slots_.size(); ++s) {
                if (slots_[s].range != range || !slots_[s].ready) continue;
                ++slots_[s].users;
                size_t const ahead = (range + 1) % ranges();
                if (ahead_ != ahead) {
                    ahead_ = ahead;
                    changed_.notify_all();
                }
                return {tables_[s].get(), s};
            }
            if (wanted_ == NONE || range < wanted_) {
                wanted_ = range;
                changed_.notify_all();
            }
            changed_.wait(lock);
        }
    }

    void release(Lease const& lease) {
        if (lease.table == nullptr) return;
        bool free = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            free = --slots_[lease.slot].users == 0;
        }
        if (free) changed_.notify_all();
    }

  private:
    static constexpr size_t NONE = SIZE_MAX;

    // A table's range, whether it is filled, and the walkers holding it.
    struct Slot {
        size_t range = NONE;
        bool ready = false;
        size_t users = 0;
    };

    // The numbers of the distinct rests of the rows whose first pick is one
    // of `units` units from `first`, as many as `capacity`; false when
    // there are more.
    bool distinct_rests(size_t first, size_t units, size_t capacity,
                        std::vector<uint64_t>* out) const {
        out->clear();
        if (inputs_.steps.size() < 2) {
            PairsCounts rest;
            for (size_t unit = first; unit < first + units; ++unit)
                if (pairs_subtract(inputs_.bag,
                                   (*inputs_.steps[0])[unit].counts, &rest))
                    out->push_back(index_.number(rest));
            return out->size() <= capacity;
        }
        return run_distinct_rests_gpu(PROGRAM, gpu_, first, units, capacity,
                                      out);
    }

    bool held(size_t range) const {
        for (Slot const& slot : slots_)
            if (slot.range == range) return true;
        return false;
    }

    // The next range to fill, a walker's first, and a free table for it;
    // false when there is neither.
    bool next(size_t* range, size_t* slot) const {
        for (size_t const target : {wanted_, ahead_}) {
            if (target == NONE || held(target)) continue;
            for (size_t s = 0; s < slots_.size(); ++s) {
                Slot const& other = slots_[s];
                if (other.users != 0 ||
                    (other.range != NONE &&
                     (other.range == wanted_ || other.range == ahead_)))
                    continue;
                *range = target;
                *slot = s;
                return true;
            }
        }
        return false;
    }

    void feed() {
        std::vector<uint64_t> rests;
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            size_t range = 0;
            size_t slot = 0;
            changed_.wait(lock, [&] { return stop_ || next(&range, &slot); });
            if (stop_) return;
            slots_[slot] = {range, false, 0};
            lock.unlock();
            RestTable& table = *tables_[slot];
            table.clear();
            distinct_rests(ranges_[range],
                           ranges_[range + 1] - ranges_[range],
                           table.capacity(), &rests);
            count_rests(*counter_, index_, rests, streams_, table);
            lock.lock();
            slots_[slot].ready = true;
            if (wanted_ == range) wanted_ = NONE;
            changed_.notify_all();
        }
    }

    BagStatsArgs const& args_;
    BagStatsInputs const& inputs_;
    RemainderIndex const index_;
    size_t const streams_;
    size_t const megabytes_;
    GpuLeftoverInputs gpu_;
    std::vector<size_t> sorted_;
    std::vector<uint64_t> rests_;
    bool enabled_ = false;
    bool whole_ = false;
    GpuRowCounter const* counter_ = nullptr;
    std::vector<std::unique_ptr<RestTable>> tables_;
    // Range r is units [ranges_[r], ranges_[r + 1]).
    std::vector<size_t> ranges_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Slot> slots_;
    size_t wanted_ = NONE;
    size_t ahead_ = NONE;
    bool stop_ = false;
    std::thread thread_;
};

// Walks the rows as PickWalk does, buffering each row's picks and rest, and
// counts the buffer on the GPU when it holds GPU_BATCH_ROWS rows and at the
// end of a walk. Looks each rest up in `feeder`'s table for the walk's
// units, counting only the rests it lacks.
class GpuRowWalker : public PickWalk {
  public:
    GpuRowWalker(std::vector<std::vector<PairBag> const*> const& steps,
                 int pair_steps, size_t pair_bags,
                 GpuRowCounter const& counter, RestFeeder* feeder)
        : PickWalk(steps, pair_steps, pair_bags), counter_(counter),
          feeder_(feeder) {}

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
        RestFeeder::Lease const lease = feeder_->acquire(first);
        rest_table_ = lease.table;
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
        feeder_->release(lease);
    }

  private:
    bool lookup(PairsCounts const& rest, BagStatsCounts* out) {
        if (rest_table_ == nullptr) return false;
        ++rest_counts_.lookups;
        if (!rest_table_->get(rest, out)) return false;
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
                if (rest_table_ == nullptr) continue;
                PairsCounts rest;
                std::copy_n(rests_.begin() + m * GPU_LETTERS_BYTES,
                            GPU_LETTERS_BYTES, rest.values.begin());
                rest_table_->put(rest, row);
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
    RestFeeder* const feeder_;
    // The table of the block being walked, or null.
    RestTable const* rest_table_ = nullptr;
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
    explicit GpuCacheHooks(bool enabled) : enabled_(enabled) {}

    void finished(GpuRowWalker* walker) {
        cache_.add(walker->take_rest_counts());
    }

    // ", cache hits N/M (P%)" once there have been lookups, ", cache off"
    // when there is no table, else nothing.
    std::string text() const {
        if (cache_.lookups.load(std::memory_order_relaxed) != 0)
            return cache_.text("cache");
        return enabled_ ? "" : ", cache off";
    }

  private:
    bool const enabled_;
    SharedCacheCounts cache_;
};

struct Walk : WalkBase {
    GpuRowCounter const& counter;
    RestFeeder& feeder;

    GpuRowWalker walker(int) const {
        return GpuRowWalker(steps, args.search.pairs, pair_bags.size(),
                            counter, &feeder);
    }

    GpuCacheHooks progress_hooks() const {
        return GpuCacheHooks(feeder.enabled());
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

// The --unique output of `inputs` for `args`, walked on the GPU.
int run_pair_usage(BagStatsArgs const& args, BagStatsInputs const& inputs) {
    GpuLeftoverInputs gpu;
    std::vector<size_t> sorted;
    leftover_inputs(inputs, args, &gpu, &sorted);
    RemainderIndex const index(inputs.bag, UINT64_MAX - 1);
    if (index.total() != 0) number_inputs(inputs, index, sorted, &gpu);
    std::vector<uint64_t> by_position;
    with_walk_progress(args, inputs, [&](GpuWalkProgress const& progress) {
        run_pair_usage_gpu(PROGRAM, gpu, progress, &by_position);
    });
    size_t const pair_bags = size_t(gpu.pair_bags);
    std::vector<uint64_t> counts(sorted.size(), 0);
    for (size_t position = 0; position < sorted.size(); ++position)
        counts[sorted[position] + (position < pair_bags ? 0 : pair_bags)] =
            by_position[position];
    std::vector<BagStatsPairUsage> used;
    rank_pair_usage(args, {&inputs.pair_rows, &inputs.prospect_rows}, counts,
                    &used);
    return print_pair_usage(args, used);
}

}  // namespace

int main(int argc, char* argv[]) {
    BagStatsArgs args;
    CudaOptions cuda;
    bool help = false;
    std::string letters;
    if (!parse_args(argv, &args, &cuda, &help) ||
        (!help && !remaining_letters(args, &letters))) {
        usage(argv[0], stderr);
        return 2;
    }
    if (help) {
        usage(argv[0], stdout);
        return 0;
    }

    auto const start = std::chrono::steady_clock::now();
    auto const stage = [&](char const* what) {
        if (args.progress == 0) return;
        std::chrono::duration<double> const elapsed =
            std::chrono::steady_clock::now() - start;
        report(PROGRAM, "[%.1fs] %s", elapsed.count(), what);
    };

    BagStatsInputs inputs(pairs_letter_counts(letters));
    if (letter_total(inputs.bag) > GPU_MAX_LETTERS) {
        report(PROGRAM, "LETTERS has more than %d letters", GPU_MAX_LETTERS);
        return 1;
    }
    stage("loading inputs");
    if (!load_bag_stats_inputs(PROGRAM, args, &inputs, nullptr)) return 1;
    if (args.leftovers) {
        stage("walking");
        return run_leftovers(args, letters, inputs);
    }
    if (args.pair_usage) {
        stage("walking");
        return run_pair_usage(args, inputs);
    }
    size_t const threads = bag_stats_threads(args);
    BagStatsRankers rankers(args, inputs);
    std::vector<std::vector<PairBag> const*> const& steps = inputs.steps;
    bool const one_row = !args.rank.score.empty() && args.rank.sorts.empty();
    if (!one_row && args.cache_size_mb != 0)
        stage("finding distinct remainders");
    RestFeeder feeder(args, inputs, !one_row, size_t(args.cache_size_mb),
                      size_t(cuda.streams));
    stage("building GPU tables and pair-sum filter");
    GpuRowCounter const counter(PROGRAM, gpu_inputs(inputs, args),
                                size_t(cuda.streams));
    if (args.progress != 0) {
        report(PROGRAM, "%s, %d-bit bag keys, %.1f MB of GPU memory",
               counter.device().c_str(), counter.key_bits(),
               double(counter.bytes()) / (1024 * 1024));
        GpuFilterInfo const filter = counter.filter_info();
        report(PROGRAM,
               "pair-sum filter: %llu group pairs, longest sum %d letters, "
               "%.1f MB (%.1f bits per pair), built in %.1fs",
               (unsigned long long)filter.pairs, filter.longest_sum,
               double(filter.bytes) / (1024 * 1024),
               filter.pairs == 0 ? 0.0
                                 : double(filter.bytes) * 8 / filter.pairs,
               filter.seconds);
    }

    if (!one_row) stage("counting each unit's rows");
    std::vector<uint64_t> unit_work;
    std::vector<size_t> blocks =
        one_row ? unit_blocks(steps[0]->size())
                : census_blocks(steps, args.search.pairs, inputs.bag, threads,
                                GPU_BATCH_ROWS, &unit_work);
    if (feeder.enabled()) {
        stage("counting distinct remainders");
        feeder.start(counter, blocks, unit_work, args.progress != 0);
        if (args.progress != 0 && feeder.whole())
            report(PROGRAM, "cache holds every distinct remainder");
        if (args.progress != 0 && !feeder.whole())
            report(PROGRAM, "more distinct remainders than the cache's "
                   "%zu entries: counting them in %zu ranges of rows, each "
                   "pass", rest_cache_entries(size_t(args.cache_size_mb)),
                   feeder.ranges());
    }
    Walk const walk{{PROGRAM, steps, inputs.pair_bags, inputs.prospect_bags,
                     inputs.bag, inputs.word_bags, args,
                     feeder.enabled() ? threads : size_t(cuda.streams),
                     rankers.passes ? 2 : 1, std::move(blocks),
                     std::move(unit_work)},
                    counter, feeder};
    stage("walking");
    int const status = run_bag_stats(walk, letters, &rankers);
    if (args.progress != 0) {
        GpuFilterCounts const filtered = counter.filter_counts();
        report(PROGRAM, "pair-sum filter: scanned %llu of %llu leftovers "
               "(%.1f%%)",
               (unsigned long long)filtered.scanned,
               (unsigned long long)filtered.leftovers,
               filtered.leftovers == 0
                   ? 0.0
                   : 100.0 * double(filtered.scanned) / filtered.leftovers);
    }
    return status;
}
