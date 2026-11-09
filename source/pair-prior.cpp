// pair-prior.cpp - Score word pairs by phase 1 classifier verdicts and
// crossword clue evidence, for bag-stats --prior.

#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "log.h"
#include "optparse.h"
#include "pair-evidence.h"
#include "row-input.h"
#include "workflow-paths.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <vector>

namespace {

constexpr char PROGRAM[] = "pair-prior";
constexpr int OPT_P1 = 256;
constexpr int OPT_CLUES = 257;
constexpr int OPT_FEATURES = 258;
constexpr int OPT_PRIOR_WEIGHTS = 259;
constexpr int OPT_RANKED = 260;
constexpr int OPT_AS_PAIRS = 261;
constexpr int OPT_MANIFEST = 262;

struct Args {
    PriorPaths paths;
    std::optional<std::string> prior_weights;
    std::optional<std::filesystem::path> manifest;
    bool features = false;
    bool ranked = false;
    bool as_pairs = false;
    std::optional<int> limit;
    std::vector<std::filesystem::path> pairs;
};

struct ScoredRow {
    double score;
    std::string left;
    std::string right;
};

void usage(std::string_view program, FILE* out) {
    fprintf(out, "usage: %.*s [--p1 INDEX] [--clues CLUEDATA]\n"
            "       [--prior-weights SPEC | --manifest FILE | --features]\n"
            "       [--ranked [--as-pairs] [-n N]] PAIRS...\n",
            SV_ARG(program));
    if (out != stdout) return;

    fputs("  print \"SCORE word,word\" for each distinct pair of PAIRS, in\n"
          "  order, SCORE as --prior-weights weighs it; PAIRS may be - for\n"
          "  stdin\n"
          "\noptions:\n", stdout);
    dfs_help_option("--p1 INDEX",
                    "look pairs up in INDEX, as make-p1-index writes it "
                    "(default: %s when it exists, skipped with a warning when it "
                    "can't be opened)", P1_INDEX_PATH);
    dfs_help_option("--clues CLUEDATA",
                    "look pairs up in the cluer clue index beside CLUEDATA, "
                    "in its index directory (default: %s when it exists, "
                    "skipped with a warning when it can't be opened)",
                    CLUEDATA_PATH);
    dfs_help_option("--prior-weights SPEC",
                    "SCORE is the sum of each term's weight times its value, "
                    "SPEC comma-separated NAME=WEIGHT, unnamed terms 0; "
                    "the p1 terms need the p1 index, the others the clue "
                    "index (default %s, less the terms of an index not "
                    "opened)",
                    PRIOR_WEIGHTS_DEFAULT);
    dfs_help_option("--manifest FILE",
                    "read the prior from FILE, TERM=WEIGHT lines with weights "
                    "not negative, plus options=squared to square it; an "
                    "index a term needs must open");
    dfs_help_option("--features",
                    "print \"AB BA TOGETHER ADJACENT ADJACENT_SHORT EXACT "
                    "MIN_WORDS LINKS LINK_EXACT LINK_MIN_WORDS PHRASE "
                    "CLUES_A CLUES_B word,word\" instead: the phase 1 YES "
                    "of each word order (-1 without a verdict) and the "
                    "clue counts");
    dfs_help_option("--ranked",
                    "the one PAIRS file holds COUNT word word lines; print "
                    "\"SCORE word word\", SCORE being COUNT times the prior, "
                    "rounded, by descending score then pair; a pair listed "
                    "twice, in either order, or a negative weight is an "
                    "error");
    dfs_help_option("--as-pairs",
                    "with --ranked, print word,word in that order, no score");
    dfs_help_option("-n N",
                    "with --ranked, print only the first N rows; 0 prints all");
    dfs_help_option("-h, --help", "show this help");
    fputs("\n--prior-weights terms:\n", stdout);
    help_prior_terms();
}

bool parse_args(char* argv[], Args* out, bool* help) {
    static struct optparse_long const long_options[] = {
        { "p1", OPT_P1, OPTPARSE_REQUIRED },
        { "clues", OPT_CLUES, OPTPARSE_REQUIRED },
        { "prior-weights", OPT_PRIOR_WEIGHTS, OPTPARSE_REQUIRED },
        { "manifest", OPT_MANIFEST, OPTPARSE_REQUIRED },
        { "features", OPT_FEATURES, OPTPARSE_NONE },
        { "ranked", OPT_RANKED, OPTPARSE_NONE },
        { "as-pairs", OPT_AS_PAIRS, OPTPARSE_NONE },
        { NULL, 'n', OPTPARSE_REQUIRED },
        { "help", 'h', OPTPARSE_NONE },
        { NULL, 0, OPTPARSE_NONE },
    };
    struct optparse options;
    optparse_init(&options, argv);
    int option;
    while ((option = optparse_long(&options, long_options, NULL)) != -1) {
        switch (option) {
          case OPT_P1:
            out->paths.p1 = options.optarg;
            break;
          case OPT_CLUES:
            out->paths.clues = options.optarg;
            break;
          case OPT_PRIOR_WEIGHTS:
            out->prior_weights = options.optarg;
            break;
          case OPT_MANIFEST:
            out->manifest = options.optarg;
            break;
          case OPT_FEATURES:
            out->features = true;
            break;
          case OPT_RANKED:
            out->ranked = true;
            break;
          case OPT_AS_PAIRS:
            out->as_pairs = true;
            break;
          case 'n': {
            int limit;
            if (!parse_count(options.optarg, "-n", &limit)) return false;
            out->limit = limit;
            break;
          }
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
    default_prior_paths(&out->paths);
    if (!out->manifest && !out->paths.p1 && !out->paths.clues) {
        report(PROGRAM, "needs --p1, --clues or both, or %s or %s here",
               P1_INDEX_PATH, CLUEDATA_PATH);
        return false;
    }
    if (out->features && out->prior_weights) {
        report(PROGRAM, "--features and --prior-weights don't go together");
        return false;
    }
    if (out->manifest && (out->prior_weights || out->features)) {
        report(PROGRAM,
               "--manifest doesn't go with --prior-weights or --features");
        return false;
    }
    if (out->ranked && out->features) {
        report(PROGRAM, "--ranked and --features don't go together");
        return false;
    }
    if (out->ranked && out->pairs.size() != 1) {
        report(PROGRAM, "--ranked takes exactly one file");
        return false;
    }
    if (!out->ranked && (out->as_pairs || out->limit)) {
        report(PROGRAM, "--as-pairs and -n require --ranked");
        return false;
    }
    return true;
}

bool read_manifest(std::filesystem::path const& path, std::string* spec,
                   bool* squared) {
    return read_lines(PROGRAM, path,
        [&](std::string const& line, size_t number) {
            size_t const equals = line.find('=');
            std::string_view const value =
                equals == std::string::npos
                    ? std::string_view()
                    : std::string_view(line).substr(equals + 1);
            if (line.compare(0, equals, "options") == 0) {
                if (value != "squared" || *squared) {
                    report(PROGRAM, "%s line %zu: bad or repeated option",
                           path.c_str(), number);
                    return false;
                }
                *squared = true;
                return true;
            }
            if (line.empty() || line.find(',') != std::string::npos) {
                report(PROGRAM, "%s line %zu: expected TERM=WEIGHT",
                       path.c_str(), number);
                return false;
            }
            if (value.starts_with('-')) {
                report(PROGRAM, "%s line %zu: negative weight", path.c_str(),
                       number);
                return false;
            }
            if (!spec->empty()) *spec += ',';
            *spec += line;
            return true;
        });
}

bool open_manifest_prior(Args const& args, bool* squared, PriorWeights* weights,
                         PairEvidenceSource* source) {
    std::filesystem::path const& manifest = *args.manifest;
    std::string spec;
    if (!read_manifest(manifest, &spec, squared)) return false;
    if (spec.empty()) {
        report(PROGRAM, "%s: no TERM=WEIGHT line", manifest.c_str());
        return false;
    }
    std::string error;
    std::string stale;
    PriorIndex failed;
    if (!open_prior(args.paths.p1, args.paths.clues, spec, false,
                    manifest.native(), weights, source, &error, &failed,
                    &stale)) {
        if (failed == PriorIndex::none)
            report(PROGRAM, "%s", error.c_str());
        else
            report(PROGRAM, "%s: %s", manifest.c_str(), error.c_str());
        return false;
    }
    if (!stale.empty()) alert(PROGRAM, "%s", stale.c_str());
    return true;
}

uint64_t rounded_score(double score) {
    double const limit = 18446744073709551616.0;
    if (score <= 0) return 0;
    if (score >= limit) return std::numeric_limits<uint64_t>::max();
    return uint64_t(std::round(score));
}

bool print_ranked(std::vector<ScoredRow>* rows, Args const& args) {
    size_t const count = args.limit && *args.limit != 0
        ? std::min(size_t(*args.limit), rows->size())
        : rows->size();
    std::partial_sort(rows->begin(), rows->begin() + count, rows->end(),
        [](ScoredRow const& a, ScoredRow const& b) {
            if (a.score != b.score) return a.score > b.score;
            return std::tie(a.left, a.right) < std::tie(b.left, b.right);
        });
    for (size_t i = 0; i < count; ++i) {
        ScoredRow const& row = (*rows)[i];
        if (args.as_pairs)
            printf("%s,%s\n", row.left.c_str(), row.right.c_str());
        else
            printf("%" PRIu64 " %s %s\n", rounded_score(row.score),
                   row.left.c_str(), row.right.c_str());
    }
    if (fflush(stdout) != 0) {
        report(PROGRAM, "can't write output");
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
    bool squared = false;
    if (args.manifest) {
        if (!open_manifest_prior(args, &squared, &weights, &source)) return 1;
    } else {
        if (!open_default_prior(PROGRAM, &args.paths, args.prior_weights,
                                args.features, &weights, &source))
            return 1;
        if (!args.paths.p1 && !args.paths.clues) {
            report(PROGRAM, "no index left to read");
            return 1;
        }
    }
    if (args.ranked &&
        std::any_of(weights.weights.begin(), weights.weights.end(),
                    [](double weight) { return weight < 0; })) {
        report(PROGRAM, "--ranked needs weights not negative");
        return 1;
    }
    std::unordered_set<std::string> seen;
    std::vector<ScoredRow> ranked;
    RowFormat const format = args.ranked ? RowFormat::ranked : RowFormat::pairs;
    for (std::filesystem::path const& path : args.pairs) {
        bool const read = read_rows(PROGRAM, path, "pairs", format,
            [&](uint64_t count, DfsPairRow const& row, std::string const&) {
                if (!seen.insert(pair_key(row.left, row.right)).second) {
                    if (!args.ranked) return true;
                    report(PROGRAM, "%s line %zu: duplicate pair %s,%s",
                           path.c_str(), row.line_number, row.left.c_str(),
                           row.right.c_str());
                    return false;
                }
                PairEvidence const e = source.lookup(row.left, row.right);
                if (!args.features) {
                    double prior = prior_score(e, weights);
                    if (squared) prior *= prior;
                    if (args.ranked)
                        ranked.push_back(
                            {double(count) * prior, row.left, row.right});
                    else
                        printf("%.6f %s,%s\n", prior, row.left.c_str(),
                               row.right.c_str());
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
    if (args.ranked && !print_ranked(&ranked, args)) return 1;
    return 0;
}
