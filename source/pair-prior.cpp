// pair-prior.cpp - Score word pairs by phase 1 classifier verdicts and
// crossword clue evidence, for bag-stats --prior.

#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "optparse.h"
#include "pair-evidence.h"
#include "row-input.h"

#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

constexpr char PROGRAM[] = "pair-prior";
constexpr int OPT_P1 = 256;
constexpr int OPT_CLUES = 257;
constexpr int OPT_FEATURES = 258;
constexpr int OPT_PRIOR_WEIGHTS = 259;

struct Args {
    char const* p1 = NULL;
    char const* clues = NULL;
    char const* prior_weights = NULL;
    bool features = false;
    std::vector<char const*> pairs;
};

void usage(char const* program, FILE* out) {
    fprintf(out, "usage: %s [--p1 INDEX] [--clues CLUEDATA]\n"
            "       [--prior-weights SPEC | --features] PAIRS...\n", program);
    if (out != stdout) return;

    fputs("  print \"SCORE word,word\" for each distinct pair of PAIRS, in\n"
          "  order, SCORE as --prior-weights weighs it; PAIRS may be - for\n"
          "  stdin\n"
          "\noptions:\n", stdout);
    dfs_help_option("--p1 INDEX",
                    "look pairs up in INDEX, as make-p1-index writes it");
    dfs_help_option("--clues CLUEDATA",
                    "look pairs up in the cluer clue index beside CLUEDATA, "
                    "in its index directory (e.g. ../words/cluer/data/"
                    "cluedata)");
    dfs_help_option("--prior-weights SPEC",
                    "SCORE is the sum of each term's weight times its value, "
                    "SPEC comma-separated NAME=WEIGHT, unnamed terms 0; "
                    "the p1 terms need --p1, the others --clues (default "
                    "%s, less the terms of an index not given)",
                    PRIOR_WEIGHTS_DEFAULT);
    dfs_help_option("--features",
                    "print \"AB BA TOGETHER ADJACENT ADJACENT_SHORT EXACT "
                    "MIN_WORDS LINKS LINK_EXACT LINK_MIN_WORDS PHRASE "
                    "CLUES_A CLUES_B word,word\" instead: the phase 1 YES "
                    "of each word order (-1 without a verdict) and the "
                    "clue counts");
    dfs_help_option("-h, --help", "show this help");
    fputs("\n--prior-weights terms:\n", stdout);
    help_prior_terms();
}

bool parse_args(char* argv[], Args* out, bool* help) {
    static struct optparse_long const long_options[] = {
        { "p1", OPT_P1, OPTPARSE_REQUIRED },
        { "clues", OPT_CLUES, OPTPARSE_REQUIRED },
        { "prior-weights", OPT_PRIOR_WEIGHTS, OPTPARSE_REQUIRED },
        { "features", OPT_FEATURES, OPTPARSE_NONE },
        { "help", 'h', OPTPARSE_NONE },
        { NULL, 0, OPTPARSE_NONE },
    };
    struct optparse options;
    optparse_init(&options, argv);
    int option;
    while ((option = optparse_long(&options, long_options, NULL)) != -1) {
        switch (option) {
          case OPT_P1:
            out->p1 = options.optarg;
            break;
          case OPT_CLUES:
            out->clues = options.optarg;
            break;
          case OPT_PRIOR_WEIGHTS:
            out->prior_weights = options.optarg;
            break;
          case OPT_FEATURES:
            out->features = true;
            break;
          case 'h':
            *help = true;
            return true;
          default:
            fprintf(stderr, "error: %s\n", options.errmsg);
            return false;
        }
    }
    for (char const* arg; (arg = optparse_arg(&options)) != NULL;)
        out->pairs.push_back(arg);
    if (out->pairs.empty()) return false;
    if (out->p1 == NULL && out->clues == NULL) {
        fprintf(stderr, "%s: needs --p1, --clues or both\n", PROGRAM);
        return false;
    }
    if (out->features && out->prior_weights != NULL) {
        fprintf(stderr, "%s: --features and --prior-weights don't go "
                "together\n", PROGRAM);
        return false;
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

    PriorWeights weights;
    PairEvidenceSource source;
    std::string error;
    if (!open_prior(args.p1, args.clues, args.prior_weights, args.features,
                    &weights, &source, &error)) {
        fprintf(stderr, "%s: %s\n", PROGRAM, error.c_str());
        return 1;
    }
    std::unordered_set<std::string> seen;
    for (char const* path : args.pairs) {
        bool const read = read_rows(PROGRAM, path, "pairs", false,
            [&](DfsPairRow const& row, std::string const&) {
                if (!seen.insert(pair_key(row.left, row.right)).second)
                    return true;
                PairEvidence const e = source.lookup(row.left, row.right);
                if (!args.features) {
                    printf("%.6f %s,%s\n", prior_score(e, weights),
                           row.left.c_str(), row.right.c_str());
                    return true;
                }
                printf("%.6f %.6f %u %u %u %u %u %u %u %u %u %u %u %s,%s\n",
                       e.p1 ? e.yes.ab : -1.0, e.p1 ? e.yes.ba : -1.0,
                       e.clues.together, e.clues.adjacent,
                       e.clues.adjacent_short, e.clues.exact,
                       e.clues.min_words, e.clues.answer_links,
                       e.clues.answer_exact, e.clues.answer_min_words,
                       e.clues.phrase_answer, e.clues.clues_a,
                       e.clues.clues_b, row.left.c_str(), row.right.c_str());
                return true;
            });
        if (!read) return 1;
    }
    return 0;
}
