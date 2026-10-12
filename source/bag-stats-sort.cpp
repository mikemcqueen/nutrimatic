// bag-stats-sort.cpp - Sort bag-stats rows by one count column or the MIN_WB
// fraction, largest first.

#include "bad-bag-bitmap.h"
#include "bag-stats-impl.h"
#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "optparse.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
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
    {"min-wb", "MIN_WB"},
};

constexpr int SORT_FLAG_COUNT = sizeof(SORT_FLAGS) / sizeof(SORT_FLAGS[0]);
constexpr int OPT_SORT = 256;

constexpr int OPT_MIN_WB_FRAC = OPT_SORT + SORT_FLAG_COUNT;
constexpr int OPT_BY_LENGTH = OPT_MIN_WB_FRAC + 1;
constexpr int OPT_PAIR_BAG_LENGTH = OPT_MIN_WB_FRAC + 2;

struct Args {
    BagStatsCount const* key = NULL;
    bool min_wb_frac = false;
    bool by_length = false;
    int pair_bag_length = -1;
    char const* input_file = "-";
};

void usage(char const* program, FILE* out) {
    fprintf(out,
            "usage: %s (--wb | --wbwp | --tp | --mp | --min-wb | --min-wb-frac)\n"
            "       [--by-length] [--pb-len N] [FILE|-]\n",
            program);
    if (out != stdout) return;

    fputs("  print bag-stats output from FILE, standard input when it is '-'\n"
          "  or not given, with its rows sorted by one count column or the\n"
          "  MIN_WB fraction, largest first; equal rows keep their order\n"
          "\noptions:\n", stdout);
    for (SortFlag const& flag : SORT_FLAGS) {
        std::string const option = std::string("--") + flag.option;
        dfs_help_option(option.c_str(), "sort by %s", flag.column);
    }
    dfs_help_option("--min-wb-frac",
                    "sort by MIN_WB_WITH_PAIRS / MIN_WB, the fraction of "
                    "MIN_WB word bags whose remainder has enough exact pairs "
                    "(0 when MIN_WB is 0)");
    dfs_help_option("--by-length",
                    "compare each row's sort value only with the rows whose "
                    "PAIRBAG has the same number of letters, and sort by its "
                    "percentile there, so the best of each length comes "
                    "first; no effect with --pb-len");
    dfs_help_option("--pb-len N",
                    "print only the rows whose PAIRBAG has N letters");
    dfs_help_option("-h, --help", "show this help");
}

bool parse_args(char* argv[], Args* out, bool* help) {
    static struct optparse_long const long_options[] = {
        { SORT_FLAGS[0].option, OPT_SORT + 0, OPTPARSE_NONE },
        { SORT_FLAGS[1].option, OPT_SORT + 1, OPTPARSE_NONE },
        { SORT_FLAGS[2].option, OPT_SORT + 2, OPTPARSE_NONE },
        { SORT_FLAGS[3].option, OPT_SORT + 3, OPTPARSE_NONE },
        { SORT_FLAGS[4].option, OPT_SORT + 4, OPTPARSE_NONE },
        { "min-wb-frac", OPT_MIN_WB_FRAC, OPTPARSE_NONE },
        { "by-length", OPT_BY_LENGTH, OPTPARSE_NONE },
        { "pb-len", OPT_PAIR_BAG_LENGTH, OPTPARSE_REQUIRED },
        { "help", 'h', OPTPARSE_NONE },
        { NULL, 0, OPTPARSE_NONE },
    };
    static_assert(SORT_FLAG_COUNT == 5);

    struct optparse options;
    optparse_init(&options, argv);

    int option;
    while ((option = optparse_long(&options, long_options, NULL)) != -1) {
        bool const is_sort = option == OPT_MIN_WB_FRAC ||
            (option >= OPT_SORT && option < OPT_SORT + SORT_FLAG_COUNT);
        if (is_sort && (out->key != NULL || out->min_wb_frac)) {
            fprintf(stderr, "%s: give one sort option\n", PROGRAM);
            return false;
        }
        if (option == OPT_MIN_WB_FRAC) {
            out->min_wb_frac = true;
            continue;
        }
        if (option >= OPT_SORT && option < OPT_SORT + SORT_FLAG_COUNT) {
            out->key =
                find_bag_stats_count(SORT_FLAGS[option - OPT_SORT].column);
            continue;
        }
        switch (option) {
          case OPT_BY_LENGTH:
            out->by_length = true;
            break;
          case OPT_PAIR_BAG_LENGTH:
            if (!parse_count(options.optarg, "--pb-len",
                             &out->pair_bag_length))
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

    if (out->key == NULL && !out->min_wb_frac) {
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

double min_wb_fraction(BagStatsRow const& row) {
    return row.min_word_bags == 0
        ? 0 : double(row.min_with_pairs) / double(row.min_word_bags);
}

// Each of `values`, the sort value of the row at the same index of `rows`, as
// a mid-rank percentile among the values of the rows whose PAIRBAG has the
// same length.
std::vector<double> length_ranks(std::vector<BagStatsRow> const& rows,
                                 std::vector<double> const& values) {
    std::unordered_map<size_t, std::vector<double>> by_length;
    for (size_t i = 0; i < rows.size(); ++i)
        by_length[rows[i].pair_bag.size()].push_back(values[i]);
    for (auto& entry : by_length)
        std::sort(entry.second.begin(), entry.second.end());

    std::vector<double> out;
    out.reserve(rows.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        std::vector<double> const& group = by_length[rows[i].pair_bag.size()];
        double const value = values[i];
        auto const below =
            std::lower_bound(group.begin(), group.end(), value) - group.begin();
        auto const through =
            std::upper_bound(group.begin(), group.end(), value) - group.begin();
        out.push_back(double(below + through) / 2 / double(group.size()));
    }
    return out;
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

    if (args.pair_bag_length >= 0)
        rows.erase(std::remove_if(rows.begin(), rows.end(),
            [&args](BagStatsRow const& row) {
                return row.pair_bag.size() != size_t(args.pair_bag_length);
            }), rows.end());

    std::vector<double> keys;
    keys.reserve(rows.size());
    for (BagStatsRow const& row : rows)
        keys.push_back(args.min_wb_frac ? min_wb_fraction(row)
                                        : double(row.*args.key->value));
    if (args.by_length) keys = length_ranks(rows, keys);

    std::vector<size_t> order(rows.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
        [&keys](size_t a, size_t b) { return keys[a] > keys[b]; });

    setvbuf(stdout, nullptr, _IOFBF, 1 << 22);
    write_bag_stats_header(stdout, *header);
    for (size_t i : order) write_bag_stats_row(stdout, rows[i]);
    return 0;
}
