// seed-pairs.cpp - Print a seed: the pairs of a pair file that pass a
// length, dictionary and phase 1 YES filter.

#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "log.h"
#include "optparse.h"
#include "p1-index.h"
#include "seed-view.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr char PROGRAM[] = "seed-pairs";
constexpr int OPT_P1 = 256;
constexpr int OPT_PROB_MIN = 257;
constexpr int OPT_THREADS = 258;
constexpr int OPT_PAIRS = 259;

struct Args {
    std::optional<std::filesystem::path> p1;
    std::optional<std::filesystem::path> dictionary;
    SeedFilter filter;
    int threads = 0;
    std::optional<std::filesystem::path> pairs;
    std::optional<int> sentence;
};

void usage(std::string_view program, FILE* out) {
    fprintf(out, "usage: %.*s (-s N | --pairs PAIRS) [--p1 INDEX] [--pm P] "
            "[-m N]\n       [-x N] [-d FILE] [--threads N]\n",
            SV_ARG(program));
    if (out != stdout) return;

    fputs("  print each word,word line of PAIRS that passes the filter, in\n"
          "  file order, written in the word order with the larger phase 1\n"
          "  YES\n"
          "\noptions:\n", stdout);
    dfs_help_option("-s, --sentence N",
                    "read $IDX_DIR/pairs.sN");
    dfs_help_option("--pairs PAIRS", "read PAIRS");
    dfs_help_option("--p1 INDEX",
                    "look pairs up in INDEX, as make-p1-index writes it "
                    "(default: $IDX_DIR/p1.idx)");
    dfs_help_option("--pm, --prob-min P",
                    "keep pairs whose larger YES is in [P, 1]; pairs "
                    "without a verdict are dropped (default .85)");
    dfs_help_option("-m N", "keep pairs whose words both have at least N "
                    "letters (default 3)");
    dfs_help_option("-x N", "keep pairs whose words both have at most N "
                    "letters (default 13)");
    dfs_help_option("-d, --dict FILE",
                    "keep pairs whose words are both in FILE");
    dfs_help_option("--threads N",
                    "read PAIRS on N threads, 0 for all of them (default 0)");
    dfs_help_option("-h, --help", "show this help");
}

bool parse_args(char* argv[], Args* out, bool* help) {
    static struct optparse_long const long_options[] = {
        { "sentence", 's', OPTPARSE_REQUIRED },
        { "pairs", OPT_PAIRS, OPTPARSE_REQUIRED },
        { "p1", OPT_P1, OPTPARSE_REQUIRED },
        { "pm", OPT_PROB_MIN, OPTPARSE_REQUIRED },
        { "prob-min", OPT_PROB_MIN, OPTPARSE_REQUIRED },
        { "min", 'm', OPTPARSE_REQUIRED },
        { "max", 'x', OPTPARSE_REQUIRED },
        { "dict", 'd', OPTPARSE_REQUIRED },
        { "threads", OPT_THREADS, OPTPARSE_REQUIRED },
        { "help", 'h', OPTPARSE_NONE },
        { NULL, 0, OPTPARSE_NONE },
    };
    struct optparse options;
    optparse_init(&options, argv);
    int option;
    double prob_min;
    int sentence;
    out->filter.min_yes = .85f;
    while ((option = optparse_long(&options, long_options, NULL)) != -1) {
        switch (option) {
          case 's':
            if (!parse_count(options.optarg, "-s", &sentence)) return false;
            out->sentence = sentence;
            break;
          case OPT_PAIRS:
            out->pairs = options.optarg;
            break;
          case OPT_P1:
            out->p1 = options.optarg;
            break;
          case OPT_PROB_MIN:
            if (!parse_double(options.optarg, "--pm", &prob_min))
                return false;
            out->filter.min_yes = float(prob_min);
            break;
          case 'm':
            if (!parse_count(options.optarg, "-m", &out->filter.min_len))
                return false;
            break;
          case 'x':
            if (!parse_count(options.optarg, "-x", &out->filter.max_len))
                return false;
            break;
          case 'd':
            out->dictionary = options.optarg;
            break;
          case OPT_THREADS:
            if (!parse_count(options.optarg, "--threads", &out->threads))
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
    if (optparse_arg(&options) != NULL) return false;
    if (out->sentence.has_value() == out->pairs.has_value()) {
        report(PROGRAM, "needs one of -s or --pairs");
        return false;
    }
    if (out->filter.min_len > out->filter.max_len) {
        report(PROGRAM, "-m %d is more than -x %d", out->filter.min_len,
               out->filter.max_len);
        return false;
    }
    if (!out->p1 || out->sentence) {
        char const* const idx_dir = getenv("IDX_DIR");
        if (idx_dir == NULL || idx_dir[0] == '\0') {
            report(PROGRAM, "needs a nonempty IDX_DIR without --p1 or with -s");
            return false;
        }
        std::filesystem::path const dir(idx_dir);
        if (!out->p1) out->p1 = dir / "p1.idx";
        if (out->sentence)
            out->pairs = dir / ("pairs.s" + std::to_string(*out->sentence));
    }
    return true;
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

    std::string error;
    std::string stale;
    P1Index p1;
    if (!p1.open(*args.p1, &error, &stale)) {
        report(PROGRAM, "%s", error.c_str());
        return 1;
    }
    if (!stale.empty()) alert(PROGRAM, "%s", stale.c_str());
    DfsDictionary dictionary;
    if (args.dictionary) {
        if (!load_dictionary(*args.dictionary, &dictionary)) return 1;
        args.filter.dictionary = &dictionary;
    }
    std::vector<SeedPair> pairs;
    if (!read_seed_pairs(*args.pairs, &p1,
                         resolve_search_threads(args.threads), &pairs,
                         &error)) {
        report(PROGRAM, "%s", error.c_str());
        return 1;
    }
    for (SeedPair const& pair : pairs)
        if (seed_pair_passes(pair, args.filter)) puts(pair.line.c_str());
    return 0;
}
