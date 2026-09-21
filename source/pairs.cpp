// pairs.cpp - Find dictionary word pairs that fit within a letter bag.

#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "optparse.h"
#include "workflow-paths.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

static constexpr int ALPHA = 26;

using Counts = uint8_t[ALPHA];

static void letter_counts(const char* s, Counts out) {
    memset(out, 0, ALPHA);
    for (; *s; ++s) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c >= 'a' && c <= 'z') out[c - 'a']++;
    }
}

static bool subtract(const Counts pool, const Counts wc, Counts out) {
    for (int i = 0; i < ALPHA; ++i) {
        if (wc[i] > pool[i]) return false;
        out[i] = pool[i] - wc[i];
    }
    return true;
}

static bool fits(const Counts pool, const Counts wc) {
    for (int i = 0; i < ALPHA; ++i) {
        if (wc[i] > pool[i]) return false;
    }
    return true;
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
        char const* const root = workflow_root_from_env();
        if (root == NULL) {
            fputs("pairs: WFROOT must be set and nonempty unless -d is given\n",
                  stderr);
            return 1;
        }
        default_dictionary =
            (std::filesystem::path(root) / WORKFLOW_DICT_PATH).string();
        args.dictionary_file = default_dictionary.c_str();
    }

    setvbuf(stdout, nullptr, _IOFBF, 1 << 22);

    Counts sc;
    letter_counts(args.letters.c_str(), sc);

    // Read and filter wordlist
    FILE* wf = fopen(args.dictionary_file, "r");
    if (!wf) { perror(args.dictionary_file); return 1; }

    struct Word {
        std::string text;
        Counts wc;
    };

    std::vector<Word> words;
    char line[4096];
    while (fgets(line, sizeof(line), wf)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if ((int)len < args.min_word_length) continue;
        Counts wc;
        letter_counts(line, wc);
        Counts remaining;
        if (subtract(sc, wc, remaining)) {
            words.push_back({std::string(line), {}});
            memcpy(words.back().wc, wc, ALPHA);
        }
    }
    fclose(wf);

    const size_t n = words.size();
    long long total = 0;
    Counts remaining;

    for (size_t i = 0; i < n; ++i) {
        const Word& w1 = words[i];
        if (!subtract(sc, w1.wc, remaining)) continue;
        for (size_t j = i + 1; j < n; ++j) {
            const Word& w2 = words[j];
            if (fits(remaining, w2.wc)) {
                printf("%s,%s\n", w1.text.c_str(), w2.text.c_str());
                ++total;
            }
        }
    }

    printf("%lld\n", total);
    return 0;
}
