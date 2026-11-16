// pair-answers.cpp - List the crossword answers of each pair's clues, marked
// by how each fits a dictionary.

#include "clue-index.h"
#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "log.h"
#include "optparse.h"
#include "row-input.h"
#include "workflow-paths.h"

#include <algorithm>
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
    std::filesystem::path dict;
    std::filesystem::path clues = CLUEDATA_PATH;
    std::filesystem::path pairs;
};

struct Dictionary {
    DfsDictionary words;
    std::vector<std::string> forward;
    // Each word spelled backwards, sorted.
    std::vector<std::string> reversed;
};

void usage(std::string_view program, FILE* out) {
    fprintf(out, "usage: %.*s -e|-j [-d DICT] [--clues CLUEDATA] PAIRS-FILE\n",
            SV_ARG(program));
    if (out != stdout) return;

    fputs("  for each phrase,phrase line of PAIRS-FILE (- for stdin) whose\n"
          "  clues have answers, print the pair, after a blank line unless it\n"
          "  is the first, then \"ANSWER: FLAGS\" for each answer, FLAGS\n"
          "  being M when ANSWER is a DICT word, P when it is a prefix of a\n"
          "  DICT word whose rest is a DICT word, S when it is such a suffix,\n"
          "  or X for none of them\n"
          "\noptions:\n", stdout);
    dfs_help_option("-e", "match clues that are the pair and nothing else, "
                    "as cluer's query_index.py -e does");
    dfs_help_option("-j", "match clues where the pair's words are adjacent, "
                    "as cluer's query_index.py -j does");
    dfs_help_option("-d DICT", "the dictionary, one word per line "
                    "(default: $DICT)");
    dfs_help_option("--clues CLUEDATA",
                    "read the cluer clue index beside CLUEDATA, in its index "
                    "directory (default: %s)", CLUEDATA_PATH);
    dfs_help_option("-h, --help", "show this help");
}

bool parse_args(char* argv[], Args* out, bool* help) {
    static struct optparse_long const long_options[] = {
        { NULL, 'e', OPTPARSE_NONE },
        { NULL, 'j', OPTPARSE_NONE },
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
    if (!out->match) {
        report(PROGRAM, "needs -e or -j");
        return false;
    }
    if (out->dict.empty()) {
        char const* const env = getenv("DICT");
        if (env == NULL || *env == '\0') {
            report(PROGRAM, "needs -d DICT or $DICT");
            return false;
        }
        out->dict = env;
    }
    return true;
}

bool load(std::filesystem::path const& path, Dictionary* out) {
    if (!load_dictionary(path, &out->words)) return false;
    if (out->words.empty()) {
        report(PROGRAM, "%s: no words", path.c_str());
        return false;
    }
    out->forward.assign(out->words.begin(), out->words.end());
    std::sort(out->forward.begin(), out->forward.end());
    out->reversed.reserve(out->forward.size());
    for (std::string const& word : out->forward)
        out->reversed.emplace_back(word.rbegin(), word.rend());
    std::sort(out->reversed.begin(), out->reversed.end());
    return true;
}

// Whether a word of `sorted` starts with `part` and goes on to a word of
// `words`, spelled backwards when `backwards`.
bool splits(std::vector<std::string> const& sorted, DfsDictionary const& words,
            std::string_view part, bool backwards) {
    for (auto it = std::lower_bound(sorted.begin(), sorted.end(), part);
         it != sorted.end() && it->starts_with(part); ++it) {
        if (it->size() == part.size()) continue;
        std::string rest = it->substr(part.size());
        if (backwards) std::reverse(rest.begin(), rest.end());
        if (words.contains(rest)) return true;
    }
    return false;
}

std::string flags(Dictionary const& dict, std::string_view answer) {
    std::string word(answer);
    for (char& c : word)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    std::string const backwards(word.rbegin(), word.rend());
    std::string out;
    if (dict.words.contains(word)) out += 'M';
    if (splits(dict.forward, dict.words, word, false)) out += 'P';
    if (splits(dict.reversed, dict.words, backwards, true)) out += 'S';
    if (out.empty()) out = "X";
    return out;
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

    Dictionary dict;
    if (!load(args.dict, &dict)) return 1;
    ClueIndex index;
    std::string error;
    if (!index.open(args.clues, &error)) {
        report(PROGRAM, "%s", error.c_str());
        return 1;
    }

    std::vector<std::string_view> answers;
    bool first = true;
    bool const read = read_rows(PROGRAM, args.pairs, "pairs",
        RowFormat::phrase_pairs,
        [&](uint64_t, DfsPairRow const& row, std::string const&) {
            index.answers(row.left, row.right, *args.match, &answers);
            if (answers.empty()) return true;
            std::sort(answers.begin(), answers.end());
            printf("%s%s,%s\n", first ? "" : "\n", row.left.c_str(),
                   row.right.c_str());
            first = false;
            for (std::string_view answer : answers)
                printf("%.*s: %s\n", SV_ARG(answer),
                       flags(dict, answer).c_str());
            return true;
        });
    if (!read) return 1;
    if (fflush(stdout) != 0) {
        report(PROGRAM, "can't write output");
        return 1;
    }
    return 0;
}
