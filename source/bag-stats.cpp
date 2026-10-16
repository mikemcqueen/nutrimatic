// bag-stats.cpp - Rank combinations of pair letter bags by the exact word +
// pair completions they leave.

#include "bag-stats-cpu.h"
#include "bag-stats-run.h"
#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "optparse.h"

#include <cstdio>
#include <cstring>
#include <strings.h>
#include <string>

namespace {

constexpr char PROGRAM[] = "bag-stats";
constexpr int OPT_CACHE_SIZE = OPT_COMMON_END;
constexpr int OPT_PAIR_CACHE_SIZE = OPT_COMMON_END + 1;
constexpr int OPT_COMPLETIONS = OPT_COMMON_END + 2;
constexpr int OPT_EXACT = OPT_COMMON_END + 3;
constexpr int OPT_ALL = OPT_COMMON_END + 4;
constexpr int OPT_SORT = OPT_COMMON_END + 5;

void usage(char const* program, FILE* out) {
    fprintf(out,
            "usage: %s [-u LETTERS] [-s N | --no-nopairs] [--min-wb-len N]\n"
            "       [--ss STEPS] [--pp FILE] [--threads N] [--cache-size N]\n"
            "       [--pair-cache-size N]\n"
            "       [--progress N]\n"
            "       [-n N SORT... [--by-length | --by-pick]]\n"
            "       [--score W1,W2... [--score-detail] [--completions N]\n"
            "        [SORT... [--by-length | --by-pick]]]\n"
            "       [--best-pairs appear|lead]\n"
            "       [--prior FILE | [--p1 INDEX] [--clues CLUEDATA]\n"
            "        [--prior-weights SPEC]] [--top-segments]\n"
            "       LETTERS PAIRS DICT\n"
            "       %s --ss pf|wb,pf|pb,wb,pf [-u LETTERS]\n"
            "       [-s N | --no-nopairs] [--min-wb-len N] [--threads N]\n"
            "       [--exact]\n"
            "       [--completions N | --all] [--sort TP|UP]\n"
            "       LETTERS PAIRS DICT\n",
            program, program);
    if (out != stdout) return;

    help_common_options(true);
    dfs_help_option("--cache-size N",
                    "hold at most N MB of cached remainder counts, 0 for "
                    "none (default: %d)", BAG_STATS_DEFAULT_CACHE_SIZE_MB);
    dfs_help_option("--pair-cache-size N",
                    "hold at most N MB of cached pair counts for leftovers "
                    "too long for the pair count table, 0 for none "
                    "(default: %d)", BAG_STATS_DEFAULT_PAIR_CACHE_SIZE_MB);
    dfs_help_option("--completions N",
                    "with --score, below the rank print the row's N "
                    "completions with the most exact pairs, ties in DICT "
                    "order: each one's PC, SOLO when it leaves nothing, and "
                    "its word bags, each as its DICT spellings; --score "
                    "without SORT options prints the row without its rank, "
                    "counted from what its picks leave alone; with --ss pf "
                    "or wb,pf, below the row print the N DICT word bags with "
                    "the most pairs, counted as the row counts them, or "
                    "SOLO; with pb,wb,pf, instead of the row print the N "
                    "PAIRS bags with the most pairs, or SOLO");
    dfs_help_option("--exact",
                    "with --ss pf, count only the PAIRS rows that use all "
                    "of what's left: of what each DICT word leaves, with "
                    "wb,pf after a second DICT word, with pb,wb,pf of what "
                    "each PAIRS bag leaves after two DICT words");
    dfs_help_option("--all",
                    "with --ss pf, instead of the row print every bag "
                    "--completions would, with pairs or SOLO, as it prints "
                    "them");
    dfs_help_option("--sort KEY",
                    "with --ss pf, what --completions and --all sort by and "
                    "show, in either case: TP, total pairs, counted once for each way of "
                    "reaching them, or UP, unique pairs, counted once; they "
                    "differ only with pb,wb,pf (default: TP)");
    help_common_tail();
}

bool parse_args(char* argv[], BagStatsOptions* out, bool* help) {
    static struct optparse_long const long_options[] = {
        BAG_STATS_COMMON_LONG_OPTIONS,
        { "cache-size", OPT_CACHE_SIZE, OPTPARSE_REQUIRED },
        { "pair-cache-size", OPT_PAIR_CACHE_SIZE, OPTPARSE_REQUIRED },
        { "completions", OPT_COMPLETIONS, OPTPARSE_REQUIRED },
        { "exact", OPT_EXACT, OPTPARSE_NONE },
        { "all", OPT_ALL, OPTPARSE_NONE },
        { "sort", OPT_SORT, OPTPARSE_REQUIRED },
        { "help", 'h', OPTPARSE_NONE },
        { NULL, 0, OPTPARSE_NONE },
    };

    struct optparse options;
    optparse_init(&options, argv);

    bool unused_by_pf = false;
    bool sorted = false;
    int option;
    while ((option = optparse_long(&options, long_options, NULL)) != -1) {
        unused_by_pf |= option == OPT_COMMON_PROGRESS ||
            option == OPT_CACHE_SIZE || option == OPT_PAIR_CACHE_SIZE;
        RankOption const common = parse_common_option(
            PROGRAM, option, options.optarg, &out->common);
        if (common == RankOption::BAD) return false;
        if (common == RankOption::PARSED) continue;
        switch (option) {
          case OPT_CACHE_SIZE:
            if (!parse_count(options.optarg, "--cache-size",
                             &out->cache_size_mb))
                return false;
            break;
          case OPT_PAIR_CACHE_SIZE:
            if (!parse_count(options.optarg, "--pair-cache-size",
                             &out->pair_cache_size_mb))
                return false;
            break;
          case OPT_COMPLETIONS:
            if (!parse_count(options.optarg, "--completions",
                             &out->completions))
                return false;
            break;
          case OPT_EXACT:
            out->exact = true;
            break;
          case OPT_ALL:
            out->all = true;
            break;
          case OPT_SORT:
            if (strcasecmp(options.optarg, "TP") == 0) {
                out->sort = PairFilterSort::TP;
            } else if (strcasecmp(options.optarg, "UP") == 0) {
                out->sort = PairFilterSort::UP;
            } else {
                fprintf(stderr, "%s: --sort is TP or UP, not %s\n", PROGRAM,
                        options.optarg);
                return false;
            }
            sorted = true;
            break;
          case 'h':
            *help = true;
            return true;
          default:
            fprintf(stderr, "error: %s\n", options.errmsg);
            return false;
        }
    }

    if (!check_common_args(PROGRAM, &options, &out->common)) return false;
    if (unused_by_pf && out->common.search.pair_filter) {
        fprintf(stderr, "%s: --ss pf goes without --progress, --cache-size "
                "and --pair-cache-size\n", PROGRAM);
        return false;
    }
    if (out->common.search.pair_filter && out->common.search.words > 0 &&
        !out->exact) {
        fprintf(stderr, "%s: --ss %s needs --exact\n", PROGRAM,
                search_text(out->common.search).c_str());
        return false;
    }
    if (out->common.search.pair_filter && out->common.search.pairs > 0 &&
        !out->all && out->completions == 0) {
        fprintf(stderr, "%s: --ss pb,wb,pf needs --all or --completions\n",
                PROGRAM);
        return false;
    }
    if (out->exact && !out->common.search.pair_filter) {
        fprintf(stderr, "%s: --exact goes with --ss pf\n", PROGRAM);
        return false;
    }
    if (sorted && !out->common.search.pair_filter) {
        fprintf(stderr, "%s: --sort goes with --ss pf\n", PROGRAM);
        return false;
    }
    if (out->all && !out->common.search.pair_filter) {
        fprintf(stderr, "%s: --all goes with --ss pf\n", PROGRAM);
        return false;
    }
    if (out->all && out->completions > 0) {
        fprintf(stderr, "%s: --all doesn't go with --completions\n",
                PROGRAM);
        return false;
    }
    if (out->completions > 0 && out->common.rank.score.empty() &&
        !out->common.search.pair_filter) {
        fprintf(stderr, "%s: --completions goes with --score or --ss pf\n",
                PROGRAM);
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    BagStatsOptions options;
    bool help = false;
    std::string letters;
    if (!parse_args(argv, &options, &help) ||
        (!help && !remaining_letters(options.common, &letters))) {
        usage(argv[0], stderr);
        return 2;
    }
    if (help) {
        usage(argv[0], stdout);
        return 0;
    }
    return bag_stats(options, letters);
}
