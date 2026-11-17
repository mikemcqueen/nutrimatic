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
#include "parse-number.h"
#include "workflow-paths.h"

#include <algorithm>
#include <cmath>
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
constexpr int OPT_PAIRS = 259;
constexpr int OPT_BEST_ROWS = 260;
constexpr int OPT_SORT = 261;
constexpr int OPT_PAIR_SCORE = 262;
constexpr int OPT_GOAL = 263;

struct Args {
    int sentence = CLASSIFIED_NO_SENTENCE;
    RankArgs rank;
    int pair_bag_length = -1;
    bool best_rows = false;
    // -x, each pair's letters.
    std::vector<PairsCounts> excluded;
    // --goal, each word,word.
    std::vector<std::string> goals;
    // --ss, --pp and --pairs, when given.
    std::optional<std::string> search;
    std::optional<std::filesystem::path> prospects_file;
    std::optional<std::filesystem::path> pairs_file;
    std::filesystem::path input_file = "-";
};

void usage(std::string_view program, FILE* out) {
    fprintf(out,
            "usage: %.*s [-s N] [-n N] --sort KEY[,KEY...]...\n"
            "       [--by-length | --by-pick]\n"
            "       [--score W1,W2... [--score-detail]]\n"
            "       [--best-pairs appear|lead | --best-rows | "
            "--pair-score LAMBDA [--goal PAIR]...]\n"
            "       [--prior FILE | --no-prior |\n"
            "        [--p1 INDEX] [--clues CLUEDATA] [--prior-weights SPEC]]\n"
            "       [--top-segments] [--pb-len N] [--ss STEPS] [--pp FILE]\n"
            "       [--pairs PAIRS-FILE] [-x PAIR]... [FILE|-]\n",
            SV_ARG(program));
    if (out != stdout) return;

    fputs("  rank the rows of the bag-stats dump FILE, standard input when it\n"
          "  is '-' or not given, as bag-stats ranks its rows with the same\n"
          "  options, among the dump's rows only, and print them sorted;\n"
          "  equal rows keep their order\n"
          "\n"
          "  the pick bags are rebuilt from the PAIRS-FILE and --pp files the\n"
          "  header names, or --pairs' and --pp's, less the classified NO\n"
          "  pairs of the dump's sentence; a row whose pick has no pairs left\n"
          "  is dropped, and the others' PAIR and PAIRS are rebuilt from\n"
          "  their bags\n"
          "\n"
          "  with more than one sort key, each row's value under each key\n"
          "  becomes its percentile among all rows, and rows sort by\n"
          "  their best percentile, then by the sum of them: the sorted lists\n"
          "  merged, each row at its better place\n"
          "\noptions:\n", stdout);
    dfs_help_option("-s, --sentence N",
                    "drop pairs listed in $WFROOT/%s/sN/no/no.pairs, in "
                    "place of the dump's sentence's",
                    WORKFLOW_CLASSIFIED_PATH);
    dfs_help_option("-n N", "print only the N rows that rank first");
    help_rank_output_options();
    dfs_help_option("--best-rows",
                    "print only the rows none of whose ppb and pb picks is in "
                    "a row printed before; -n counts the rows printed");
    dfs_help_option("-x, --exclude PAIR",
                    "drop the rows with a ppb or pb pick whose bag is "
                    "PAIR's, word,word, before ranking; repeatable");
    dfs_help_option("--goal PAIR",
                    "with --pair-score, one per ppb and pb step, each "
                    "word,word: print the goal picks' lines, each with its "
                    "RANK among all picks, then for each goal pick, where "
                    "the row with every goal pick ranks among its rows by "
                    "row score for the pick, and those rows by that score: "
                    "SCORE POINTS, each sort key's top fraction F, each "
                    "pick's PRIOR and PAIR, '*' marking the goal row; -n "
                    "prints only the first N rows of each goal pick");
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
    dfs_help_option("--pairs PAIRS-FILE",
                    "the pairs the pb picks are from, in place of the "
                    "dump's");
    dfs_help_option("-h, --help", "show this help");
    fputs("\n--sort KEY[,KEY...], at least one KEY, repeatable; each "
          "sorts by:\n", stdout);
    bag_stats_help_sorts();
    fputs("\nranking options:\n", stdout);
    help_rank_sort_options(true);
}

bool parse_args(char* argv[], Args* out, bool* help) {
    static struct optparse_long const long_options[] = {
        CLASSIFIED_SENTENCE_LONG_OPTION,
        BAG_STATS_RANK_LONG_OPTIONS,
        { "pb-len", OPT_PAIR_BAG_LENGTH, OPTPARSE_REQUIRED },
        { "ss", OPT_SEARCH, OPTPARSE_REQUIRED },
        { "pp", OPT_PROSPECTS, OPTPARSE_REQUIRED },
        { "pairs", OPT_PAIRS, OPTPARSE_REQUIRED },
        { "best-rows", OPT_BEST_ROWS, OPTPARSE_NONE },
        { "sort", OPT_SORT, OPTPARSE_REQUIRED },
        { "pair-score", OPT_PAIR_SCORE, OPTPARSE_REQUIRED },
        { "goal", OPT_GOAL, OPTPARSE_REQUIRED },
        { "exclude", 'x', OPTPARSE_REQUIRED },
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
          case OPT_PAIRS:
            out->pairs_file = options.optarg;
            break;
          case OPT_BEST_ROWS:
            out->best_rows = true;
            break;
          case OPT_SORT:
            for (std::string_view keys = options.optarg;;) {
                size_t const comma = keys.find(',');
                std::string_view const name = keys.substr(0, comma);
                BagStatsSort const* const sort = find_bag_stats_sort(name);
                if (sort == NULL) {
                    report(PROGRAM, "--sort has no key %.*s",
                           SV_ARG(name));
                    return false;
                }
                out->rank.sorts.push_back(sort);
                if (comma == std::string_view::npos) break;
                keys.remove_prefix(comma + 1);
            }
            break;
          case OPT_PAIR_SCORE: {
            double lambda;
            if (!parse_number(std::string_view(options.optarg), &lambda) ||
                !std::isfinite(lambda) || lambda < 0) {
                report(PROGRAM, "--pair-score LAMBDA is a finite number, "
                       "not negative");
                return false;
            }
            out->rank.pair_score = lambda;
            break;
          }
          case OPT_GOAL:
            out->goals.push_back(options.optarg);
            break;
          case 'x': {
            std::string_view const pair = options.optarg;
            size_t const comma = pair.find(',');
            if (comma == std::string_view::npos || comma == 0 ||
                comma + 1 == pair.size() ||
                pair.find(',', comma + 1) != std::string_view::npos) {
                report(PROGRAM, "-x %s isn't word,word", options.optarg);
                return false;
            }
            out->excluded.push_back(pairs_letter_counts(pair));
            break;
          }
          case 'h':
            *help = true;
            return true;
          default:
            fprintf(stderr, "error: %s\n", options.errmsg);
            return false;
        }
    }

    if (out->rank.sorts.empty()) {
        report(PROGRAM, "give --sort");
        return false;
    }
    if (out->best_rows &&
        (out->rank.best_pairs != BestPairs::NONE ||
         !out->rank.score.empty())) {
        report(PROGRAM, "--best-rows goes without --best-pairs and --score");
        return false;
    }
    if (!out->goals.empty() && !out->rank.pair_score) {
        report(PROGRAM, "--goal goes with --pair-score");
        return false;
    }
    if (out->best_rows && out->rank.pair_score) {
        report(PROGRAM, "--pair-score goes without --best-rows");
        return false;
    }
    char const* const input = optparse_arg(&options);
    if (input != NULL) out->input_file = input;
    if ((out->input_file == "-") + (out->rank.prior_file == "-") +
        (out->prospects_file == "-") + (out->pairs_file == "-") > 1) {
        report(PROGRAM, "only one of FILE, --prior, --pp and --pairs can be -");
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
    uint64_t excluded = 0;
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
        if (std::any_of(names.begin(), names.end(),
                        [&](std::string const& name) {
            PairsCounts const counts = pairs_letter_counts(name);
            return std::find(args.excluded.begin(), args.excluded.end(),
                             counts) != args.excluded.end();
        })) {
            ++excluded;
            continue;
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
    if (!args.excluded.empty())
        report(PROGRAM, "-x dropped %llu of %zu rows",
               (unsigned long long)excluded, line_number);
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
        if (args.pairs_file) h.pairs = bag_stats_header_path(*args.pairs_file);
        else if (pick_pairs && h.pairs == "-") {
            report(PROGRAM, "the dump's PAIRS-FILE was standard input; give "
                   "--pairs");
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
            print_score_detail("", args.rank.sorts, ranker, target, bags,
                               target_picks);
    return 0;
}

// Counts the dump's rows into `ranker` and finishes it.
template <typename R>
void count_rows(Dump const& dump, R* ranker) {
    std::vector<typename R::Part> parts(1, ranker->part());
    for (RankedRow const& row : dump.rows)
        ranker->add(&parts[0], row.row, bag_stats_pair_length(row.row.pair),
                    dump.row_picks(row.seq));
    ranker->finish(parts);
}

// Ranks the dump's rows with `ranker` and prints them, the -n first of them,
// or the --score row.
template <typename R>
int print_ranked(Dump* dump, Args const& args, R* ranker) {
    RankArgs const& rank = args.rank;
    if (ranker->needs_counts()) count_rows(*dump, ranker);
    if (!rank.score.empty()) return print_score(*dump, args, *ranker);

    std::vector<RankedRow>& rows = dump->rows;
    for (RankedRow& row : rows)
        row.key = ranker->key(row.row, bag_stats_pair_length(row.row.pair),
                              dump->row_picks(row.seq));
    if (rank.top > 0 && size_t(rank.top) < rows.size() && !args.best_rows) {
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
    size_t printed = 0;
    for (RankedRow& ranked : rows) {
        line.clear();
        std::span<uint32_t const> const picks = dump->row_picks(ranked.seq);
        if (args.best_rows) {
            if (rank.top > 0 && printed == size_t(rank.top)) break;
            if (std::any_of(picks.begin(), picks.end(),
                            [&](uint32_t pick) { return seen.count(pick); }))
                continue;
            seen.insert(picks.begin(), picks.end());
            ++printed;
        }
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

// Prints --pair-score's line per pick, best first, or with --goal, the goal
// picks and the rows with them.
int print_pair_scores(Dump const& dump, Args const& args) {
    std::vector<BagStatsSort const*> const& keys = args.rank.sorts;
    double const lambda = *args.rank.pair_score;

    std::vector<uint32_t> goal;
    if (!args.goals.empty()) {
        if (args.goals.size() !=
            size_t(dump.search.prospects + dump.search.pairs)) {
            report(PROGRAM, "--goal names one pick per ppb and pb step, %d",
                   dump.search.prospects + dump.search.pairs);
            return 1;
        }
        std::vector<size_t> picks;
        if (!score_picks(PROGRAM, args.goals, dump.search, dump.steps,
                         pairs_letter_counts(dump.header.letters), &picks,
                         "--goal"))
            return 1;
        for (size_t j = 0; j < picks.size(); ++j)
            goal.push_back(uint32_t(
                picks[j] + (j < size_t(dump.search.pairs)
                            ? 0 : dump.pair_bags.size())));
    }

    size_t const pair_count = dump.pair_bags.size();
    size_t const pick_count = pair_count + dump.prospect_bags.size();
    PickRanker ranker(keys, std::vector<double>(pick_count, 1.0));
    count_rows(dump, &ranker);

    auto const bag = [&](uint32_t pick) -> PairBag const& {
        return pick < pair_count ? dump.pair_bags[pick]
                                 : dump.prospect_bags[pick - pair_count];
    };
    auto const prior = [&](uint32_t pick) {
        double const p = bag(pick).prior;
        return std::isfinite(p) ? p : 0.0;
    };
    auto const words = [&](uint32_t pick) {
        std::string out = bag(pick).pair;
        std::replace(out.begin(), out.end(), ' ', ',');
        return out;
    };
    // The row's POINTS for its pick `i`, and into `fractions` unless null
    // each key's f_k, the largest over the other picks.
    auto const row_points = [&](BagStatsRow const& row,
                                std::span<uint32_t const> row_picks, size_t i,
                                std::vector<double>* fractions) {
        if (fractions) fractions->assign(keys.size(), 0);
        double f = 0;
        for (size_t j = 0; j < row_picks.size(); ++j) {
            if (j == i) continue;
            double best = 1;
            for (size_t k = 0; k < keys.size(); ++k) {
                double const f_k = ranker.top_fraction(
                    k, row_picks[j], bag_stats_sort_value(keys[k], row));
                if (fractions)
                    (*fractions)[k] = std::max((*fractions)[k], f_k);
                best = std::min(best, f_k);
            }
            f = std::max(f, best);
        }
        return 0.0 - std::log2(std::min(1.0, double(keys.size()) * f));
    };
    auto const prior_sum = [&](std::span<uint32_t const> row_picks) {
        double sum = 0;
        for (uint32_t pick : row_picks) sum += prior(pick);
        return sum;
    };

    struct Pick {
        uint64_t rows = 0;
        double best = 0;
        double points = 0;
        uint64_t seq = 0;
    };
    std::vector<Pick> picks(pick_count);
    for (RankedRow const& ranked : dump.rows) {
        std::span<uint32_t const> const row_picks = dump.row_picks(ranked.seq);
        double const priors = prior_sum(row_picks);
        for (size_t i = 0; i < row_picks.size(); ++i) {
            double const points =
                row_points(ranked.row, row_picks, i, nullptr);
            double const score = priors + lambda * points;
            Pick& p = picks[row_picks[i]];
            if (p.rows++ == 0 || score > p.best) {
                p.best = score;
                p.points = points;
                p.seq = ranked.seq;
            }
        }
    }

    std::vector<std::pair<double, uint32_t>> order;
    for (uint32_t pick = 0; pick < picks.size(); ++pick)
        if (picks[pick].rows > 0) order.emplace_back(picks[pick].best, pick);
    std::stable_sort(order.begin(), order.end(),
                     [](auto const& a, auto const& b) {
                         return a.first > b.first;
                     });

    auto const print_pick = [&](double score, uint32_t pick) {
        Pick const& p = picks[pick];
        printf("%.3f %.3f %.3f %llu %s %s\n", score, prior(pick), p.points,
               (unsigned long long)p.rows, words(pick).c_str(),
               dump.rows[p.seq].row.pair.c_str());
    };
    if (goal.empty()) {
        if (args.rank.top > 0 && size_t(args.rank.top) < order.size())
            order.resize(size_t(args.rank.top));
        fputs("SCORE PRIOR POINTS ROWS PICK BEST\n", stdout);
        for (auto const& [score, pick] : order) print_pick(score, pick);
        return 0;
    }

    fputs("RANK SCORE PRIOR POINTS ROWS PICK BEST\n", stdout);
    for (uint32_t pick : goal) {
        auto const found = std::find_if(order.begin(), order.end(),
            [&](auto const& entry) { return entry.second == pick; });
        if (found == order.end()) {
            printf("- - %.3f - 0 %s -\n", prior(pick), words(pick).c_str());
            continue;
        }
        printf("%zu/%zu ", size_t(found - order.begin()) + 1,
               order.size());
        print_pick(found->first, pick);
    }

    // Each goal pick's rows, by the row score for the pick, then walk order.
    struct GoalRow {
        double score;
        double points;
        uint64_t seq;
        size_t index;
        bool goal;
    };
    for (uint32_t pick : goal) {
        std::vector<GoalRow> rows;
        for (RankedRow const& ranked : dump.rows) {
            std::span<uint32_t const> const row_picks =
                dump.row_picks(ranked.seq);
            auto const at =
                std::find(row_picks.begin(), row_picks.end(), pick);
            if (at == row_picks.end()) continue;
            size_t const index = size_t(at - row_picks.begin());
            bool const all = std::all_of(goal.begin(), goal.end(),
                [&](uint32_t g) {
                    return std::find(row_picks.begin(), row_picks.end(),
                                     g) != row_picks.end();
                });
            double const points =
                row_points(ranked.row, row_picks, index, nullptr);
            rows.push_back({prior_sum(row_picks) + lambda * points, points,
                            ranked.seq, index, all});
        }
        std::stable_sort(rows.begin(), rows.end(),
                         [](GoalRow const& a, GoalRow const& b) {
                             return a.score > b.score;
                         });

        putchar('\n');
        auto const goal_row = std::find_if(rows.begin(), rows.end(),
            [](GoalRow const& row) { return row.goal; });
        if (goal_row == rows.end()) {
            printf("%s: no kept row has every --goal pick\n",
                   words(pick).c_str());
        } else {
            uint64_t above = 0;
            uint64_t tied = 0;
            for (GoalRow const& row : rows) {
                if (&row == &*goal_row) continue;
                above += row.score > goal_row->score;
                tied += row.score == goal_row->score;
            }
            printf("%s: the goal row is %llu of %zu rows, %llu above it "
                   "and %llu tied with it\n", words(pick).c_str(),
                   (unsigned long long)(above + 1), rows.size(),
                   (unsigned long long)above, (unsigned long long)tied);
        }

        fputs("SCORE POINTS", stdout);
        for (BagStatsSort const* key : keys) printf(" F:%s", key->option);
        fputs(" PRIORS PAIR\n", stdout);
        std::vector<double> fractions;
        size_t printed = 0;
        for (GoalRow const& row : rows) {
            if (args.rank.top > 0 && printed++ == size_t(args.rank.top)) break;
            BagStatsRow const& bag_row = dump.rows[row.seq].row;
            std::span<uint32_t const> const row_picks =
                dump.row_picks(row.seq);
            row_points(bag_row, row_picks, row.index, &fractions);
            printf("%s%.3f %.3f", row.goal ? "*" : "", row.score, row.points);
            for (double f : fractions) printf(" %.4g", f);
            // PAIR names ppb picks first, then pb picks.
            char separator = ' ';
            for (size_t i = size_t(dump.search.pairs); i < row_picks.size();
                 ++i) {
                printf("%c%.3f", separator, prior(row_picks[i]));
                separator = ',';
            }
            for (size_t i = 0; i < size_t(dump.search.pairs); ++i) {
                printf("%c%.3f", separator, prior(row_picks[i]));
                separator = ',';
            }
            printf(" %s\n", bag_row.pair.c_str());
        }
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

    std::vector<BagStatsSort const*> const& keys = args.rank.sorts;
    setvbuf(stdout, nullptr, _IOFBF, 1 << 22);
    if (args.rank.pair_score) return print_pair_scores(dump, args);
    if (args.rank.by_pick) {
        PickRanker ranker(keys, pick_ranks(dump.pair_bags, dump.prospect_bags,
                                           dump.has_prior));
        return print_ranked(&dump, args, &ranker);
    }
    Ranker ranker(keys, args.rank.by_length);
    return print_ranked(&dump, args, &ranker);
}
