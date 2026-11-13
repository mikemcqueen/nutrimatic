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
                        [&](size_t done, size_t units, double share) {
        progress.measured(share, units);
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

// Counts the rests `index` numbers `rests` on `counter`, a chunk at a time:
// `feeders` threads of their own, one a stream of `counter`, feed the GPU a
// chunk's batches while `threads` threads decode the next chunk's rests and
// call store(i, letters, counts) for each rest i of the chunk before,
// `letters` its GPU_LETTERS_BYTES letter counts.
template <typename Store>
void count_rests(GpuRowCounter const& counter, RemainderIndex const& index,
                 std::vector<uint64_t> const& rests, size_t threads,
                 size_t feeders, Store const& store) {
    constexpr size_t CHUNK_ROWS = 16 * GPU_BATCH_ROWS;
    constexpr size_t SHARE_ROWS = 4096;
    struct Chunk {
        size_t first = 0;
        size_t rows = 0;
        std::vector<uint8_t> letters;
        std::vector<GpuCounts> counts;
    };
    size_t const chunk_rows = std::min(CHUNK_ROWS, rests.size());
    Chunk chunks[3];
    for (Chunk& chunk : chunks) {
        chunk.letters.resize(chunk_rows * GPU_LETTERS_BYTES);
        chunk.counts.resize(chunk_rows);
    }
    Chunk* decoded = &chunks[0];
    Chunk* counted = &chunks[1];
    Chunk* stored = &chunks[2];
    size_t next_first = 0;
    while (true) {
        std::swap(stored, counted);
        std::swap(counted, decoded);
        decoded->first = next_first;
        decoded->rows = std::min(chunk_rows, rests.size() - next_first);
        next_first += decoded->rows;
        if (decoded->rows == 0 && counted->rows == 0 && stored->rows == 0)
            return;
        std::atomic<size_t> next_batch{0};
        std::vector<std::thread> feeding;
        for (size_t f = 0; f < feeders; ++f)
            feeding.emplace_back([&] {
                for (size_t at; (at = next_batch.fetch_add(GPU_BATCH_ROWS)) <
                                counted->rows;)
                    counter.count(
                        counted->letters.data() + at * GPU_LETTERS_BYTES,
                        std::min(GPU_BATCH_ROWS, counted->rows - at),
                        counted->counts.data() + at);
            });
        std::atomic<size_t> next_decode{0};
        std::atomic<size_t> next_store{0};
        run_threads(threads, [&](size_t) {
            for (size_t at;
                 (at = next_decode.fetch_add(SHARE_ROWS)) < decoded->rows;) {
                size_t const end = std::min(at + SHARE_ROWS, decoded->rows);
                for (size_t r = at; r < end; ++r) {
                    PairsCounts const rest =
                        numbered_rest(index, rests[decoded->first + r]);
                    std::copy(rest.values.begin(), rest.values.end(),
                              decoded->letters.begin() +
                                  r * GPU_LETTERS_BYTES);
                }
            }
            for (size_t at;
                 (at = next_store.fetch_add(SHARE_ROWS)) < stored->rows;) {
                size_t const end = std::min(at + SHARE_ROWS, stored->rows);
                for (size_t r = at; r < end; ++r)
                    store(stored->first + r,
                          stored->letters.data() + r * GPU_LETTERS_BYTES,
                          stored->counts[r]);
            }
        });
        for (std::thread& feeder : feeding) feeder.join();
        stored->rows = 0;
    }
}

// count_rests() into `rest_table`.
void count_rests(GpuRowCounter const& counter, RemainderIndex const& index,
                 std::vector<uint64_t> const& rests, size_t threads,
                 RestTable const& rest_table) {
    count_rests(counter, index, rests, threads, 1,
                [&](size_t, uint8_t const* letters, GpuCounts const& counts) {
        PairsCounts rest;
        std::copy_n(letters, GPU_LETTERS_BYTES, rest.values.begin());
        rest_table.put(rest, gpu_counts(counts));
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
               bool enabled, size_t megabytes, size_t threads)
        : args_(args), inputs_(inputs),
          index_(inputs.bag, UINT64_MAX - 1), threads_(threads),
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
            count_rests(counter, index_, rests_, threads_, *tables_[0]);
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
            count_rests(*counter_, index_, rests, threads_, table);
            lock.lock();
            slots_[slot].ready = true;
            if (wanted_ == range) wanted_ = NONE;
            changed_.notify_all();
        }
    }

    BagStatsArgs const& args_;
    BagStatsInputs const& inputs_;
    RemainderIndex const index_;
    size_t const threads_;
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

// A PickWalk set to a row's picks, to name them.
class PickNamer : public PickWalk {
  public:
    using PickWalk::PickWalk;

    void set(uint32_t const* places) {
        for (size_t j = 0; j < chosen_.size(); ++j) chosen_[j] = places[j];
    }
};

// The streams print_top_by_rest() counts rests on, at least.
constexpr size_t REST_STREAMS = 3;

// The most rows tied at the last kept sort key that are taken from the GPU
// without first counting them by unit.
constexpr uint64_t TIES_UNCOUNTED = uint64_t(1) << 22;

// The host bytes a rest of a range takes while the range is ranked.
constexpr size_t RANGE_REST_BYTES = 88;

// The rests of the rows of a range of units: their numbers, the rows that
// leave each, their counts and their rows' pair letters.
struct RangeRests {
    std::vector<uint64_t> numbers;
    std::vector<uint64_t> rows;
    std::vector<BagStatsCounts> counts;
    std::vector<uint8_t> pair_lengths;
};

using RestKey = std::pair<double, double>;

// Runs work(begin, end) over [0, count) in parts, each on one of at most
// `threads` threads; the parts it is in.
template <typename Work>
size_t run_parts(size_t count, size_t threads, Work const& work) {
    size_t const parts = std::max<size_t>(std::min(threads, count >> 16), 1);
    std::atomic<size_t> next{0};
    run_threads(parts, [&](size_t) {
        for (size_t part; (part = next.fetch_add(1)) < parts;)
            work(count * part / parts, count * (part + 1) / parts);
    });
    return parts;
}

// Sorts [first, last) by `before`: in runs on at most `threads` threads,
// then merged pairwise.
template <typename Iterator, typename Before>
void sort_on_threads(Iterator first, Iterator last, size_t threads,
                     Before const& before) {
    size_t const count = size_t(last - first);
    size_t const runs = run_parts(count, threads, [&](size_t begin,
                                                      size_t end) {
        std::sort(first + begin, first + end, before);
    });
    auto const run_start = [&](size_t run) { return count * run / runs; };
    for (size_t span = 1; span < runs; span *= 2) {
        std::atomic<size_t> next{0};
        run_threads(std::max<size_t>(runs / (2 * span), 1), [&](size_t) {
            for (size_t run;
                 (run = next.fetch_add(2 * span)) + span < runs;)
                std::inplace_merge(
                    first + run_start(run), first + run_start(run + span),
                    first + run_start(std::min(run + 2 * span, runs)),
                    before);
        });
    }
}

// Sorts `numbers` on `threads` threads, each one's entry of `rows` with it.
void sort_rests(std::vector<uint64_t>* numbers, std::vector<uint64_t>* rows,
                size_t threads) {
    size_t const count = numbers->size();
    std::vector<std::pair<uint64_t, uint64_t>> both(count);
    run_parts(count, threads, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i)
            both[i] = {(*numbers)[i], (*rows)[i]};
    });
    sort_on_threads(both.begin(), both.end(), threads, std::less<>());
    run_parts(count, threads, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            (*numbers)[i] = both[i].first;
            (*rows)[i] = both[i].second;
        }
    });
}

// Where split_top() leaves the rests: those before kept_end have every row
// in the top, those from there to tied_end share the last kept key and have
// tie_rows of their tied_rows rows in it.
struct TopSplit {
    size_t kept_end;
    size_t tied_end;
    uint64_t tie_rows;
    uint64_t tied_rows;
};

// Reorders `order`, indexes into `keys` and `rows`, so the rests whose rows
// are the `top` that rank first come first.
TopSplit split_top(std::vector<uint32_t>* order,
                   std::vector<RestKey> const& keys,
                   std::vector<uint64_t> const& rows, uint64_t top) {
    size_t const count = order->size();
    uint64_t all_rows = 0;
    for (uint32_t const i : *order) all_rows += rows[i];
    if (all_rows <= top) return {count, count, 0, 0};
    size_t low = 0;
    size_t high = count;
    uint64_t need = top;
    while (true) {
        RestKey const a = keys[(*order)[low]];
        RestKey const b = keys[(*order)[low + (high - low) / 2]];
        RestKey const c = keys[(*order)[high - 1]];
        RestKey const pivot =
            std::max(std::min(a, b), std::min(std::max(a, b), c));
        auto const first = order->begin() + low;
        auto const last = order->begin() + high;
        auto const equal = std::partition(first, last, [&](uint32_t i) {
            return keys[i] > pivot;
        });
        auto const below = std::partition(equal, last, [&](uint32_t i) {
            return keys[i] == pivot;
        });
        uint64_t above_rows = 0;
        for (auto at = first; at != equal; ++at) above_rows += rows[*at];
        uint64_t equal_rows = 0;
        for (auto at = equal; at != below; ++at) equal_rows += rows[*at];
        if (need <= above_rows) {
            high = size_t(equal - order->begin());
        } else if (need < above_rows + equal_rows) {
            return {size_t(equal - order->begin()),
                    size_t(below - order->begin()), need - above_rows,
                    equal_rows};
        } else if (need == above_rows + equal_rows) {
            size_t const end = size_t(below - order->begin());
            return {end, end, 0, 0};
        } else {
            need -= above_rows + equal_rows;
            low = size_t(below - order->begin());
        }
    }
}

// Prints the -n rows of `walk` without walking them on the host. The rows
// are taken a range of units at a time, one range when the device and
// --cache-size hold every distinct rest: the GPU walk finds the range's
// distinct rests and the rows that leave each, the rests are counted, and
// each stands for its rows in the sort value counts and then in the top.
// The GPU walk then writes out only the rows at or before the last kept
// sort key.
void print_top_by_rest(Walk const& walk, BagStatsInputs const& inputs,
                       Ranker* ranker, bool passes,
                       std::function<void(char const*)> const& stage) {
    BagStatsArgs const& args = walk.args;
    size_t const threads = walk.threads;
    size_t const units = walk.steps[0]->size();
    uint64_t const top = uint64_t(args.rank.top);
    int const bag_letters = letter_total(walk.bag);
    GpuLeftoverInputs gpu;
    std::vector<size_t> sorted;
    leftover_inputs(inputs, args, &gpu, &sorted);
    RemainderIndex const index(inputs.bag, UINT64_MAX - 1);
    number_inputs(inputs, index, sorted, &gpu);
    size_t const device_capacity = gpu_rest_set_capacity(true);
    size_t const cache_capacity = std::max<size_t>(
        size_t(args.cache_size_mb) * 1024 * 1024 / RANGE_REST_BYTES,
        size_t(1) << 16);
    size_t const capacity = std::min(device_capacity, cache_capacity);

    // Range r is units [bounds[r], bounds[r + 1]).
    stage("finding distinct remainders");
    std::vector<size_t> bounds{0};
    RangeRests whole;
    bool const one_range = run_distinct_rests_gpu(
        PROGRAM, gpu, 0, units, capacity, &whole.numbers, &whole.rows);
    if (one_range) {
        bounds.push_back(units);
        if (args.progress != 0)
            report(PROGRAM, "distinct remainders: %zu in one range (optimal), "
                   "of the %zu the device and --cache-size hold",
                   whole.numbers.size(), capacity);
    } else {
        whole = {};
        std::vector<uint64_t> unit_rows;
        run_top_rows_gpu(PROGRAM, gpu, 0, units, {}, {}, 0, 0, nullptr,
                         &unit_rows);
        std::vector<uint64_t> before(units + 1, 0);
        for (size_t unit = 0; unit < units; ++unit)
            before[unit + 1] = before[unit] + unit_rows[unit];
        // The rows a rest, guessed: a range takes that many times the rests
        // a set holds, and half as many when its rests don't fit.
        double share = 4;
        for (size_t first = 0; first < units;) {
            uint64_t const most =
                before[first] + uint64_t(double(capacity) * share);
            size_t end = size_t(
                std::upper_bound(before.begin() + first + 1, before.end(),
                                 most) - before.begin()) - 1;
            end = std::min(std::max(end, first + 1), units);
            size_t found = 0;
            if (run_distinct_rests_gpu(PROGRAM, gpu, first, end - first,
                                       capacity, nullptr, nullptr, &found)) {
                bounds.push_back(end);
                if (found != 0)
                    share = std::max(
                        1.0, 0.8 * double(before[end] - before[first]) /
                                 double(found));
                first = end;
                continue;
            }
            if (end == first + 1) {
                report(PROGRAM, "one pair bag's rows leave more than %zu "
                       "distinct remainders; give a larger --cache-size",
                       capacity);
                exit(1);
            }
            share /= 2;
        }
        if (args.progress != 0)
            report(PROGRAM, "more distinct remainders than the %zu the "
                   "%s holds (device %zu, --cache-size %zu): counting them "
                   "in %zu ranges of rows (constrained)", capacity,
                   device_capacity < cache_capacity ? "device"
                                                    : "--cache-size",
                   device_capacity, cache_capacity, bounds.size() - 1);
    }
    size_t const ranges = bounds.size() - 1;

    constexpr size_t SHARE = 1 << 14;
    auto const each = [&](size_t count, auto const& visit) {
        std::atomic<size_t> next{0};
        run_threads(threads, [&](size_t t) {
            for (size_t at; (at = next.fetch_add(SHARE)) < count;)
                for (size_t i = at; i < std::min(at + SHARE, count); ++i)
                    visit(t, i);
        });
    };
    // Range r's rests, counted. One range's are kept between sweeps. With
    // more, each range's rests are sorted by number, and the first sweep
    // keeps the counts of as many ranges as --cache-size holds, packed, for
    // the second.
    struct Packed {
        uint32_t counts[6];
        uint8_t pair_length;
    } __attribute__((packed));
    RangeRests scratch;
    std::vector<std::vector<Packed>> packed(ranges);
    size_t packed_room =
        size_t(args.cache_size_mb) * 1024 * 1024 / sizeof(Packed);
    auto const range_rests = [&](size_t r, bool keep) -> RangeRests const& {
        RangeRests& range = one_range ? whole : scratch;
        if (one_range && !range.counts.empty()) return range;
        if (!one_range) {
            if (!run_distinct_rests_gpu(PROGRAM, gpu, bounds[r],
                                        bounds[r + 1] - bounds[r], capacity,
                                        &range.numbers, &range.rows)) {
                report(PROGRAM, "the device no longer holds the distinct "
                       "remainders of a range of rows; its free memory "
                       "fell during the run");
                exit(1);
            }
            sort_rests(&range.numbers, &range.rows, threads);
        }
        size_t const count = range.numbers.size();
        range.counts.resize(count);
        range.pair_lengths.resize(count);
        if (!packed[r].empty()) {
            each(count, [&](size_t, size_t i) {
                Packed const& from = packed[r][i];
                range.counts[i] = {from.counts[0], from.counts[1],
                                   from.counts[2], from.counts[3],
                                   from.counts[4], from.counts[5]};
                range.pair_lengths[i] = from.pair_length;
            });
            std::vector<Packed>().swap(packed[r]);
            return range;
        }
        count_rests(walk.counter, index, range.numbers, threads,
                    walk.counter.streams(),
                    [&](size_t i, uint8_t const* letters,
                        GpuCounts const& counts) {
            range.counts[i] = gpu_counts(counts);
            int left = 0;
            for (size_t k = 0; k < GPU_LETTERS_BYTES; ++k) left += letters[k];
            range.pair_lengths[i] = uint8_t(bag_letters - left);
        });
        if (!keep || one_range || count == 0 || count > packed_room)
            return range;
        std::atomic<bool> wide{false};
        std::vector<Packed> small(count);
        each(count, [&](size_t, size_t i) {
            BagStatsCounts const& from = range.counts[i];
            uint64_t const values[6] = {
                from.word_bags, from.with_pairs, from.total_pairs,
                from.max_pairs, from.solo, from.with_min_pairs};
            for (int k = 0; k < 6; ++k) {
                if (values[k] > UINT32_MAX)
                    wide.store(true, std::memory_order_relaxed);
                small[i].counts[k] = uint32_t(values[k]);
            }
            small[i].pair_length = range.pair_lengths[i];
        });
        if (!wide.load()) {
            packed_room -= count;
            packed[r] = std::move(small);
        }
        return range;
    };
    auto const started = std::chrono::steady_clock::now();
    double next_report = double(args.progress);
    auto const progress = [&](int pass, size_t r) {
        if (args.progress == 0 || ranges == 1) return;
        double const elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        if (elapsed < next_report && r + 1 < ranges) return;
        next_report = elapsed + double(args.progress);
        report(PROGRAM, "pass %d/%d: %zu/%zu ranges, %s", pass,
               passes ? 2 : 1, r + 1, ranges,
               format_duration(elapsed).c_str());
    };

    if (passes) {
        stage("counting distinct remainders' sort values");
        std::vector<Ranker::Part> parts(threads, ranker->part());
        for (size_t r = 0; r < ranges; ++r) {
            RangeRests const& range = range_rests(r, true);
            each(range.numbers.size(), [&](size_t t, size_t i) {
                ranker->add(&parts[t], counts_row(range.counts[i]),
                            range.pair_lengths[i], {}, range.rows[i]);
            });
            progress(1, r);
        }
        ranker->finish(parts);
    }

    // The rests that may have rows in the top, grouped by range in order.
    stage("ranking distinct remainders");
    std::vector<uint64_t> top_numbers;
    std::vector<uint64_t> top_rows;
    std::vector<BagStatsCounts> top_counts;
    std::vector<RestKey> top_keys;
    std::vector<uint32_t> top_ranges;
    std::vector<uint8_t> top_kinds;
    // Keeps of them the ones whose rows are the `top` that rank first: kind
    // 1, every row, or 2, tied at the last kept key, from the first ranges
    // that hold the tied rows the top takes. Returns how many those are.
    auto const trim = [&]() {
        size_t const count = top_keys.size();
        std::vector<uint32_t> order(count);
        std::iota(order.begin(), order.end(), 0u);
        TopSplit const split = split_top(&order, top_keys, top_rows, top);
        std::vector<uint8_t> kinds(count, 0);
        for (size_t k = 0; k < split.tied_end; ++k)
            kinds[order[k]] = k < split.kept_end ? 1 : 2;
        uint64_t ties_before = 0;
        size_t out = 0;
        for (size_t i = 0; i < count;) {
            uint32_t const r = top_ranges[i];
            bool const keep_ties = ties_before < split.tie_rows;
            for (; i < count && top_ranges[i] == r; ++i) {
                if (kinds[i] == 0 || (kinds[i] == 2 && !keep_ties)) continue;
                if (kinds[i] == 2) ties_before += top_rows[i];
                top_numbers[out] = top_numbers[i];
                top_rows[out] = top_rows[i];
                top_counts[out] = top_counts[i];
                top_keys[out] = top_keys[i];
                top_ranges[out] = top_ranges[i];
                top_kinds[out] = kinds[i];
                ++out;
            }
        }
        top_numbers.resize(out);
        top_rows.resize(out);
        top_counts.resize(out);
        top_keys.resize(out);
        top_ranges.resize(out);
        top_kinds.resize(out);
        return split.tie_rows;
    };
    uint64_t tie_rows = 0;
    for (size_t r = 0; r < ranges; ++r) {
        RangeRests const& range = range_rests(r, false);
        size_t const count = range.numbers.size();
        std::vector<RestKey> keys(count);
        each(count, [&](size_t, size_t i) {
            keys[i] = ranker->key(counts_row(range.counts[i]),
                                  range.pair_lengths[i], {});
        });
        std::vector<uint32_t> order(count);
        std::iota(order.begin(), order.end(), 0u);
        TopSplit const split = split_top(&order, keys, range.rows, top);
        for (size_t k = 0; k < split.tied_end; ++k) {
            uint32_t const i = order[k];
            top_numbers.push_back(range.numbers[i]);
            top_rows.push_back(range.rows[i]);
            top_counts.push_back(range.counts[i]);
            top_keys.push_back(keys[i]);
            top_ranges.push_back(uint32_t(r));
        }
        top_kinds.resize(top_keys.size());
        tie_rows = trim();
        progress(passes ? 2 : 1, r);
    }
    whole = {};
    scratch = {};
    if (top_keys.size() >= size_t(1) << 30) {
        report(PROGRAM, "too many distinct remainders tied in the top rows");
        exit(1);
    }

    stage("walking the top rows");
    size_t const steps = walk.steps.size();
    size_t const width = steps + 1;
    std::vector<uint32_t> rows;
    for (size_t first = 0; first < top_keys.size();) {
        size_t const r = top_ranges[first];
        size_t end = first;
        uint64_t kept_rows = 0;
        uint64_t tied_rows = 0;
        for (; end < top_keys.size() && top_ranges[end] == r; ++end)
            (top_kinds[end] == 1 ? kept_rows : tied_rows) += top_rows[end];
        bool const every_tie = tied_rows <= tie_rows;
        size_t slots = 1024;
        while (slots < 2 * (end - first)) slots *= 2;
        std::vector<uint64_t> table_keys(slots, 0);
        std::vector<uint32_t> table_values(slots, 0);
        for (size_t k = first; k < end; ++k) {
            if (top_kinds[k] == 2 && tie_rows == 0) continue;
            uint64_t const key = top_numbers[k] + 1;
            size_t slot = size_t(mix_bits(key)) & (slots - 1);
            while (table_keys[slot] != 0) slot = (slot + 1) & (slots - 1);
            table_keys[slot] = key;
            table_values[slot] = uint32_t(k) << 2 |
                (top_kinds[k] == 1 || every_tie ? 1 : 2);
        }
        size_t const range_units = bounds[r + 1] - bounds[r];
        uint32_t last_unit = UINT32_MAX;
        uint64_t tie_capacity = tie_rows == 0 ? 0 : tied_rows;
        if (!every_tie && tie_rows != 0 &&
            tied_rows > std::max(top, TIES_UNCOUNTED)) {
            std::vector<uint64_t> unit_ties;
            run_top_rows_gpu(PROGRAM, gpu, bounds[r], range_units, table_keys,
                             table_values, 0, 0, nullptr, &unit_ties);
            uint64_t before = 0;
            for (size_t unit = bounds[r]; unit < bounds[r + 1]; ++unit) {
                before += unit_ties[unit];
                if (before < tie_rows) continue;
                last_unit = uint32_t(unit);
                break;
            }
            tie_capacity = before;
        }
        std::vector<uint32_t> range_rows;
        if (!run_top_rows_gpu(PROGRAM, gpu, bounds[r], range_units,
                              table_keys, table_values, last_unit,
                              kept_rows + tie_capacity, &range_rows,
                              nullptr)) {
            report(PROGRAM, "the GPU walk found more top rows than the "
                   "remainders' row counts give");
            exit(1);
        }
        rows.insert(rows.end(), range_rows.begin(), range_rows.end());
        tie_rows -= std::min(tie_rows, tied_rows);
        first = end;
    }

    stage("sorting and printing the top rows");
    size_t const found = rows.size() / width;
    std::vector<uint32_t> ranked(found);
    std::iota(ranked.begin(), ranked.end(), 0u);
    auto const before = [&](uint32_t a, uint32_t b) {
        uint32_t const* const row_a = &rows[a * width];
        uint32_t const* const row_b = &rows[b * width];
        RestKey const& key_a = top_keys[row_a[steps] >> 2];
        RestKey const& key_b = top_keys[row_b[steps] >> 2];
        if (key_a != key_b) return key_a > key_b;
        return std::lexicographical_compare(row_a, row_a + steps, row_b,
                                            row_b + steps);
    };
    sort_on_threads(ranked.begin(), ranked.end(), threads, before);
    if (ranked.size() > top) ranked.resize(top);

    bool const top_segments = args.rank.top_segments;
    BestPairs const best_pairs = args.rank.best_pairs;
    auto const fill = [&](PickNamer* namer, uint32_t at, BagStatsRow* row) {
        uint32_t const* const picks = &rows[at * width];
        namer->set(picks);
        *row = counts_row(top_counts[picks[steps] >> 2]);
        row->pair = namer->pair();
        row->pairs = namer->pairs();
    };
    if (best_pairs != BestPairs::NONE) {
        PickNamer namer(walk.steps, args.search.pairs, walk.pair_bags.size());
        std::unordered_set<uint32_t> seen;
        std::string line;
        std::vector<uint32_t> best;
        BagStatsRow row;
        for (uint32_t const at : ranked) {
            fill(&namer, at, &row);
            line.clear();
            best.clear();
            if (best_pairs == BestPairs::APPEAR) namer.picks(&best);
            append_best_pairs(&line, &seen, best, walk.pair_bags,
                              walk.prospect_bags);
            fwrite(line.data(), 1, line.size(), stdout);
        }
        return;
    }
    constexpr size_t PRINT_ROWS = 1 << 15;
    size_t const parts = (ranked.size() + PRINT_ROWS - 1) / PRINT_ROWS;
    for (size_t first = 0; first < parts; first += 4 * threads) {
        size_t const some = std::min(4 * threads, parts - first);
        std::vector<std::string> text(some);
        std::atomic<size_t> next{0};
        run_threads(threads, [&](size_t) {
            PickNamer namer(walk.steps, args.search.pairs,
                            walk.pair_bags.size());
            BagStatsRow row;
            for (size_t part; (part = next.fetch_add(1)) < some;) {
                size_t const begin = (first + part) * PRINT_ROWS;
                size_t const end =
                    std::min(begin + PRINT_ROWS, ranked.size());
                for (size_t k = begin; k < end; ++k) {
                    fill(&namer, ranked[k], &row);
                    append_row(&text[part], row, top_segments);
                }
            }
        });
        for (std::string const& part : text)
            fwrite(part.data(), 1, part.size(), stdout);
    }
}

// The --unique output of `inputs` for `args`, walked on the GPU.
int run_pair_usage(BagStatsArgs const& args, BagStatsInputs const& inputs) {
    GpuLeftoverInputs gpu;
    std::vector<size_t> sorted;
    leftover_inputs(inputs, args, &gpu, &sorted);
    RemainderIndex const index(inputs.bag, UINT64_MAX - 1);
    if (index.total() != 0) number_inputs(inputs, index, sorted, &gpu);
    gpu.name = [&](int position) {
        size_t const at = sorted[size_t(position)];
        return position < gpu.pair_bags ? inputs.pair_bags[at].pair
                                        : inputs.prospect_bags[at].pair;
    };
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

// Reports what `counter` looked up in its exact sum table or filter.
void report_sums(BagStatsArgs const& args, GpuRowCounter const& counter) {
    if (args.progress == 0) return;
    GpuFilterCounts const filtered = counter.filter_counts();
    double const share = filtered.leftovers == 0
        ? 0.0
        : 100.0 * double(filtered.scanned) / filtered.leftovers;
    if (counter.filter_info().exact_table)
        report(PROGRAM, "exact sum table: %llu of %llu leftovers have "
               "pairs (%.1f%%)",
               (unsigned long long)filtered.scanned,
               (unsigned long long)filtered.leftovers, share);
    else
        report(PROGRAM, "pair-sum filter: scanned %llu of %llu "
               "leftovers (%.1f%%)",
               (unsigned long long)filtered.scanned,
               (unsigned long long)filtered.leftovers, share);
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
    set_gpu_table_reports(args.progress != 0);
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
    // The -n rows of a ranking that doesn't read the picks are ranked by
    // rest, not walked on the host.
    bool const by_rest = !one_row && args.rank.score.empty() &&
        args.rank.top != 0 && !args.rank.by_pick && steps.size() > 1 &&
        args.cache_size_mb != 0 &&
        RemainderIndex(inputs.bag, UINT64_MAX - 1).total() != 0 &&
        getenv("BSC_ROW_WALK") == nullptr;
    if (!one_row && !by_rest && args.cache_size_mb != 0)
        stage("finding distinct remainders");
    RestFeeder feeder(args, inputs, !one_row && !by_rest,
                      size_t(args.cache_size_mb), threads);
    stage("building GPU tables and pair sums");
    GpuRowCounter const counter(
        PROGRAM, gpu_inputs(inputs, args),
        by_rest ? std::max(size_t(cuda.streams), REST_STREAMS)
                : size_t(cuda.streams));
    if (args.progress != 0) {
        report(PROGRAM, "%s, %d-bit bag keys, %.1f MB of GPU memory",
               counter.device().c_str(), counter.key_bits(),
               double(counter.bytes()) / (1024 * 1024));
        GpuFilterInfo const filter = counter.filter_info();
        std::string const fit = gpu_fit_text(double(filter.bytes),
                                             double(filter.optimal_bytes));
        if (filter.exact_table)
            report(PROGRAM,
                   "exact sum table: %llu group pairs, %llu sums, longest "
                   "sum %d letters, %s, built in %.1fs",
                   (unsigned long long)filter.pairs,
                   (unsigned long long)filter.distinct_sums,
                   filter.longest_sum, fit.c_str(), filter.seconds);
        else
            report(PROGRAM,
                   "pair-sum filter: %llu group pairs, longest sum %d "
                   "letters, %s (%.1f bits per pair), built in %.1fs",
                   (unsigned long long)filter.pairs, filter.longest_sum,
                   fit.c_str(),
                   filter.pairs == 0
                       ? 0.0
                       : double(filter.bytes) * 8 / filter.pairs,
                   filter.seconds);
    }

    if (by_rest) {
        Walk const walk{{PROGRAM, steps, inputs.pair_bags,
                         inputs.prospect_bags, inputs.bag, inputs.word_bags,
                         args, threads, rankers.passes ? 2 : 1, {}, {}},
                        counter, feeder};
        setvbuf(stdout, nullptr, _IOFBF, 1 << 22);
        if (!args.rank.top_segments &&
            args.rank.best_pairs == BestPairs::NONE)
            write_walk_header(walk, letters);
        print_top_by_rest(walk, inputs, &rankers.ranker, rankers.passes,
                          stage);
        report_sums(args, counter);
        return 0;
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
    report_sums(args, counter);
    return status;
}
