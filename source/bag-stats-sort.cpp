// bag-stats-sort.cpp - Sort bag-stats rows by one count column, largest
// first.

#include "bad-bag-bitmap.h"
#include "bag-stats-impl.h"
#include "dfs-cli-help.h"
#include "optparse.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr char PROGRAM[] = "bag-stats-sort";

struct SortFlag {
    char const* option;
    char const* column;
};

SortFlag const SORT_FLAGS[] = {
    {"wb", "WORDBAGS"},
    {"wbwp", "WB_WITH_PAIRS"},
    {"tp", "TOTAL_PAIRS"},
    {"mp", "MAX_PAIRS"},
};

constexpr int SORT_FLAG_COUNT = sizeof(SORT_FLAGS) / sizeof(SORT_FLAGS[0]);
constexpr int OPT_SORT = 256;

struct Args {
    BagStatsCount const* key = NULL;
    char const* input_file = "-";
};

void usage(char const* program, FILE* out) {
    fprintf(out, "usage: %s (--wb | --wbwp | --tp | --mp) [FILE|-]\n",
            program);
    if (out != stdout) return;

    fputs("  print bag-stats output from FILE, standard input when it is '-'\n"
          "  or not given, with its rows sorted by one count column, largest\n"
          "  first; equal rows keep their order\n"
          "\noptions:\n", stdout);
    for (SortFlag const& flag : SORT_FLAGS) {
        std::string const option = std::string("--") + flag.option;
        dfs_help_option(option.c_str(), "sort by %s", flag.column);
    }
    dfs_help_option("-h, --help", "show this help");
}

bool parse_args(char* argv[], Args* out, bool* help) {
    static struct optparse_long const long_options[] = {
        { SORT_FLAGS[0].option, OPT_SORT + 0, OPTPARSE_NONE },
        { SORT_FLAGS[1].option, OPT_SORT + 1, OPTPARSE_NONE },
        { SORT_FLAGS[2].option, OPT_SORT + 2, OPTPARSE_NONE },
        { SORT_FLAGS[3].option, OPT_SORT + 3, OPTPARSE_NONE },
        { "help", 'h', OPTPARSE_NONE },
        { NULL, 0, OPTPARSE_NONE },
    };
    static_assert(SORT_FLAG_COUNT == 4);

    struct optparse options;
    optparse_init(&options, argv);

    int option;
    while ((option = optparse_long(&options, long_options, NULL)) != -1) {
        if (option >= OPT_SORT && option < OPT_SORT + SORT_FLAG_COUNT) {
            if (out->key != NULL) {
                fprintf(stderr, "%s: give one sort option\n", PROGRAM);
                return false;
            }
            out->key =
                find_bag_stats_count(SORT_FLAGS[option - OPT_SORT].column);
            continue;
        }
        switch (option) {
          case 'h':
            *help = true;
            return true;
          default:
            fprintf(stderr, "error: %s\n", options.errmsg);
            return false;
        }
    }

    if (out->key == NULL) {
        fprintf(stderr, "%s: give a sort option\n", PROGRAM);
        return false;
    }
    char const* const input = optparse_arg(&options);
    if (input != NULL) out->input_file = input;
    return optparse_arg(&options) == NULL;
}

bool read_rows(FILE* in, std::string const& path,
               std::vector<BagStatsRow>* out) {
    std::string line;
    size_t line_number = 0;
    while (read_line(in, &line)) {
        auto row = parse_bag_stats_row(line, path, ++line_number);
        if (!row) return false;
        out->push_back(std::move(*row));
    }
    if (ferror(in)) {
        perror(path.c_str());
        return false;
    }
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

    bool const from_stdin = strcmp(args.input_file, "-") == 0;
    std::string const path = from_stdin ? "<stdin>" : args.input_file;
    FILE* in = from_stdin ? stdin : fopen(args.input_file, "r");
    if (!in) { perror(args.input_file); return 1; }

    std::vector<BagStatsRow> rows;
    auto const header = read_bag_stats_header(in, path);
    bool const read = header && read_rows(in, path, &rows);
    if (!from_stdin) fclose(in);
    if (!read) return 1;

    uint64_t BagStatsRow::*const value = args.key->value;
    std::stable_sort(rows.begin(), rows.end(),
        [value](BagStatsRow const& a, BagStatsRow const& b) {
            return a.*value > b.*value;
        });

    setvbuf(stdout, nullptr, _IOFBF, 1 << 22);
    write_bag_stats_header(stdout, *header);
    for (BagStatsRow const& row : rows) write_bag_stats_row(stdout, row);
    return 0;
}
