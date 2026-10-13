// pair-prior.cpp - Score word pairs by phase 1 classifier verdicts and
// crossword clue evidence, for bag-stats --prior.

#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "optparse.h"
#include "row-input.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr char PROGRAM[] = "pair-prior";
constexpr int OPT_P1 = 256;
constexpr int OPT_CLUES = 257;

struct Args {
    char const* p1 = NULL;
    char const* clues = NULL;
    std::vector<char const*> pairs;
};

// What is known about one pair.
struct Evidence {
    double yes = 0;
    bool answer_link = false;
    bool adjacent = false;
};

void usage(char const* program, FILE* out) {
    fprintf(out, "usage: %s [--p1 DIR] [--clues FILE] PAIRS...\n", program);
    if (out != stdout) return;

    fputs("  print \"SCORE word,word\" for each distinct pair of PAIRS, in\n"
          "  order: the smaller of the pair's two phase 1 YES probabilities\n"
          "  (forward and reverse, the best of its jsonl lines, 0 when it has\n"
          "  none), plus 1 when one word is a clue answer whose clue has the\n"
          "  other word, plus 1 when the two words are adjacent in a clue,\n"
          "  one space or hyphen apart;\n"
          "  PAIRS may be - for stdin\n"
          "\noptions:\n", stdout);
    dfs_help_option("--p1 DIR",
                    "read the phase 1 verdicts from every *.jsonl file in DIR "
                    "(e.g. $WFROOT/.wf/p1/done/out)");
    dfs_help_option("--clues FILE",
                    "read crossword clues and answers from FILE, the cluer "
                    "cluedata format (e.g. cluer/data/cluedata)");
    dfs_help_option("-h, --help", "show this help");
}

bool parse_args(char* argv[], Args* out, bool* help) {
    static struct optparse_long const long_options[] = {
        { "p1", OPT_P1, OPTPARSE_REQUIRED },
        { "clues", OPT_CLUES, OPTPARSE_REQUIRED },
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
    return true;
}

std::string pair_key(std::string_view a, std::string_view b) {
    if (b < a) std::swap(a, b);
    std::string out(a);
    out += ',';
    out += b;
    return out;
}

// The YES probability after `direction` ("\"fwd\"" or "\"rvs\"") in a phase
// 1 jsonl line, 0 when it lists none.
double yes_after(std::string const& line, char const* direction) {
    size_t const start = line.find(direction);
    if (start == std::string::npos) return 0;
    size_t const end = line.find(']', start);
    size_t const yes = line.find("\"YES\":", start);
    if (yes == std::string::npos || yes > end) return 0;
    return strtod(line.c_str() + yes + 6, NULL);
}

bool read_p1(char const* dir,
             std::unordered_map<std::string, Evidence>* evidence) {
    std::error_code error;
    std::vector<std::filesystem::path> files;
    for (auto const& entry : std::filesystem::directory_iterator(dir, error))
        if (entry.path().extension() == ".jsonl") files.push_back(entry.path());
    if (error) {
        fprintf(stderr, "%s: %s: %s\n", PROGRAM, dir, error.message().c_str());
        return false;
    }
    std::sort(files.begin(), files.end());
    std::string line;
    for (std::filesystem::path const& file : files) {
        std::ifstream in(file);
        if (!in) {
            fprintf(stderr, "%s: can't read %s\n", PROGRAM, file.c_str());
            return false;
        }
        while (std::getline(in, line)) {
            size_t const start = line.find("\"pair\": \"");
            if (start == std::string::npos) continue;
            size_t const begin = start + 9;
            size_t const end = line.find('"', begin);
            std::string_view const pair(line.data() + begin, end - begin);
            size_t const comma = pair.find(',');
            if (comma == std::string_view::npos) continue;
            auto const found = evidence->find(
                pair_key(pair.substr(0, comma), pair.substr(comma + 1)));
            if (found == evidence->end()) continue;
            double const yes = std::min(yes_after(line, "\"fwd\""),
                                        yes_after(line, "\"rvs\""));
            found->second.yes = std::max(found->second.yes, yes);
        }
    }
    return true;
}

uint32_t read_u32(std::string const& data, size_t offset) {
    uint32_t out;
    memcpy(&out, data.data() + offset, 4);
    return out;
}

// The clue's words as cluer's index sees them: lowercase, apostrophes
// deleted, split on anything else that isn't a-z or 0-9. `adjacent[i]` says
// whether word i follows word i - 1 after exactly one space or hyphen.
void clue_words(std::string_view clue, std::vector<std::string>* words,
                std::vector<char>* adjacent) {
    words->clear();
    adjacent->clear();
    std::string text;
    for (char c : clue)
        if (c != '\'')
            text += char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    size_t previous_end = std::string::npos;
    for (size_t i = 0; i < text.size();) {
        auto const word_char = [&](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        };
        if (!word_char(text[i])) { ++i; continue; }
        size_t j = i;
        while (j < text.size() && word_char(text[j])) ++j;
        adjacent->push_back(previous_end != std::string::npos &&
                            i == previous_end + 1 &&
                            (text[previous_end] == ' ' ||
                             text[previous_end] == '-'));
        words->push_back(text.substr(i, j - i));
        previous_end = j;
        i = j;
    }
}

bool read_clues(char const* path,
                std::unordered_map<std::string, Evidence>* evidence) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        fprintf(stderr, "%s: can't read %s\n", PROGRAM, path);
        return false;
    }
    std::string const data((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    std::unordered_set<std::string> vocabulary;
    for (auto const& [key, value] : *evidence) {
        size_t const comma = key.find(',');
        vocabulary.insert(key.substr(0, comma));
        vocabulary.insert(key.substr(comma + 1));
    }

    auto const truncated = [&] {
        fprintf(stderr, "%s: %s is truncated\n", PROGRAM, path);
        return false;
    };
    if (data.size() < 4) return truncated();
    size_t offset = 4;
    std::vector<std::string> answers(read_u32(data, 0));
    for (std::string& answer : answers) {
        if (offset >= data.size()) return truncated();
        size_t const length = uint8_t(data[offset]);
        if (offset + 1 + length > data.size()) return truncated();
        std::vector<std::string> words;
        std::vector<char> adjacent;
        clue_words(std::string_view(data).substr(offset + 1, length), &words,
                   &adjacent);
        if (words.size() == 1 && vocabulary.count(words[0]))
            answer = words[0];
        offset += 1 + length;
    }
    if (offset + 4 > data.size()) return truncated();
    uint32_t const clues = read_u32(data, offset);
    offset += 4;

    std::vector<std::string> words;
    std::vector<char> adjacent;
    std::vector<std::string const*> known;
    for (uint32_t c = 0; c < clues; ++c) {
        if (offset >= data.size()) return truncated();
        size_t const length = uint8_t(data[offset]);
        if (offset + 1 + length + 4 > data.size()) return truncated();
        clue_words(std::string_view(data).substr(offset + 1, length), &words,
                   &adjacent);
        offset += 1 + length;
        uint32_t const refs = read_u32(data, offset);
        offset += 4;
        if (offset + 4 * size_t(refs) > data.size()) return truncated();

        known.clear();
        for (size_t i = 0; i < words.size(); ++i) {
            if (!vocabulary.count(words[i])) continue;
            known.push_back(&words[i]);
            if (i == 0 || !adjacent[i] || !vocabulary.count(words[i - 1]))
                continue;
            auto const found = evidence->find(pair_key(words[i - 1], words[i]));
            if (found != evidence->end()) found->second.adjacent = true;
        }
        for (uint32_t r = 0; r < refs; ++r) {
            uint32_t const answer = read_u32(data, offset + 4 * size_t(r)) >> 1;
            if (answer >= answers.size() || answers[answer].empty()) continue;
            for (std::string const* word : known) {
                if (*word == answers[answer]) continue;
                auto const found =
                    evidence->find(pair_key(answers[answer], *word));
                if (found != evidence->end()) found->second.answer_link = true;
            }
        }
        offset += 4 * size_t(refs);
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

    std::unordered_map<std::string, Evidence> evidence;
    std::vector<std::string> order;
    for (char const* path : args.pairs) {
        bool const read = read_rows(PROGRAM, path, "pairs", false,
            [&](DfsPairRow const& row, std::string const&) {
                std::string key = pair_key(row.left, row.right);
                if (evidence.emplace(key, Evidence{}).second)
                    order.push_back(row.left + "," + row.right);
                return true;
            });
        if (!read) return 1;
    }
    if ((args.p1 != NULL && !read_p1(args.p1, &evidence)) ||
        (args.clues != NULL && !read_clues(args.clues, &evidence)))
        return 1;

    for (std::string const& pair : order) {
        size_t const comma = pair.find(',');
        Evidence const& e = evidence.at(
            pair_key(std::string_view(pair).substr(0, comma),
                     std::string_view(pair).substr(comma + 1)));
        printf("%.6f %s\n", e.yes + e.answer_link + e.adjacent, pair.c_str());
    }
    return 0;
}
