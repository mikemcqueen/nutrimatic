// pair-answers.cpp - List the crossword answers of each pair's clues, marked
// by how each fits a dictionary.

#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "log.h"
#include "optparse.h"
#include "pair-answers-impl.h"
#include "row-input.h"
#include "workflow-paths.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr char PROGRAM[] = "pair-answers";
constexpr int OPT_CLUES = 256;

struct Args {
    std::optional<ClueMatch> match;
    bool pairs_only = false;
    std::filesystem::path dict;
    std::filesystem::path clues = CLUEDATA_PATH;
    std::filesystem::path pairs;
};

void usage(std::string_view program, FILE* out) {
    fprintf(out, "usage: %.*s [-e|-j] [-p] [-d DICT] [--clues CLUEDATA] "
                 "PAIRS-FILE\n",
            SV_ARG(program));
    if (out != stdout) return;

    fputs("  for each phrase,phrase line of PAIRS-FILE (- for stdin) whose\n"
          "  clues have answers, print the pair, after a blank line unless it\n"
          "  is the first, then \"ANSWER: FLAGS\" for each answer, FLAGS\n"
          "  being M when ANSWER is a DICT word, P when it is a prefix of a\n"
          "  DICT word whose rest is a DICT word, S when it is such a suffix,\n"
          "  or X for none of them; without -e or -j a clue matches when it\n"
          "  has every word of both phrases, anywhere, in any order\n"
          "\noptions:\n", stdout);
    dfs_help_option("-e, --exact", "match clues that are the pair's words, "
                    "in either order, and nothing else");
    dfs_help_option("-j, --adjacent", "match clues where the pair's words "
                    "are adjacent, in either order");
    dfs_help_option("-p, --pairs-only", "print only the matching pairs, one "
                    "per line, without their answers; DICT isn't read");
    dfs_help_option("-d DICT", "the dictionary, one word per line "
                    "(default: $DICT)");
    dfs_help_option("--clues CLUEDATA",
                    "read the cluer clue index beside CLUEDATA, in its index "
                    "directory (default: %s)", CLUEDATA_PATH);
    dfs_help_option("-h, --help", "show this help");
}

bool parse_args(char* argv[], Args* out, bool* help) {
    static struct optparse_long const long_options[] = {
        { "exact", 'e', OPTPARSE_NONE },
        { "adjacent", 'j', OPTPARSE_NONE },
        { "pairs-only", 'p', OPTPARSE_NONE },
        { NULL, 'd', OPTPARSE_REQUIRED },
        { "clues", OPT_CLUES, OPTPARSE_REQUIRED },
        { "help", 'h', OPTPARSE_NONE },
        { NULL, 0, OPTPARSE_NONE },
    };
    struct optparse options;
    optparse_init(&options, argv);
    int option;
    while ((option = optparse_long(&options, long_options, NULL)) != -1) {
        switch (option) {
          case 'e':
          case 'j': {
            ClueMatch const match =
                option == 'e' ? ClueMatch::exact : ClueMatch::adjacent;
            if (out->match && *out->match != match) {
                report(PROGRAM, "-e and -j don't go together");
                return false;
            }
            out->match = match;
            break;
          }
          case 'p':
            out->pairs_only = true;
            break;
          case 'd':
            out->dict = options.optarg;
            break;
          case OPT_CLUES:
            out->clues = options.optarg;
            break;
          case 'h':
            *help = true;
            return true;
          default:
            fprintf(stderr, "error: %s\n", options.errmsg);
            return false;
        }
    }
    char const* const pairs = optparse_arg(&options);
    if (pairs == NULL || optparse_arg(&options) != NULL) return false;
    out->pairs = pairs;
    if (!out->match) out->match = ClueMatch::together;
    if (!out->pairs_only && out->dict.empty()) {
        char const* const env = getenv("DICT");
        if (env == NULL || *env == '\0') {
            report(PROGRAM, "needs -d DICT or $DICT");
            return false;
        }
        out->dict = env;
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

    AnswerDictionary dict;
    if (!args.pairs_only && !load_answer_dictionary(PROGRAM, args.dict, &dict))
        return 1;
    PairAnswers index;
    if (!index.open(PROGRAM, args.clues)) return 1;

    std::vector<std::string_view> answers;
    bool first = true;
    bool const read = read_rows(PROGRAM, args.pairs, "pairs",
        RowFormat::phrase_pairs,
        [&](uint64_t, DfsPairRow const& row, std::string const&) {
            index.answers(row.left, row.right, *args.match, &answers);
            if (answers.empty()) return true;
            if (args.pairs_only) {
                printf("%s,%s\n", row.left.c_str(), row.right.c_str());
                return true;
            }
            printf("%s%s,%s\n", first ? "" : "\n", row.left.c_str(),
                   row.right.c_str());
            first = false;
            for (std::string_view answer : answers)
                printf("%.*s: %s\n", SV_ARG(answer),
                       answer_flags(dict, answer).c_str());
            return true;
        });
    if (!read) return 1;
    if (fflush(stdout) != 0) {
        report(PROGRAM, "can't write output");
        return 1;
    }
    return 0;
}
