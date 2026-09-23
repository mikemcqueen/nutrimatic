// pairs.cpp - Find dictionary word pairs that fit within a letter bag.

#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "optparse.h"
#include "workflow-paths.h"

#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <smmintrin.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

static constexpr size_t ALPHA = 26;
static constexpr size_t COUNT_LANES = 32;

struct alignas(32) Counts {
    std::array<uint8_t, COUNT_LANES> values{};

    bool operator==(Counts const&) const = default;
};

static_assert(sizeof(Counts) == COUNT_LANES);
static_assert(alignof(Counts) == COUNT_LANES);

struct CountsHash {
    size_t operator()(Counts const& counts) const {
        uint64_t hash = UINT64_C(14695981039346656037);
        for (uint8_t value : counts.values) {
            hash ^= value;
            hash *= UINT64_C(1099511628211);
        }
        return static_cast<size_t>(hash);
    }
};

static Counts letter_counts(char const* s) {
    Counts out{};
    for (; *s; ++s) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c >= 'a' && c <= 'z') ++out.values[c - 'a'];
    }
    return out;
}

static bool subtract(
    Counts const& pool, Counts const& word, Counts* out) {
    Counts result{};
    for (size_t i = 0; i < ALPHA; ++i) {
        if (word.values[i] > pool.values[i]) return false;
        result.values[i] = pool.values[i] - word.values[i];
    }
    *out = result;
    return true;
}

static bool fits(Counts const& pool, Counts const& word) {
    __m128i const* p = reinterpret_cast<__m128i const*>(pool.values.data());
    __m128i const* w = reinterpret_cast<__m128i const*>(word.values.data());
    __m128i const excess = _mm_or_si128(
        _mm_subs_epu8(_mm_load_si128(w), _mm_load_si128(p)),
        _mm_subs_epu8(_mm_load_si128(w + 1), _mm_load_si128(p + 1)));
    return _mm_testz_si128(excess, excess);
}

namespace {

constexpr int DEFAULT_MIN_WORD_LENGTH = 4;

struct Args {
    char const* dictionary_file = NULL;
    std::string letters;
    int min_word_length = DEFAULT_MIN_WORD_LENGTH;
};

void usage(char const* program, FILE* out) {
    fprintf(out, "usage: %s [-d FILE] [-u LETTERS] [-m N] LETTERS\n",
            program);
    if (out != stdout) return;

    fputs("\noptions:\n", stdout);
    dfs_help_option("-d, --dict FILE",
        "read words from FILE (default: $WFROOT/%s)",
        WORKFLOW_DICT_PATH);
    dfs_help_used_letters();
    dfs_help_option("-m, --min_word_length N",
        "minimum word length (default: %d; 0 for no minimum)",
        DEFAULT_MIN_WORD_LENGTH);
    dfs_help_option("-h, --help", "show this help");
}

bool parse_args(char* argv[], Args* out, bool* help) {
    static struct optparse_long const long_options[] = {
        { "dict", 'd', OPTPARSE_REQUIRED },
        { "used-letters", 'u', OPTPARSE_REQUIRED },
        { "min_word_length", 'm', OPTPARSE_REQUIRED },
        { "help", 'h', OPTPARSE_NONE },
        { NULL, 0, OPTPARSE_NONE },
    };

    struct optparse options;
    optparse_init(&options, argv);

    std::string used_letters;
    int option;
    while ((option = optparse_long(&options, long_options, NULL)) != -1) {
        switch (option) {
          case 'd':
            out->dictionary_file = options.optarg;
            break;
          case 'u':
            used_letters += options.optarg;
            break;
          case 'm':
            if (!parse_count(options.optarg, "--min_word_length",
                             &out->min_word_length))
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

    char const* letters = optparse_arg(&options);
    if (letters == NULL || optparse_arg(&options) != NULL) return false;

    std::string bag;
    std::string remove;
    if (!clean_letters(letters, "letters", &bag) ||
        !clean_letters(used_letters.c_str(), "used letters", &remove) ||
        !subtract_letters(bag, remove, &out->letters))
        return false;
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
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

    std::string default_dictionary;
    if (args.dictionary_file == NULL) {
        char const* const root = require_workflow_root("pairs");
        if (root == NULL) return 1;
        default_dictionary =
            (std::filesystem::path(root) / WORKFLOW_DICT_PATH).string();
        args.dictionary_file = default_dictionary.c_str();
    }

    setvbuf(stdout, nullptr, _IOFBF, 1 << 22);

    Counts const bag = letter_counts(args.letters.c_str());

    // Read and filter wordlist
    FILE* wf = fopen(args.dictionary_file, "r");
    if (!wf) { perror(args.dictionary_file); return 1; }

    struct Word {
        std::string text;
        size_t ordinal;
    };

    struct Group {
        std::vector<Word> words;
    };

    std::vector<Counts> keys;
    std::vector<Group> groups;
    std::unordered_map<Counts, size_t, CountsHash> group_indices;
    char line[4096];
    size_t ordinal = 0;
    while (fgets(line, sizeof(line), wf)) {
        size_t const this_ordinal = ordinal++;
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if ((int)len < args.min_word_length) continue;
        Counts const counts = letter_counts(line);
        Counts remaining;
        if (!subtract(bag, counts, &remaining)) continue;

        auto const inserted = group_indices.emplace(counts, groups.size());
        if (inserted.second) {
            keys.push_back(counts);
            groups.push_back({});
        }
        std::vector<Word>& words = groups[inserted.first->second].words;
        bool duplicate = false;
        for (Word const& word : words) duplicate |= word.text == line;
        if (duplicate) continue;
        words.push_back({line, this_ordinal});
    }
    fclose(wf);

    uint64_t total = 0;
    Counts remaining;
    std::vector<size_t> matching_groups(groups.size());

    for (size_t a = 0; a < groups.size(); ++a) {
        Group const& first_group = groups[a];
        subtract(bag, keys[a], &remaining);

        if (first_group.words.size() >= 2 && fits(remaining, keys[a])) {
            size_t const size = first_group.words.size();
            total += static_cast<uint64_t>(size) * (size - 1) / 2;
            for (size_t i = 0; i < size; ++i) {
                for (size_t j = i + 1; j < size; ++j) {
                    printf("%s,%s\n", first_group.words[i].text.c_str(),
                           first_group.words[j].text.c_str());
                }
            }
        }

        size_t matching_count = 0;
        for (size_t b = a + 1; b < groups.size(); ++b) {
            matching_groups[matching_count] = b;
            matching_count += fits(remaining, keys[b]);
        }

        for (size_t match = 0; match < matching_count; ++match) {
            Group const& second_group = groups[matching_groups[match]];
            total += static_cast<uint64_t>(first_group.words.size()) *
                second_group.words.size();
            for (Word const& first : first_group.words) {
                for (Word const& second : second_group.words) {
                    Word const* left = &first;
                    Word const* right = &second;
                    if (right->ordinal < left->ordinal) {
                        left = &second;
                        right = &first;
                    }
                    printf("%s,%s\n", left->text.c_str(),
                           right->text.c_str());
                }
            }
        }
    }

    return 0;
}
