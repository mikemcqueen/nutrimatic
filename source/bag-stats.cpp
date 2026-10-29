// bag-stats.cpp - Rank combinations of pair letter bags by the exact word +
// pair completions they leave.

#include "bag-stats-cpu.h"
#include "bag-stats-run.h"
#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "log.h"
#include "optparse.h"

#include <cstdio>
#include <string>
#include <string_view>

namespace {

constexpr char PROGRAM[] = "bag-stats";
constexpr int OPT_PAIR_CACHE_SIZE = OPT_COMMON_END;
constexpr int OPT_COMPLETIONS = OPT_COMMON_END + 1;
constexpr int OPT_EXACT = OPT_COMMON_END + 2;
constexpr int OPT_ALL = OPT_COMMON_END + 3;
constexpr int OPT_LEFTOVERS = OPT_COMMON_END + 4;
constexpr int OPT_HLL = OPT_COMMON_END + 5;

void usage(std::string_view program, FILE* out) {
    fprintf(out,
            "usage: %.*s [-u LETTERS] [-s N | --no-nopairs] [--min-wb-len N]\n"
            "       [--min-pb-len N]\n"
            "       [--ss STEPS] [--pp FILE] [--threads N] [--cache-size N]\n"
            "       [--pair-cache-size N]\n"
            "       [--progress N]\n"
            "       [-n N SORT... [--by-length | --by-pick]]\n"
            "       [--score W1,W2... [--score-detail]\n"
            "        [--completions N | --all]\n"
            "        [SORT... [--by-length | --by-pick]]]\n"
            "       [--best-pairs appear|lead]\n"
            "       [--prior FILE | --no-prior |\n"
            "        [--p1 INDEX] [--clues CLUEDATA] [--prior-weights SPEC]]\n"
            "       [--top-segments] [--exact]\n"
            "       LETTERS PAIRS DICT\n"
            "       %.*s --ss [wb,...]p|pf [-u LETTERS]\n"
            "       [-s N | --no-nopairs] [--min-wb-len N] [--threads N]\n"
            "       [--exact] [--completions N | --all]\n"
            "       LETTERS PAIRS DICT\n"
            "       %.*s --leftovers [--hll] --ss pb[,pb...] [-u LETTERS]\n"
            "       [-s N | --no-nopairs] [--min-pb-len N] [--threads N]\n"
            "       [--progress N] LETTERS PAIRS DICT\n",
            SV_ARG(program),
            SV_ARG(program),
            SV_ARG(program));
    if (out != stdout) return;

    help_common_options(true);
    dfs_help_option("--pair-cache-size N",
                    "hold at most N MB of cached pair counts for leftovers "
                    "too long for the pair count table, 0 for none "
                    "(default: %d)", BAG_STATS_DEFAULT_PAIR_CACHE_SIZE_MB);
    dfs_help_option("--completions N",
                    "with --score, or without ppb or pb steps, below the row "
                    "print its N completions with the most pairs, ties in "
                    "DICT order, without pick steps only those with pairs, "
                    "or SOLO: each one's PC, SOLO when it leaves nothing, "
                    "and its word bags, each as its DICT spellings, or with "
                    "a last pf and no wb steps each PAIRS bag pf counts, as "
                    "its rows; --score without SORT options prints the row "
                    "without its rank, counted from what its picks leave "
                    "alone");
    dfs_help_option("--all",
                    "--completions of every completion with pairs, or SOLO");
    dfs_help_option("--leftovers",
                    "with --ss of pb steps alone, count the combinations "
                    "that fit, by how many picks they make, and those of "
                    "every pb step, their PAIRS rows and their distinct "
                    "leftovers by how many letters they leave");
    dfs_help_option("--hll, --hyper-log-log",
                    "with --leftovers, estimate the distinct leftovers by "
                    "HyperLogLog, within about 1%%, in little memory, as "
                    "LEFTOVERS_HLL");
    dfs_help_option("--exact",
                    "with a last pf, count only the PAIRS rows that use all "
                    "of what's left, as wb and pb steps before it need; a "
                    "last p counts only those already");
    help_common_tail();
}

bool parse_args(char* argv[], BagStatsOptions* out, bool* help) {
    static struct optparse_long const long_options[] = {
        BAG_STATS_COMMON_LONG_OPTIONS,
        { "pair-cache-size", OPT_PAIR_CACHE_SIZE, OPTPARSE_REQUIRED },
        { "completions", OPT_COMPLETIONS, OPTPARSE_REQUIRED },
        { "exact", OPT_EXACT, OPTPARSE_NONE },
        { "all", OPT_ALL, OPTPARSE_NONE },
        { "leftovers", OPT_LEFTOVERS, OPTPARSE_NONE },
        { "hll", OPT_HLL, OPTPARSE_NONE },
        { "hyper-log-log", OPT_HLL, OPTPARSE_NONE },
        { "help", 'h', OPTPARSE_NONE },
        { NULL, 0, OPTPARSE_NONE },
    };

    struct optparse options;
    optparse_init(&options, argv);

    int option;
    while ((option = optparse_long(&options, long_options, NULL)) != -1) {
        RankOption const common = parse_common_option(
            PROGRAM, option, options.optarg, &out->common);
        if (common == RankOption::BAD) return false;
        if (common == RankOption::PARSED) continue;
        switch (option) {
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
          case OPT_LEFTOVERS:
            out->common.leftovers = true;
            break;
          case OPT_HLL:
            out->hll = true;
            break;
          case 'h':
            *help = true;
            return true;
          default:
            fprintf(stderr, "error: %s\n", options.errmsg);
            return false;
        }
    }

    if (out->hll && !out->common.leftovers) {
        report(PROGRAM, "--hll goes with --leftovers");
        return false;
    }
    if (!check_common_args(PROGRAM, &options, &out->common)) return false;
    Search const& search = out->common.search;
    if (out->common.leftovers) {
        if (out->exact || out->all || out->completions > 0) {
            report(PROGRAM, "--leftovers doesn't go with --exact, --all or "
                   "--completions");
            return false;
        }
        return true;
    }
    if (search.pair_filter && search.words + search.pairs > 0 &&
        !out->exact) {
        report(PROGRAM, "--ss %s needs --exact", search_text(search).c_str());
        return false;
    }
    if (out->all && out->completions > 0) {
        report(PROGRAM, "--all doesn't go with --completions");
        return false;
    }
    if ((out->completions > 0 || out->all) &&
        out->common.rank.score.empty() &&
        search.prospects + search.pairs > 0) {
        report(PROGRAM, "--completions and --all go with --score or "
               "without ppb and pb steps");
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
