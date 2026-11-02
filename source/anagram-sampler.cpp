// anagram-sampler.cpp - Draw full anagrams of a bag from pair and solo-word
// units by weight, and print how often each unit appears.

#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "anagram-sampler-impl.h"
#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "letter-bag.h"
#include "log.h"
#include "optparse.h"
#include "row-input.h"

namespace {

constexpr char PROGRAM[] = "anagram-sampler";
constexpr size_t DIFFERENCE_UNITS = 1000;

enum {
    OPT_PAIRS = 256,
    OPT_SOLO,
    OPT_BETA,
    OPT_PAIR_PENALTY,
    OPT_SOLO_SCALE,
    OPT_SOLO_PENALTY,
    OPT_THREADS,
    OPT_SEED,
    OPT_BURN,
    OPT_MOVES,
    OPT_WATCH,
    OPT_SECONDS,
    OPT_PROGRESS,
};

struct Args {
    std::filesystem::path pairs;
    std::filesystem::path solo;
    std::optional<std::filesystem::path> watch;
    SamplerWeights weights;
    int threads = 0;
    int64_t seed = 0;
    int64_t burn = 100000;
    int64_t moves = 1000000;
    double seconds = 0;
    double progress = 10;
    std::string letters;
};

void usage(std::string_view program, FILE* out) {
    fprintf(out,
            "usage: %.*s --pairs PRIOR --solo SOLO [--beta B]\n"
            "       [--pair-penalty P] [--solo-scale A] [--solo-penalty L]\n"
            "       [--threads T] [--seed N] [--burn M] [--moves M]\n"
            "       [--seconds S] [--progress S] [--watch FILE] LETTERS\n",
            SV_ARG(program));
    if (out != stdout) return;
    fputs("  draw full anagrams of LETTERS made of pair units (PRIOR's\n"
          "  \"SCORE word,word\" lines) and solo-word units (SOLO's \"COUNT\n"
          "  word\" lines), each with probability proportional to the product\n"
          "  of its units' weights, and print \"SHARE unit\" for every unit\n"
          "  drawn, SHARE the share of draws holding it, highest first\n"
          "\noptions:\n",
          stdout);
    dfs_help_option("--pairs PRIOR", "pair units, as pair-prior prints them");
    dfs_help_option("--solo SOLO",
                    "solo-word units, as query-index LETTERS -w1 -n0 prints "
                    "them");
    dfs_help_option("--beta B", "scale every log weight (default 1)");
    dfs_help_option("--pair-penalty P",
                    "a pair unit's log weight is B * (SCORE - P) "
                    "(default 4.5)");
    dfs_help_option("--solo-scale A", "see --solo-penalty (default 0.5)");
    dfs_help_option("--solo-penalty L",
                    "a solo unit's log weight is B * (A * ln(COUNT) - L) "
                    "(default 4)");
    dfs_help_option("--threads T",
                    "run T independent chains (default: hardware threads)");
    dfs_help_option("--seed N", "seed chain i's random numbers from N and i "
                    "(default 0)");
    dfs_help_option("--burn M", "discard M moves per chain (default 100000)");
    dfs_help_option("--moves M",
                    "then record M moves per chain (default 1000000)");
    dfs_help_option("--seconds S",
                    "stop each chain's recording after S seconds, even "
                    "short of --moves");
    dfs_help_option("--progress S",
                    "report progress every S seconds, 0 for never "
                    "(default 10)");
    dfs_help_option("--watch FILE",
                    "also report \"watch SHARE line\" on stderr at the end "
                    "for each of FILE's lines of words, comma- or "
                    "space-separated, SHARE the share of draws holding all "
                    "of its words, in any units");
    dfs_help_option("-h, --help", "show this help");
}

bool parse_args(char* argv[], Args* out, bool* help) {
    static struct optparse_long const long_options[] = {
        {"pairs", OPT_PAIRS, OPTPARSE_REQUIRED},
        {"solo", OPT_SOLO, OPTPARSE_REQUIRED},
        {"beta", OPT_BETA, OPTPARSE_REQUIRED},
        {"pair-penalty", OPT_PAIR_PENALTY, OPTPARSE_REQUIRED},
        {"solo-scale", OPT_SOLO_SCALE, OPTPARSE_REQUIRED},
        {"solo-penalty", OPT_SOLO_PENALTY, OPTPARSE_REQUIRED},
        {"threads", OPT_THREADS, OPTPARSE_REQUIRED},
        {"seed", OPT_SEED, OPTPARSE_REQUIRED},
        {"burn", OPT_BURN, OPTPARSE_REQUIRED},
        {"moves", OPT_MOVES, OPTPARSE_REQUIRED},
        {"watch", OPT_WATCH, OPTPARSE_REQUIRED},
        {"seconds", OPT_SECONDS, OPTPARSE_REQUIRED},
        {"progress", OPT_PROGRESS, OPTPARSE_REQUIRED},
        {"help", 'h', OPTPARSE_NONE},
        {NULL, 0, OPTPARSE_NONE},
    };
    struct optparse options;
    optparse_init(&options, argv);
    int option;
    while ((option = optparse_long(&options, long_options, NULL)) != -1) {
        char const* const arg = options.optarg;
        bool ok = true;
        switch (option) {
          case OPT_PAIRS: out->pairs = arg; break;
          case OPT_SOLO: out->solo = arg; break;
          case OPT_WATCH: out->watch = arg; break;
          case OPT_BETA:
            ok = parse_double(arg, "--beta", &out->weights.beta);
            break;
          case OPT_PAIR_PENALTY:
            ok = parse_double(arg, "--pair-penalty",
                              &out->weights.pair_penalty);
            break;
          case OPT_SOLO_SCALE:
            ok = parse_double(arg, "--solo-scale", &out->weights.solo_scale);
            break;
          case OPT_SOLO_PENALTY:
            ok = parse_double(arg, "--solo-penalty",
                              &out->weights.solo_penalty);
            break;
          case OPT_THREADS:
            ok = parse_count(arg, "--threads", &out->threads);
            break;
          case OPT_SEED: ok = parse_count64(arg, "--seed", &out->seed); break;
          case OPT_BURN: ok = parse_count64(arg, "--burn", &out->burn); break;
          case OPT_MOVES:
            ok = parse_count64(arg, "--moves", &out->moves);
            break;
          case OPT_SECONDS:
            ok = parse_double(arg, "--seconds", &out->seconds);
            break;
          case OPT_PROGRESS:
            ok = parse_double(arg, "--progress", &out->progress);
            break;
          case 'h': *help = true; return true;
          default:
            fprintf(stderr, "error: %s\n", options.errmsg);
            return false;
        }
        if (!ok) return false;
    }
    char const* const letters = optparse_arg(&options);
    if (letters == NULL || optparse_arg(&options) != NULL) return false;
    if (out->pairs.empty() || out->solo.empty()) {
        report(PROGRAM, "needs --pairs and --solo");
        return false;
    }
    out->letters = az_letters(letters);
    if (out->letters.empty()) {
        report(PROGRAM, "LETTERS has no a-z letters");
        return false;
    }
    if (out->threads == 0)
        out->threads = int(std::max(1u, std::thread::hardware_concurrency()));
    return true;
}

// Splits "NUMBER rest" lines; false, diagnosed, when the line isn't one.
bool split_number(std::filesystem::path const& path, size_t number,
                  std::string_view line, double* value,
                  std::string_view* rest) {
    size_t const space = line.find(' ');
    char* end = NULL;
    std::string const head(line.substr(0, space));
    if (space != std::string_view::npos) *value = strtod(head.c_str(), &end);
    if (space == std::string_view::npos || head.empty() ||
        end != head.c_str() + head.size() ||
        !std::isfinite(*value)) {
        report(PROGRAM, "%s:%zu: expected \"NUMBER text\"", path.c_str(),
               number);
        return false;
    }
    *rest = line.substr(space + 1);
    return true;
}

bool read_units(Args const& args, SamplerModel* model) {
    auto const pair_line = [&](std::string_view line, size_t number) {
        if (line.empty()) return true;
        double score;
        std::string_view pair;
        if (!split_number(args.pairs, number, line, &score, &pair))
            return false;
        size_t const comma = pair.find(',');
        if (comma == std::string_view::npos ||
            pair.find(',', comma + 1) != std::string_view::npos) {
            report(PROGRAM, "%s:%zu: expected \"SCORE word,word\"",
                   args.pairs.c_str(), number);
            return false;
        }
        model->add_pair(pair.substr(0, comma), pair.substr(comma + 1), score);
        return true;
    };
    auto const solo_line = [&](std::string_view line, size_t number) {
        if (line.empty()) return true;
        double count;
        std::string_view word;
        if (!split_number(args.solo, number, line, &count, &word))
            return false;
        if (count <= 0) {
            report(PROGRAM, "%s:%zu: COUNT must be positive",
                   args.solo.c_str(), number);
            return false;
        }
        model->add_solo(word, count);
        return true;
    };
    return read_lines(PROGRAM, args.pairs, pair_line) &&
           read_lines(PROGRAM, args.solo, solo_line);
}

struct Watch {
    std::string line;
    std::vector<uint32_t> words;
};

bool read_watch(std::filesystem::path const& path, SamplerModel const& model,
                std::vector<Watch>* out) {
    return read_lines(PROGRAM, path, [&](std::string_view line, size_t) {
        Watch watch{std::string(line), {}};
        size_t start = 0;
        while (start <= line.size()) {
            size_t end = line.find_first_of(", ", start);
            if (end == std::string_view::npos) end = line.size();
            if (end > start)
                watch.words.push_back(
                    model.find_word(line.substr(start, end - start)));
            start = end + 1;
        }
        if (!watch.words.empty()) out->push_back(std::move(watch));
        return true;
    });
}

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// What one chain has counted, read by the progress report while it runs.
struct ChainCounts {
    explicit ChainCounts(size_t units, size_t watches)
        : units(units), watches(watches) {}

    void add(std::atomic<uint64_t>& counter) {
        counter.store(counter.load(std::memory_order_relaxed) + 1,
                      std::memory_order_relaxed);
    }

    std::vector<std::atomic<uint64_t>> units;
    std::vector<std::atomic<uint64_t>> watches;
    std::atomic<uint64_t> moves{0};
    std::atomic<uint64_t> draws{0};
    std::atomic<uint64_t> accepted{0};
    uint64_t pair_units = 0;
    uint64_t solo_units = 0;
    uint64_t proposed_by[2][2] = {};
    uint64_t accepted_by[2][2] = {};
};

// The largest difference in SHARE between two chains over the units with
// the highest SHARE.
double chain_difference(std::deque<ChainCounts> const& chains) {
    size_t const n = chains[0].units.size();
    std::vector<uint64_t> total(n, 0);
    std::vector<double> draws;
    for (ChainCounts const& chain : chains)
        draws.push_back(double(chain.draws.load(std::memory_order_relaxed)));
    for (ChainCounts const& chain : chains)
        for (size_t u = 0; u < n; ++u)
            total[u] += chain.units[u].load(std::memory_order_relaxed);
    std::vector<uint32_t> top(n);
    for (size_t u = 0; u < n; ++u) top[u] = uint32_t(u);
    size_t const keep = std::min(n, DIFFERENCE_UNITS);
    std::partial_sort(top.begin(), top.begin() + keep, top.end(),
                      [&](uint32_t x, uint32_t y) {
                          return total[x] > total[y];
                      });
    double most = 0;
    for (size_t i = 0; i < keep; ++i) {
        double low = 1, high = 0;
        for (size_t c = 0; c < chains.size(); ++c) {
            if (draws[c] == 0) continue;
            double const share =
                double(chains[c].units[top[i]].load(
                    std::memory_order_relaxed)) / draws[c];
            low = std::min(low, share);
            high = std::max(high, share);
        }
        most = std::max(most, high - low);
    }
    return most;
}

void run_chain(Args const& args, SamplerModel const& model,
               std::vector<Watch> const& watches,
               std::vector<uint32_t> start, SamplerRng* rng,
               ChainCounts* counts) {
    SamplerChain chain(model, std::move(start), rng);
    auto const moved = [&](uint64_t moves) {
        counts->moves.store(moves, std::memory_order_relaxed);
        if ((moves & 255) != 0) return;
        uint64_t accepted = 0;
        for (auto const& row : chain.accepted)
            for (uint64_t const a : row) accepted += a;
        counts->accepted.store(accepted, std::memory_order_relaxed);
    };
    for (int64_t i = 0; i < args.burn; ++i) {
        chain.move();
        moved(uint64_t(i + 1));
    }
    Clock::time_point const recording = Clock::now();
    for (int64_t i = 0; i < args.moves; ++i) {
        if (args.seconds > 0 && (i & 255) == 0 &&
            seconds_since(recording) >= args.seconds)
            break;
        chain.move();
        for (uint32_t const u : chain.units()) counts->add(counts->units[u]);
        for (size_t w = 0; w < watches.size(); ++w) {
            bool all = true;
            for (uint32_t const word : watches[w].words)
                all = all && chain.has_word(word);
            if (all) counts->add(counts->watches[w]);
        }
        counts->pair_units += uint64_t(chain.pair_units());
        counts->solo_units += uint64_t(chain.solo_units());
        counts->add(counts->draws);
        moved(uint64_t(args.burn + i + 1));
    }
    for (int r = 0; r < 2; ++r)
        for (int s = 0; s < 2; ++s) {
            counts->proposed_by[r][s] = chain.proposed[r][s];
            counts->accepted_by[r][s] = chain.accepted[r][s];
        }
}

double share(uint64_t count, uint64_t draws) {
    return draws == 0 ? 0.0 : double(count) / double(draws);
}

}  // namespace

int main(int argc, char* argv[]) {
    (void)argc;
    Args args;
    bool help = false;
    if (!parse_args(argv, &args, &help)) {
        usage(argv[0], stderr);
        return 2;
    }
    if (help) {
        usage(argv[0], stdout);
        return 0;
    }

    Clock::time_point phase = Clock::now();
    SamplerModel model;
    std::string error;
    if (!model.load(args.letters, args.weights, &error)) {
        report(PROGRAM, "%s", error.c_str());
        return 1;
    }
    if (!read_units(args, &model)) return 1;
    report(PROGRAM, "read %zu units, %zu words, in %.2fs",
           model.units().size(), model.word_count(), seconds_since(phase));
    phase = Clock::now();
    if (!model.finish(&error)) {
        report(PROGRAM, "%s", error.c_str());
        return 1;
    }
    report(PROGRAM, "grouped into %zu groups in %.2fs", model.group_count(),
           seconds_since(phase));
    std::vector<Watch> watches;
    if (args.watch && !read_watch(*args.watch, model, &watches)) return 1;

    size_t const threads = size_t(args.threads);
    std::vector<SamplerRng> rngs;
    for (size_t c = 0; c < threads; ++c)
        rngs.emplace_back(uint64_t(args.seed), uint64_t(c));
    std::vector<std::vector<uint32_t>> starts(threads);
    phase = Clock::now();
    for (size_t c = 0; c < threads; ++c) {
        if (!sampler_start(model, &rngs[c], &starts[c])) {
            report(PROGRAM, "the letters have no full anagram");
            return 1;
        }
    }
    report(PROGRAM, "found %zu starts in %.2fs", threads,
           seconds_since(phase));

    std::deque<ChainCounts> chains;
    for (size_t c = 0; c < threads; ++c)
        chains.emplace_back(model.units().size(), watches.size());
    std::atomic<size_t> running{threads};
    Clock::time_point const started = Clock::now();
    std::vector<std::thread> workers;
    for (size_t c = 0; c < threads; ++c)
        workers.emplace_back([&, c] {
            run_chain(args, model, watches, std::move(starts[c]), &rngs[c],
                      &chains[c]);
            running.fetch_sub(1);
        });
    if (args.progress > 0) {
        Clock::time_point next = started;
        while (running.load() != 0) {
            next += std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(args.progress));
            while (running.load() != 0 && Clock::now() < next)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (running.load() == 0) break;
            uint64_t moves = 0, accepted = 0, draws = 0;
            for (ChainCounts const& chain : chains) {
                moves += chain.moves.load(std::memory_order_relaxed);
                accepted += chain.accepted.load(std::memory_order_relaxed);
                draws += chain.draws.load(std::memory_order_relaxed);
            }
            double const elapsed = seconds_since(started);
            report(PROGRAM,
                   "%llu moves, %.0f/s, %llu draws, accept %.3f, chain "
                   "difference %.4f",
                   (unsigned long long)moves, double(moves) / elapsed,
                   (unsigned long long)draws,
                   moves == 0 ? 0.0 : double(accepted) / double(moves),
                   draws == 0 ? 0.0 : chain_difference(chains));
        }
    }
    for (std::thread& worker : workers) worker.join();
    double const elapsed = seconds_since(started);

    size_t const n = model.units().size();
    std::vector<uint64_t> total(n, 0);
    uint64_t draws = 0, moves = 0, pair_units = 0, solo_units = 0;
    uint64_t proposed[2][2] = {}, accepted[2][2] = {};
    std::vector<uint64_t> watched(watches.size(), 0);
    for (ChainCounts const& chain : chains) {
        for (size_t u = 0; u < n; ++u) total[u] += chain.units[u].load();
        for (size_t w = 0; w < watches.size(); ++w)
            watched[w] += chain.watches[w].load();
        draws += chain.draws.load();
        moves += chain.moves.load();
        pair_units += chain.pair_units;
        solo_units += chain.solo_units;
        for (int r = 0; r < 2; ++r)
            for (int s = 0; s < 2; ++s) {
                proposed[r][s] += chain.proposed_by[r][s];
                accepted[r][s] += chain.accepted_by[r][s];
            }
    }

    std::vector<uint32_t> order;
    for (size_t u = 0; u < n; ++u)
        if (total[u] != 0) order.push_back(uint32_t(u));
    std::stable_sort(order.begin(), order.end(), [&](uint32_t x, uint32_t y) {
        return total[x] > total[y];
    });
    for (uint32_t const u : order)
        printf("%.6f %s\n", share(total[u], draws),
               model.units()[u].text.c_str());

    report(PROGRAM, "%llu draws, %llu moves in %.2fs, %.0f moves/s",
           (unsigned long long)draws, (unsigned long long)moves, elapsed,
           double(moves) / elapsed);
    for (int r = 0; r < 2; ++r)
        for (int s = 0; s < 2; ++s)
            report(PROGRAM, "r %d -> s %d: %llu proposed, accept %.4f", r + 1,
                   s + 1, (unsigned long long)proposed[r][s],
                   proposed[r][s] == 0
                       ? 0.0
                       : double(accepted[r][s]) / double(proposed[r][s]));
    report(PROGRAM,
           "per draw: %.3f pair units, %.3f solo units; chain difference "
           "%.4f over the top %zu units",
           draws == 0 ? 0.0 : double(pair_units) / double(draws),
           draws == 0 ? 0.0 : double(solo_units) / double(draws),
           chain_difference(chains), std::min(n, DIFFERENCE_UNITS));
    for (size_t w = 0; w < watches.size(); ++w)
        report(PROGRAM, "watch %.6f %s", share(watched[w], draws),
               watches[w].line.c_str());
    return 0;
}
