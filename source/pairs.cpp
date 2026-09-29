// pairs.cpp - Find dictionary word pairs that fit within a letter bag.

#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "optparse.h"
#include "pairs-impl.h"
#include "workflow-paths.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

constexpr int OPT_ALLOW_SOLO = 256;

struct Args {
    char const* dictionary_file = NULL;
    PairsOptions options;
};

void usage(char const* program, FILE* out) {
    fprintf(out,
            "usage: %s [-d FILE] [-u LETTERS] [-m N] [-x [--allow-solo]] "
            "LETTERS\n", program);
    if (out != stdout) return;

    fputs("  print word,word pairs of distinct dictionary words that fit\n"
          "  together within LETTERS\n"
          "\noptions:\n", stdout);
    dfs_help_option("-d, --dict FILE",
        "read words from FILE (default: $WFROOT/%s)",
        WORKFLOW_DICT_PATH);
    dfs_help_used_letters();
    dfs_help_option("-m, --min_word_length N",
        "minimum word length (default: %d; 0 for no minimum)",
        PAIRS_DEFAULT_MIN_WORD_LENGTH);
    dfs_help_option("-x, --exact",
        "only print pairs that use all of LETTERS");
    dfs_help_option("--allow-solo",
        "with -x, also print single words that use all of LETTERS");
    dfs_help_option("-h, --help", "show this help");
}

bool parse_args(char* argv[], Args* out, bool* help) {
    static struct optparse_long const long_options[] = {
        { "dict", 'd', OPTPARSE_REQUIRED },
        { "used-letters", 'u', OPTPARSE_REQUIRED },
        { "min_word_length", 'm', OPTPARSE_REQUIRED },
        { "exact", 'x', OPTPARSE_NONE },
        { "allow-solo", OPT_ALLOW_SOLO, OPTPARSE_NONE },
        { "help", 'h', OPTPARSE_NONE },
        { NULL, 0, OPTPARSE_NONE },
    };

    struct optparse options;
    optparse_init(&options, argv);

    int option;
    while ((option = optparse_long(&options, long_options, NULL)) != -1) {
        switch (option) {
          case 'd':
            out->dictionary_file = options.optarg;
            break;
          case 'u':
            out->options.used_letters += options.optarg;
            break;
          case 'm':
            if (!parse_count(options.optarg, "--min_word_length",
                             &out->options.min_word_length))
                return false;
            break;
          case 'x':
            out->options.exact = true;
            break;
          case OPT_ALLOW_SOLO:
            out->options.allow_solo = true;
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
    if (out->options.allow_solo && !out->options.exact) {
        fputs("pairs: --allow-solo requires --exact\n", stderr);
        return false;
    }
    out->options.letters = letters;
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    Args args;
    bool help = false;
    Pairs pairs;
    if (!parse_args(argv, &args, &help) ||
        (!help && !pairs.load(args.options))) {
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

    FILE* wf = fopen(args.dictionary_file, "r");
    if (!wf) { perror(args.dictionary_file); return 1; }

    char line[4096];
    std::string word;
    while (fgets(line, sizeof(line), wf)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        word.assign(line, len);
        pairs.add(word);
    }
    fclose(wf);

    pairs.write(stdout);
    return 0;
}
