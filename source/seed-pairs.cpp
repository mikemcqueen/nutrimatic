// seed-pairs.cpp - Print a seed: the pairs of a pair file (--pairs, or
// idx/pairs.sN with -s N) that pass a length, dictionary and phase 1 YES
// filter, and with -s N the forced pairs of idx/sN.pairs, which skip the
// YES filter.

#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "log.h"
#include "optparse.h"
#include "seed-pairs-impl.h"
#include "workflow-paths.h"

#include <cstdio>
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
    SeedPairsOptions options;
    std::optional<std::filesystem::path> pairs;
    std::optional<int> sentence;
};

void usage(std::string_view program, FILE* out) {
    fprintf(out, "usage: %.*s [-s N] [--pairs PAIRS] [--p1 INDEX] [--pm P] "
            "[-m N]\n       [-x N] [-d FILE] [--threads N]\n",
            SV_ARG(program));
    if (out != stdout) return;

    fprintf(stdout,
            "  print each word,word line of the pair file that passes the\n"
            "  filter, in file order, written in the word order with the\n"
            "  larger phase 1 YES. with -s N, the pairs of %s/sN.pairs,\n"
            "  when it exists, are forced in: they skip --pm, and those the\n"
            "  pair file lacks print last, in its order\n"
            "\noptions:\n", IDX_PATH);
    dfs_help_description_column(30);
    dfs_help_option("-s, --sentence N",
                    "sentence N, from 1 to %d: force in %s/sN.pairs; the "
                    "pair file defaults to %s/pairs.sN",
                    WORKFLOW_SENTENCES, IDX_PATH, IDX_PATH);
    dfs_help_option("--pairs PAIRS",
                    "read PAIRS as the pair file, overriding the default "
                    "of -s");
    dfs_help_option("--p1 INDEX",
                    "look pairs up in INDEX, as make-p1-index writes it "
                    "(default: %s)", P1_INDEX_PATH);
    dfs_help_option("--pm, --prob-min P",
                    "keep pairs whose larger YES is in [P, 1]; pairs "
                    "without a verdict are dropped (default .85)");
    dfs_help_option("-m N", "keep pairs whose words both have at least N "
                    "letters (default %d)", SEED_DEFAULT_MIN_WORD_LEN);
    dfs_help_option("-x N", "keep pairs whose words both have at most N "
                    "letters (default %d)", SEED_DEFAULT_MAX_WORD_LEN);
    dfs_help_option("-d, --dict FILE",
                    "keep pairs whose words are both in FILE, warning "
                    "about each word of a forced pair that isn't");
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
    SeedPairsOptions& opts = out->options;
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
            opts.p1 = options.optarg;
            break;
          case OPT_PROB_MIN:
            if (!parse_double(options.optarg, "--pm", &prob_min))
                return false;
            opts.filter.min_yes = float(prob_min);
            break;
          case 'm':
            if (!parse_count(options.optarg, "-m", &opts.filter.min_len))
                return false;
            break;
          case 'x':
            if (!parse_count(options.optarg, "-x", &opts.filter.max_len))
                return false;
            break;
          case 'd':
            opts.dictionary_path = options.optarg;
            break;
          case OPT_THREADS:
            if (!parse_count(options.optarg, "--threads", &opts.threads))
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
    if (!out->sentence && !out->pairs) {
        report(PROGRAM, "needs -s or --pairs");
        return false;
    }
    if (opts.filter.min_len > opts.filter.max_len) {
        report(PROGRAM, "-m %d is more than -x %d", opts.filter.min_len,
               opts.filter.max_len);
        return false;
    }
    if (out->sentence) {
        std::string const sentence = "s" + std::to_string(*out->sentence);
        if (!valid_seed_sentence(sentence)) {
            report(PROGRAM, "-s %d isn't from 1 to %d", *out->sentence,
                   WORKFLOW_SENTENCES);
            return false;
        }
        if (!out->pairs)
            out->pairs =
                std::filesystem::path(IDX_PATH) / ("pairs." + sentence);
        opts.forced = forced_pairs_path(IDX_PATH, sentence);
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

    DfsDictionary dictionary;
    if (!args.options.dictionary_path.empty()) {
        if (!load_dictionary(args.options.dictionary_path, &dictionary))
            return 1;
        args.options.filter.dictionary = &dictionary;
    }
    SeedPairs seed_pairs;
    if (!seed_pairs.load(PROGRAM, args.options)) return 1;
    std::vector<SeedPair> pairs;
    if (!seed_pairs.run(*args.pairs, &pairs)) return 1;
    for (SeedPair const& pair : pairs) puts(pair.line.c_str());
    return 0;
}
