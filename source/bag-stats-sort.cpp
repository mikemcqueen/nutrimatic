// bag-stats-sort.cpp - Rank a bag-stats dump's rows as bag-stats ranks the
// rows of its walk.

#include "bad-bag-bitmap.h"
#include "bag-stats-impl.h"
#include "bag-stats-rank.h"
#include "classified.h"
#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "log.h"
#include "optparse.h"
#include "pairs-impl.h"
#include "workflow-paths.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr char PROGRAM[] = "bag-stats-sort";

constexpr int OPT_PAIR_BAG_LENGTH = 256;
constexpr int OPT_SEARCH = 257;
constexpr int OPT_PROSPECTS = 258;

struct Args {
    int sentence = CLASSIFIED_NO_SENTENCE;
    RankArgs rank;
    int pair_bag_length = -1;
    // --ss and --pp, when given.
    std::optional<std::string> search;
    std::optional<std::filesystem::path> prospects_file;
    std::filesystem::path input_file = "-";
};

void usage(std::string_view program, FILE* out) {
    fprintf(out,
            "usage: %.*s [-s N] [-n N] SORT... [--by-length | --by-pick]\n"
            "       [--score W1,W2... [--score-detail]]\n"
            "       [--best-pairs appear|lead]\n"
            "       [--prior FILE | --no-prior |\n"
            "        [--p1 INDEX] [--clues CLUEDATA] [--prior-weights SPEC]]\n"
            "       [--top-segments] [--pb-len N] [--ss STEPS] [--pp FILE]\n"
            "       [FILE|-]\n",
            SV_ARG(program));
    if (out != stdout) return;

    fputs("  rank the rows of the bag-stats dump FILE, standard input when it\n"
          "  is '-' or not given, as bag-stats ranks its rows with the same\n"
          "  options, among the dump's rows only, and print them sorted;\n"
          "  equal rows keep their order\n"
          "\n"
          "  the pick bags are rebuilt from the PAIRS and --pp files the\n"
          "  header names, or --pp's, less the classified NO pairs of the dump's\n"
          "  sentence; a row whose pick has no pairs left is dropped, and\n"
          "  the others' PAIR and PAIRS are rebuilt from their bags\n"
          "\n"
          "  with more than one sort option, each row's value under each\n"
          "  option becomes its percentile among all rows, and rows sort by\n"
          "  their best percentile, then by the sum of them: the sorted lists\n"
          "  merged, each row at its better place\n"
          "\noptions:\n", stdout);
    dfs_help_option("-s, --sentence N",
                    "drop pairs listed in $WFROOT/%s/sN/no/no.pairs, in "
                    "place of the dump's sentence's",
                    WORKFLOW_CLASSIFIED_PATH);
    dfs_help_option("-n N", "print only the N rows that rank first");
    help_rank_output_options();
    dfs_help_option("--pb-len N",
                    "rank and print only the rows whose PAIR has N letters");
    dfs_help_option("--ss STEPS",
                    "the search in place of the dump's, with as many ppb and "
                    "pb steps together and as many wb steps; each pick is a "
                    "ppb pick when its bag is a --pp bag, and a row whose "
                    "picks aren't the search's is dropped; with no ppb "
                    "step and no --pp, the dump's --pp isn't used");
    dfs_help_option("--pp FILE",
                    "the prospective pairs the ppb picks are from, in place "
                    "of the dump's");
    dfs_help_option("-h, --help", "show this help");
    fputs("\nSORT options, at least one:\n", stdout);
    help_rank_sort_options();
}

bool parse_args(char* argv[], Args* out, bool* help) {
    static struct optparse_long const long_options[] = {
        CLASSIFIED_SENTENCE_LONG_OPTION,
        BAG_STATS_RANK_LONG_OPTIONS,
        { "pb-len", OPT_PAIR_BAG_LENGTH, OPTPARSE_REQUIRED },
        { "ss", OPT_SEARCH, OPTPARSE_REQUIRED },
        { "pp", OPT_PROSPECTS, OPTPARSE_REQUIRED },
        { "help", 'h', OPTPARSE_NONE },
        { NULL, 0, OPTPARSE_NONE },
    };

    struct optparse options;
    optparse_init(&options, argv);

    int option;
    while ((option = optparse_long(&options, long_options, NULL)) != -1) {
        RankOption const rank =
            parse_rank_option(PROGRAM, option, options.optarg, &out->rank);
        if (rank == RankOption::BAD) return false;
        if (rank == RankOption::PARSED) continue;
        switch (option) {
          case 's':
            if (!parse_classified_sentence(options.optarg, &out->sentence))
                return false;
            break;
          case OPT_PAIR_BAG_LENGTH:
            if (!parse_count(options.optarg, "--pb-len",
                             &out->pair_bag_length))
                return false;
            break;
          case OPT_SEARCH:
            out->search = options.optarg;
            break;
          case OPT_PROSPECTS:
            out->prospects_file = options.optarg;
            break;
          case 'h':
            *help = true;
            return true;
          default:
            fprintf(stderr, "error: %s\n", options.errmsg);
            return false;
        }
    }

    if (out->rank.sorts.empty()) {
        report(PROGRAM, "give a sort option");
        return false;
    }
    char const* const input = optparse_arg(&options);
    if (input != NULL) out->input_file = input;
    if ((out->input_file == "-") + (out->rank.prior_file == "-") +
        (out->prospects_file == "-") > 1) {
        report(PROGRAM, "only one of FILE, --prior and --pp can be -");
        return false;
    }
    return optparse_arg(&options) == NULL;
}

// The dump's rows with the bags their picks are rebuilt from.
struct Dump {
    BagStatsHeader header;
    Search search;
    std::vector<PairBag> pair_bags;
    std::vector<PairBag> prospect_bags;
    // Whether the bags have priors.
    bool has_prior = false;
    // Each step's list, pb steps first, as bag-stats walks them.
    std::vector<std::vector<PairBag> const*> steps;
    // Each row with `unit` 0 and `seq` its place among the rows.
    std::vector<RankedRow> rows;
    // Each row's picks, steps.size() of them, as RowWalker::picks() numbers
    // them.
    std::vector<uint32_t> picks;

    std::span<uint32_t const> row_picks(size_t seq) const {
        return {picks.data() + seq * steps.size(), steps.size()};
    }
};

enum class Refresh { KEPT, NO_BAG, NOT_THE_SEARCH };

// Rebuilds `row`'s PAIR, PAIRS and picks, appended to `picks`, from the bags
// its PAIR's names have the letters of, each a ppb pick when its bag is a
// --pp bag and a pb pick otherwise.
Refresh refresh_row(Dump const& dump,
                    std::unordered_map<PairsCounts, uint32_t,
                                       PairsCountsHash> const* indices,
                    std::vector<std::string> const& names, BagStatsRow* row,
                    std::vector<uint32_t>* picks) {
    std::vector<uint32_t> chosen[2];
    for (std::string const& name : names) {
        PairsCounts const counts = pairs_letter_counts(name);
        bool found = false;
        for (size_t list : {1, 0}) {
            auto const index = indices[list].find(counts);
            if (index == indices[list].end()) continue;
            chosen[list].push_back(index->second);
            found = true;
            break;
        }
        if (!found) return Refresh::NO_BAG;
    }
    if (chosen[1].size() != size_t(dump.search.prospects))
        return Refresh::NOT_THE_SEARCH;
    std::sort(chosen[0].begin(), chosen[0].end());
    std::sort(chosen[1].begin(), chosen[1].end());
    row->pair.clear();
    row->pairs = 1;
    for (size_t list : {1, 0}) {
        std::vector<PairBag> const& bags =
            list == 0 ? dump.pair_bags : dump.prospect_bags;
        for (uint32_t pick : chosen[list]) {
            if (!row->pair.empty()) row->pair += " + ";
            row->pair += bags[pick].pair;
            row->pairs *= bags[pick].pairs;
        }
    }
    for (uint32_t pick : chosen[0]) picks->push_back(pick);
    for (uint32_t pick : chosen[1])
        picks->push_back(pick + uint32_t(dump.pair_bags.size()));
    return Refresh::KEPT;
}

// Reads the rows after the header from `in`, the file at `path`, refreshing
// each and keeping those of --pb-len's length.
bool read_rows(FILE* in, std::string const& path, Args const& args,
               Dump* dump) {
    std::unordered_map<PairsCounts, uint32_t, PairsCountsHash> indices[2];
    for (size_t list = 0; list < 2; ++list) {
        std::vector<PairBag> const& bags =
            list == 0 ? dump->pair_bags : dump->prospect_bags;
        for (size_t i = 0; i < bags.size(); ++i)
            indices[list].emplace(bags[i].counts, uint32_t(i));
    }

    std::string line;
    std::vector<std::string> names;
    size_t line_number = 0;
    uint64_t dropped = 0;
    uint64_t not_the_search = 0;
    while (read_line(in, &line)) {
        auto row = parse_bag_stats_row(line, path, ++line_number);
        if (!row) return false;
        names.clear();
        for (size_t start = 0;;) {
            size_t const plus = row->pair.find(" + ", start);
            names.push_back(row->pair.substr(start, plus - start));
            if (plus == std::string::npos) break;
            start = plus + 3;
        }
        if (names.size() != dump->steps.size()) {
            fprintf(stderr, "%s line %zu: PAIR doesn't have %zu picks\n",
                    path.c_str(), line_number, dump->steps.size());
            return false;
        }
        size_t const kept = dump->picks.size();
        Refresh const refreshed =
            refresh_row(*dump, indices, names, &*row, &dump->picks);
        if (refreshed == Refresh::NO_BAG) ++dropped;
        if (refreshed == Refresh::NOT_THE_SEARCH) ++not_the_search;
        if (refreshed != Refresh::KEPT) continue;
        if (args.pair_bag_length >= 0 &&
            bag_stats_pair_length(row->pair) != size_t(args.pair_bag_length)) {
            dump->picks.resize(kept);
            continue;
        }
        dump->rows.push_back({{}, 0, dump->rows.size(), std::move(*row), {}});
    }
    if (ferror(in)) {
        perror(path.c_str());
        return false;
    }
    if (dropped > 0)
        report(PROGRAM, "dropped %llu of %zu rows whose picks have no "
               "pairs left", (unsigned long long)dropped, line_number);
    if (not_the_search > 0)
        report(PROGRAM, "dropped %llu of %zu rows whose picks aren't "
               "the search's ppb and pb picks",
               (unsigned long long)not_the_search, line_number);
    return true;
}

// Reads the dump from `args`' FILE and rebuilds its pick bags.
bool load_dump(Args const& args, Dump* dump) {
    bool const from_stdin = args.input_file == "-";
    std::string const path = from_stdin ? "<stdin>" : args.input_file.string();
    FILE* in = from_stdin ? stdin : fopen(args.input_file.c_str(), "r");
    if (!in) { perror(args.input_file.c_str()); return false; }

    bool const loaded = [&] {
        auto header = read_bag_stats_header(in, path);
        if (!header) return false;
        dump->header = std::move(*header);
        BagStatsHeader& h = dump->header;
        if (!parse_search(PROGRAM, h.search, &dump->search))
            return false;
        if ((dump->search.prospects > 0) != !h.prospects.empty()) {
            fprintf(stderr, "%s: malformed bag-stats header\n", path.c_str());
            return false;
        }
        if (dump->search.prospects + dump->search.pairs == 0) {
            report(PROGRAM, "the dump's search has no ppb or pb step, "
                   "so no rows to rank");
            return false;
        }
        if (args.search) {
            Search search;
            if (!parse_search(PROGRAM, *args.search, &search)) return false;
            if (search.prospects + search.pairs !=
                    dump->search.prospects + dump->search.pairs ||
                search.words != dump->search.words ||
                search.pair_filter != dump->search.pair_filter) {
                report(PROGRAM, "--ss has to have the dump's %d ppb and "
                       "pb steps, %d wb steps and last step %s",
                       dump->search.prospects + dump->search.pairs,
                       dump->search.words,
                       dump->search.pair_filter ? "pf" : "p");
                return false;
            }
            dump->search = search;
            h.search = search_text(search);
            if (search.prospects == 0 && !args.prospects_file)
                h.prospects.clear();
        }
        if (!args.prospects_file && h.prospects == "-") {
            report(PROGRAM, "the dump's --pp was standard input; can't "
                   "rebuild its bags");
            return false;
        }
        if (args.prospects_file)
            h.prospects = bag_stats_header_path(*args.prospects_file);
        if ((dump->search.prospects > 0) != !h.prospects.empty()) {
            report(PROGRAM, "--pp goes with a ppb step in the search");
            return false;
        }
        if (!check_rank_args(PROGRAM, args.rank, dump->search)) return false;
        bool const pick_pairs = dump->search.pairs > 0;
        if (pick_pairs && h.pairs == "-") {
            report(PROGRAM, "the dump's PAIRS was standard input; can't "
                   "rebuild its bags");
            return false;
        }
        if (args.sentence != CLASSIFIED_NO_SENTENCE) h.sentence = args.sentence;

        std::filesystem::path const root =
            require_workflow_root(PROGRAM);
        if (root.empty()) return false;
        DfsPairSet rejected;
        PairsCounts const bag = pairs_letter_counts(h.letters);
        std::optional<std::filesystem::path> prospects;
        if (!h.prospects.empty()) prospects = h.prospects;
        if (!load_no_pairs(PROGRAM, root, h.sentence, &rejected) ||
            !load_pick_bags(PROGRAM, args.rank,
                            prospects, h.pairs, bag, rejected,
                            h.min_pair_bag_length, &dump->prospect_bags,
                            pick_pairs ? &dump->pair_bags : nullptr,
                            nullptr, nullptr,
                            &dump->has_prior))
            return false;
        dump->steps.insert(dump->steps.end(), dump->search.pairs,
                           &dump->pair_bags);
        dump->steps.insert(dump->steps.end(), dump->search.prospects,
                           &dump->prospect_bags);
        return read_rows(in, path, args, dump);
    }();
    if (!from_stdin) fclose(in);
    return loaded;
}

// Prints the --score row and its rank among the dump's rows.
template <typename R>
int print_score(Dump const& dump, Args const& args, R const& ranker) {
    std::vector<size_t> picks;
    if (!score_picks(PROGRAM, args.rank.score, dump.search, dump.steps,
                     pairs_letter_counts(dump.header.letters),
                     &picks))
        return 1;
    std::vector<PairBag const*> bags;
    std::vector<uint32_t> target_picks;
    for (size_t j = 0; j < picks.size(); ++j) {
        bags.push_back(&(*dump.steps[j])[picks[j]]);
        target_picks.push_back(uint32_t(
            picks[j] + (j < size_t(dump.search.pairs)
                        ? 0 : dump.pair_bags.size())));
    }
    auto const found = std::find_if(dump.rows.begin(), dump.rows.end(),
        [&](RankedRow const& ranked) {
            std::span<uint32_t const> const row = dump.row_picks(ranked.seq);
            return std::equal(row.begin(), row.end(), target_picks.begin());
        });
    if (found == dump.rows.end()) {
        report(PROGRAM, "no kept row has the --score picks");
        return 1;
    }

    RankedRow target = *found;
    target.key = ranker.key(target.row, bag_stats_pair_length(target.row.pair),
                            target_picks);
    ScoreRank rank;
    for (RankedRow const& row : dump.rows) {
        RankedRow const ranked{
            ranker.key(row.row, bag_stats_pair_length(row.row.pair),
                       dump.row_picks(row.seq)),
            row.unit, row.seq, {}, {}};
        add_score_rank(ranked, target, &rank);
    }
    print_score_rank(target.row, rank);
    if constexpr (R::NEEDS_PICKS)
        if (args.rank.score_detail)
            print_score_detail(args.rank.sorts, ranker, target, bags,
                               target_picks);
    return 0;
}

// Ranks the dump's rows with `ranker` and prints them, the -n first of them,
// or the --score row.
template <typename R>
int print_ranked(Dump* dump, Args const& args, R* ranker) {
    RankArgs const& rank = args.rank;
    if (ranker->needs_counts()) {
        std::vector<typename R::Part> parts(1, ranker->part());
        for (RankedRow const& row : dump->rows)
            ranker->add(&parts[0], row.row,
                        bag_stats_pair_length(row.row.pair),
                        dump->row_picks(row.seq));
        ranker->finish(parts);
    }
    if (!rank.score.empty()) return print_score(*dump, args, *ranker);

    std::vector<RankedRow>& rows = dump->rows;
    for (RankedRow& row : rows)
        row.key = ranker->key(row.row, bag_stats_pair_length(row.row.pair),
                              dump->row_picks(row.seq));
    if (rank.top > 0 && size_t(rank.top) < rows.size()) {
        std::partial_sort(rows.begin(), rows.begin() + rank.top, rows.end(),
                          ranks_before);
        rows.erase(rows.begin() + rank.top, rows.end());
    } else {
        std::sort(rows.begin(), rows.end(), ranks_before);
    }

    if (!rank.top_segments && rank.best_pairs == BestPairs::NONE)
        write_bag_stats_header(stdout, dump->header);
    std::string line;
    std::unordered_set<uint32_t> seen;
    for (RankedRow& ranked : rows) {
        line.clear();
        std::span<uint32_t const> const picks = dump->row_picks(ranked.seq);
        if (rank.best_pairs == BestPairs::NONE) {
            append_row(&line, ranked.row, rank.top_segments);
        } else {
            if (rank.best_pairs == BestPairs::APPEAR) {
                ranked.best.assign(picks.begin(), picks.end());
            } else if constexpr (R::NEEDS_PICKS) {
                ranked.best = ranker->leaders(ranked.row, picks);
            }
            append_best_pairs(&line, &seen, ranked.best, dump->pair_bags,
                              dump->prospect_bags);
        }
        fwrite(line.data(), 1, line.size(), stdout);
    }
    return 0;
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

    Dump dump;
    if (!load_dump(args, &dump)) return 1;

    std::vector<BagStatsCount const*> const keys =
        sort_flag_keys(args.rank.sorts);
    setvbuf(stdout, nullptr, _IOFBF, 1 << 22);
    if (args.rank.by_pick) {
        PickRanker ranker(keys, pick_ranks(dump.pair_bags, dump.prospect_bags,
                                           dump.has_prior));
        return print_ranked(&dump, args, &ranker);
    }
    Ranker ranker(keys, args.rank.by_length);
    return print_ranked(&dump, args, &ranker);
}
