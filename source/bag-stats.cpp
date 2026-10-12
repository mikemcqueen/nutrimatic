// bag-stats.cpp - Rank pair letter bags by the exact word + pair completions
// they leave.

#include "bag-stats-impl.h"
#include "dfs-cli-help.h"
#include "letter-bag.h"
#include "optparse.h"
#include "pairs-impl.h"
#include "row-input.h"
#include "workflow-paths.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr char PROGRAM[] = "bag-stats";
constexpr int INNER_MIN_WORD_LENGTH = 3;

struct Args {
    std::string letters;
    std::string used_letters;
    char const* pairs_file = NULL;
    char const* dictionary_file = NULL;
};

struct PairBag {
    PairsCounts counts;
    std::string pair;
    uint64_t pairs = 0;
};

void usage(char const* program, FILE* out) {
    fprintf(out, "usage: %s [-u LETTERS] LETTERS PAIRS DICT\n", program);
    if (out != stdout) return;

    fprintf(stdout,
          "  for each letter bag of a PAIRS row that fits LETTERS, count the\n"
          "  DICT word bags that fit what is left, and the exact pairs\n"
          "  (pairs -m%d -x -c) of what is left after each; PAIRS or DICT\n"
          "  may be - for stdin\n"
          "\n"
          "  output: %s\n"
          "\noptions:\n", INNER_MIN_WORD_LENGTH, BAG_STATS_COLUMNS);
    dfs_help_used_letters();
    dfs_help_option("-h, --help", "show this help");
}

bool parse_args(char* argv[], Args* out, bool* help) {
    static struct optparse_long const long_options[] = {
        { "used-letters", 'u', OPTPARSE_REQUIRED },
        { "help", 'h', OPTPARSE_NONE },
        { NULL, 0, OPTPARSE_NONE },
    };

    struct optparse options;
    optparse_init(&options, argv);

    int option;
    while ((option = optparse_long(&options, long_options, NULL)) != -1) {
        switch (option) {
          case 'u':
            out->used_letters += options.optarg;
            break;
          case 'h':
            *help = true;
            return true;
          default:
            fprintf(stderr, "error: %s\n", options.errmsg);
            return false;
        }
    }

    char const* letters = optparse_arg(&options);
    out->pairs_file = optparse_arg(&options);
    out->dictionary_file = optparse_arg(&options);
    if (out->dictionary_file == NULL || optparse_arg(&options) != NULL)
        return false;
    out->letters = letters;
    if (strcmp(out->pairs_file, "-") == 0 &&
        strcmp(out->dictionary_file, "-") == 0) {
        fprintf(stderr, "%s: PAIRS and DICT can't both be -\n", PROGRAM);
        return false;
    }
    return true;
}

bool remaining_letters(Args const& args, std::string* out) {
    std::string bag;
    std::string remove;
    return clean_letters(args.letters.c_str(), "letters", &bag) &&
        clean_letters(args.used_letters.c_str(), "used letters", &remove) &&
        subtract_letters(bag, remove, out);
}

bool load_pair_bags(char const* path, PairsCounts const& bag,
                    std::vector<PairBag>* out) {
    std::unordered_set<std::string> rows;
    std::unordered_map<PairsCounts, size_t, PairsCountsHash> indices;
    return read_rows(PROGRAM, path, "pairs", false,
        [&](DfsPairRow const& row, std::string const&) {
            PairsCounts const counts =
                pairs_letter_counts((row.left + row.right).c_str());
            if (!pairs_fits(bag, counts)) return true;
            std::string const key = row.left < row.right
                ? row.left + "," + row.right : row.right + "," + row.left;
            if (!rows.insert(key).second) return true;
            auto const inserted = indices.emplace(counts, out->size());
            if (inserted.second) out->push_back({counts, row.left + " " + row.right});
            ++(*out)[inserted.first->second].pairs;
            return true;
        });
}

bool load_word_bags(char const* path, PairsCounts const& bag,
                    std::vector<PairsCounts>* out) {
    std::unordered_set<PairsCounts, PairsCountsHash> seen;
    return read_lines(PROGRAM, path,
        [&](std::string const& line, size_t) {
            PairsCounts const counts = pairs_letter_counts(line.c_str());
            if (counts == PairsCounts{} || !pairs_fits(bag, counts) ||
                !seen.insert(counts).second)
                return true;
            out->push_back(counts);
            return true;
        });
}

bool load_inner_dictionary(PairsExactCounter* out) {
    char const* const root = require_workflow_root(PROGRAM);
    if (root == NULL) return false;
    std::string const path =
        (std::filesystem::path(root) / WORKFLOW_DICT_PATH).string();
    FILE* wf = fopen(path.c_str(), "r");
    if (!wf) { perror(path.c_str()); return false; }

    char line[4096];
    std::string word;
    while (fgets(line, sizeof(line), wf)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        word.assign(line, len);
        out->add(word);
    }
    fclose(wf);
    return true;
}

int letter_total(PairsCounts const& counts) {
    int total = 0;
    for (uint8_t value : counts.values) total += value;
    return total;
}

std::string bag_letters(PairsCounts const& counts) {
    std::string out;
    for (int i = 0; i < 26; ++i) out.append(counts.values[i], 'a' + i);
    return out;
}

}  // namespace

int main(int argc, char* argv[]) {
    Args args;
    bool help = false;
    std::string letters;
    if (!parse_args(argv, &args, &help) ||
        (!help && !remaining_letters(args, &letters))) {
        usage(argv[0], stderr);
        return 2;
    }
    if (help) {
        usage(argv[0], stdout);
        return 0;
    }

    PairsCounts const bag = pairs_letter_counts(letters.c_str());
    std::vector<PairBag> pair_bags;
    std::vector<PairsCounts> word_bags;
    PairsExactCounter counter(bag, INNER_MIN_WORD_LENGTH);
    if (!load_pair_bags(args.pairs_file, bag, &pair_bags) ||
        !load_word_bags(args.dictionary_file, bag, &word_bags) ||
        !load_inner_dictionary(&counter))
        return 1;

    setvbuf(stdout, nullptr, _IOFBF, 1 << 22);
    write_bag_stats_header(stdout, {letters, INNER_MIN_WORD_LENGTH,
                                    args.pairs_file, args.dictionary_file});

    std::unordered_map<PairsCounts, uint64_t, PairsCountsHash> cache;
    PairsCounts pair_rest;
    PairsCounts word_rest;
    for (PairBag const& pair_bag : pair_bags) {
        pairs_subtract(bag, pair_bag.counts, &pair_rest);
        BagStatsRow row{pair_bag.pair, bag_letters(pair_bag.counts),
                        pair_bag.pairs, 0, 0, 0, 0, 0};
        for (PairsCounts const& word_bag : word_bags) {
            if (!pairs_subtract(pair_rest, word_bag, &word_rest)) continue;
            ++row.word_bags;
            if (word_rest == PairsCounts{}) {
                ++row.solo;
                continue;
            }
            if (letter_total(word_rest) < 2 * INNER_MIN_WORD_LENGTH) continue;
            auto found = cache.find(word_rest);
            if (found == cache.end())
                found = cache.emplace(word_rest, counter.count(word_rest)).first;
            uint64_t const count = found->second;
            if (count == 0) continue;
            ++row.with_pairs;
            row.total_pairs += count;
            row.max_pairs = std::max(row.max_pairs, count);
        }
        write_bag_stats_row(stdout, row);
    }
    return 0;
}
