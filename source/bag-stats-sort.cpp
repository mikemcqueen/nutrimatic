// bag-stats-sort.cpp - Sort bag-stats rows by count columns or the MIN_WB
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

constexpr int SORT_FLAG_COUNT =
    sizeof(BAG_STATS_SORT_FLAGS) / sizeof(BAG_STATS_SORT_FLAGS[0]);
constexpr int OPT_SORT = 256;

constexpr int OPT_BY_LENGTH = OPT_SORT + SORT_FLAG_COUNT;
constexpr int OPT_PAIR_BAG_LENGTH = OPT_BY_LENGTH + 1;

struct Args {
    // One per sort option, NULL for the MIN_WB fraction.
    std::vector<BagStatsCount const*> keys;
    bool by_length = false;
    int pair_bag_length = -1;
    char const* input_file = "-";
};

void usage(char const* program, FILE* out) {
    fprintf(out,
            "usage: %s (--wb | --wbwp | --tp | --mp | --min-wb | --min-wb-frac)...\n"
            "       [--by-length] [--pb-len N] [FILE|-]\n",
            program);
    if (out != stdout) return;

    fputs("  print bag-stats output from FILE, standard input when it is '-'\n"
          "  or not given, with its rows sorted by one count column or the\n"
          "  MIN_WB fraction, largest first; equal rows keep their order\n"
          "\n"
          "  with more than one sort option, each row's value under each\n"
          "  option becomes its percentile among all rows, and rows sort by\n"
          "  their best percentile, then by the sum of them: the sorted lists\n"
          "  merged, each row at its better place\n"
          "\noptions:\n", stdout);
    bag_stats_help_sort_flags();
    dfs_help_option("--by-length",
                    "compare each row's sort value only with the rows whose "
                    "PAIR has the same number of letters, and sort by its "
                    "percentile there, so the best of each length comes "
                    "first; no effect with --pb-len");
    dfs_help_option("--pb-len N",
                    "print only the rows whose PAIR has N letters");
    dfs_help_option("-h, --help", "show this help");
}

bool parse_args(char* argv[], Args* out, bool* help) {
    static struct optparse_long const long_options[] = {
        { BAG_STATS_SORT_FLAGS[0].option, OPT_SORT + 0, OPTPARSE_NONE },
        { BAG_STATS_SORT_FLAGS[1].option, OPT_SORT + 1, OPTPARSE_NONE },
        { BAG_STATS_SORT_FLAGS[2].option, OPT_SORT + 2, OPTPARSE_NONE },
        { BAG_STATS_SORT_FLAGS[3].option, OPT_SORT + 3, OPTPARSE_NONE },
        { BAG_STATS_SORT_FLAGS[4].option, OPT_SORT + 4, OPTPARSE_NONE },
        { BAG_STATS_SORT_FLAGS[5].option, OPT_SORT + 5, OPTPARSE_NONE },
        { "by-length", OPT_BY_LENGTH, OPTPARSE_NONE },
        { "pb-len", OPT_PAIR_BAG_LENGTH, OPTPARSE_REQUIRED },
        { "help", 'h', OPTPARSE_NONE },
        { NULL, 0, OPTPARSE_NONE },
    };
    static_assert(SORT_FLAG_COUNT == 6);

    struct optparse options;
    optparse_init(&options, argv);

    int option;
    while ((option = optparse_long(&options, long_options, NULL)) != -1) {
        if (option >= OPT_SORT && option < OPT_SORT + SORT_FLAG_COUNT) {
            out->keys.push_back(
                bag_stats_sort_key(BAG_STATS_SORT_FLAGS[option - OPT_SORT]));
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

    if (out->keys.empty()) {
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

// The mid-rank percentile of `value` among the sorted `group`.
double percentile(std::vector<double> const& group, double value) {
    auto const below =
        std::lower_bound(group.begin(), group.end(), value) - group.begin();
    auto const through =
        std::upper_bound(group.begin(), group.end(), value) - group.begin();
    return bag_stats_percentile(uint64_t(below), uint64_t(through),
                                group.size());
}

// Each of `values`, the sort value of the row at the same index of `rows`, as
// a mid-rank percentile among the values of the rows whose PAIR has the
// same length.
std::vector<double> length_ranks(std::vector<BagStatsRow> const& rows,
                                 std::vector<double> const& values) {
    std::unordered_map<size_t, std::vector<double>> by_length;
    for (size_t i = 0; i < rows.size(); ++i)
        by_length[bag_stats_pair_length(rows[i].pair)].push_back(values[i]);
    for (auto& entry : by_length)
        std::sort(entry.second.begin(), entry.second.end());

    std::vector<double> out;
    out.reserve(rows.size());
    for (size_t i = 0; i < rows.size(); ++i)
        out.push_back(percentile(
            by_length[bag_stats_pair_length(rows[i].pair)], values[i]));
    return out;
}

// Each of `values` as a mid-rank percentile among all of them.
std::vector<double> ranks(std::vector<double> const& values) {
    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    std::vector<double> out;
    out.reserve(values.size());
    for (double value : values) out.push_back(percentile(sorted, value));
    return out;
}

// One key per row: the sort value, its percentile within its length with
// `by_length`, and with more than one sort key, the row's best percentile
// among all rows, the sum of them breaking ties.
std::vector<std::pair<double, double>> sort_keys(
        Args const& args, std::vector<BagStatsRow> const& rows) {
    std::vector<std::pair<double, double>> out(rows.size(), {-1, 0});
    for (BagStatsCount const* key : args.keys) {
        std::vector<double> values;
        values.reserve(rows.size());
        for (BagStatsRow const& row : rows)
            values.push_back(bag_stats_sort_value(key, row));
        if (args.by_length) values = length_ranks(rows, values);
        if (args.keys.size() > 1) values = ranks(values);
        for (size_t i = 0; i < rows.size(); ++i) {
            out[i].first = std::max(out[i].first, values[i]);
            out[i].second += values[i];
        }
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
                return bag_stats_pair_length(row.pair) !=
                    size_t(args.pair_bag_length);
            }), rows.end());

    auto const keys = sort_keys(args, rows);

    std::vector<size_t> order(rows.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
        [&keys](size_t a, size_t b) { return keys[a] > keys[b]; });

    setvbuf(stdout, nullptr, _IOFBF, 1 << 22);
    write_bag_stats_header(stdout, *header);
    for (size_t i : order) write_bag_stats_row(stdout, rows[i]);
    return 0;
}
