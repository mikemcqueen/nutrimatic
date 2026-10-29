#include "bag-stats-run.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <unordered_map>

#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "letter-bag.h"
#include "log.h"
#include "row-input.h"
#include "workflow-paths.h"

RankOption parse_common_option(std::string_view program, int option,
                               char const* arg, BagStatsArgs* out) {
    RankOption const rank = parse_rank_option(program, option, arg,
                                              &out->rank);
    if (rank != RankOption::NOT_ONE) return rank;
    switch (option) {
      case 'u':
        out->used_letters += arg;
        return RankOption::PARSED;
      case 's':
        return parse_classified_sentence(arg, &out->sentence)
            ? RankOption::PARSED : RankOption::BAD;
      case OPT_COMMON_MIN_WB_LENGTH:
        return parse_count(arg, "--min-wb-len", &out->min_word_bag_length)
            ? RankOption::PARSED : RankOption::BAD;
      case OPT_COMMON_MIN_PB_LENGTH:
        return parse_count(arg, "--min-pb-len", &out->min_pair_bag_length)
            ? RankOption::PARSED : RankOption::BAD;
      case OPT_COMMON_SEARCH:
        out->search_text = arg;
        return RankOption::PARSED;
      case OPT_COMMON_PROSPECTS:
        out->prospects_file = arg;
        return RankOption::PARSED;
      case OPT_COMMON_THREADS:
        return parse_count(arg, "--threads", &out->threads)
            ? RankOption::PARSED : RankOption::BAD;
      case OPT_COMMON_CACHE_SIZE:
        return parse_count(arg, "--cache-size", &out->cache_size_mb)
            ? RankOption::PARSED : RankOption::BAD;
      case OPT_COMMON_NO_NOPAIRS:
        out->drop_no = false;
        return RankOption::PARSED;
      case OPT_COMMON_PROGRESS:
        return parse_count(arg, "--progress", &out->progress)
            ? RankOption::PARSED : RankOption::BAD;
      default:
        return RankOption::NOT_ONE;
    }
}

bool check_common_args(std::string_view program, struct optparse* options,
                       BagStatsArgs* out) {
    char const* const letters = optparse_arg(options);
    char const* const pairs = optparse_arg(options);
    char const* const dictionary = optparse_arg(options);
    if (dictionary == NULL || optparse_arg(options) != NULL) return false;
    out->letters = letters;
    out->pairs_file = pairs;
    out->dictionary_file = dictionary;
    if (out->leftovers && !out->search_text) {
        report(program, "--leftovers needs --ss");
        return false;
    }
    if (!parse_search(program, out->search_text.value_or(DEFAULT_SEARCH),
                      &out->search, !out->leftovers))
        return false;
    if (!out->drop_no && out->sentence != CLASSIFIED_NO_SENTENCE) {
        report(program, "-s doesn't go with --no-nopairs");
        return false;
    }
    RankArgs const& rank = out->rank;
    if (out->leftovers || out->search.prospects + out->search.pairs == 0) {
        std::string const sort =
            rank.sorts.empty() ? "" : std::string("--") +
                rank.sorts[0]->option;
        std::pair<bool, char const*> const rank_options[] = {
            {rank.top > 0, "-n"},
            {!rank.sorts.empty(), sort.c_str()},
            {rank.by_length, "--by-length"},
            {rank.by_pick, "--by-pick"},
            {!rank.score.empty(), "--score"},
            {rank.score_detail, "--score-detail"},
            {rank.prior_file.has_value(), "--prior"},
            {rank.p1.has_value(), "--p1"},
            {rank.clues.has_value(), "--clues"},
            {rank.prior_weights.has_value(), "--prior-weights"},
            {rank.top_segments, "--top-segments"},
            {rank.best_pairs != BestPairs::NONE, "--best-pairs"},
        };
        for (auto const& [given, option] : rank_options) {
            if (!given) continue;
            report(program, out->leftovers
                       ? "--leftovers can't take %s"
                       : "--ss without a ppb or pb step can't take %s",
                   option);
            return false;
        }
    }
    bool const ranked = rank.top > 0 || !rank.score.empty();
    if (rank.top > 0 && rank.sorts.empty()) {
        report(program, "-n needs a SORT option, such as --tp");
        return false;
    }
    if (!rank.sorts.empty() && !ranked) {
        report(program, "SORT options need -n or --score");
        return false;
    }
    if ((rank.by_length || rank.by_pick) && rank.sorts.empty()) {
        report(program, "--by-length and --by-pick need a SORT option, "
               "with -n or --score");
        return false;
    }
    if (!check_rank_args(program, rank, out->search)) return false;
    if ((out->search.prospects > 0) != out->prospects_file.has_value()) {
        report(program, "--pp goes with a ppb step in --ss");
        return false;
    }
    int const stdin_inputs = (out->pairs_file == "-") +
        (out->dictionary_file == "-") + (out->prospects_file == "-") +
        (rank.prior_file == "-");
    if (stdin_inputs > 1) {
        report(program, "only one of PAIRS, DICT, --pp and --prior can "
               "be -");
        return false;
    }
    return true;
}

void help_common_options(bool pair_filter) {
    fprintf(stdout,
          "  for each combination of pair letter bags that fits LETTERS, count\n"
          "  the combinations of DICT word bags that fit what is left, and the\n"
          "  exact pairs (pairs -m%d -x -c) of what is left after each; PAIRS,\n"
          "  DICT or the --pp FILE may be - for stdin; pairs in\n"
          "  $WFROOT/%s/no/no.pairs are dropped from PAIRS and --pp\n"
          "\n"
          "  output: %s\n"
          "\noptions:\n", INNER_MIN_WORD_LENGTH, WORKFLOW_CLASSIFIED_PATH,
          BAG_STATS_COLUMNS);
    dfs_help_used_letters();
    classified_help_sentence_no();
    dfs_help_option("--no-nopairs",
                    "keep the pairs listed in the no.pairs files");
    dfs_help_option("--min-wb-len N",
                    "drop DICT words of fewer than N letters (default %d); "
                    "WB_WITH_MIN_PAIRS counts the fitting word bag "
                    "combinations whose remainder has at least %d exact "
                    "pairs, and --wbfrac-min sorts by WB_WITH_MIN_PAIRS / "
                    "WORDBAGS",
                    DEFAULT_MIN_WB_LENGTH, int(BAG_STATS_MIN_PAIRS));
    dfs_help_option("--min-pb-len N",
                    "pb picks only PAIRS bags of at least N letters "
                    "(default %d)%s", DEFAULT_MIN_PB_LENGTH,
                    pair_filter ? "; pf counts every PAIRS row" : "");
    dfs_help_option("--ss STEPS",
                    "the search sequence, comma-separated: ppb picks a pair "
                    "bag from the --pp FILE, pb one from PAIRS, wb a word bag "
                    "from DICT, and p, last and once, counts the exact pairs "
                    "left%s; order doesn't matter; each row names its ppb "
                    "and pb picks%s (default %s)",
                    pair_filter
                        ? ", or pf instead the PAIRS rows that fit what is "
                          "left; any number of each"
                        : "; at least one ppb or pb and one wb",
                    pair_filter
                        ? ", and without them the one row is all of LETTERS"
                        : "",
                    DEFAULT_SEARCH);
    dfs_help_option("--pp FILE",
                    "the prospective pairs ppb picks from, rows as in PAIRS; "
                    "their bags are dropped from PAIRS");
    dfs_help_option("--threads N",
                    "count on N threads, at most the hardware threads, 0 for "
                    "all of them (default: 0)");
    dfs_help_option("--cache-size N",
                    "hold at most N MB of cached remainder counts, 0 for "
                    "none (default: %d)", BAG_STATS_DEFAULT_CACHE_SIZE_MB);
    dfs_help_option("--progress N",
                    "report each pass's progress to stderr every N seconds, "
                    "and its time when it ends; 0 for never "
                    "(default: %d)", DEFAULT_PROGRESS_SECONDS);
    dfs_help_option("-n N",
                    "print only the N rows that rank first, in that order, "
                    "as bag-stats-sort ranks this run's full dump with the "
                    "same options; needs at least one SORT option");
    help_rank_output_options();
}

void help_common_tail() {
    dfs_help_option("-h, --help", "show this help");
    fputs("\nSORT options, with -n, or with --score for its rank:\n",
          stdout);
    help_rank_sort_options();
}

bool remaining_letters(BagStatsArgs const& args, std::string* out) {
    return subtract_letters(bag_letters(args.letters),
                            bag_letters(args.used_letters), out);
}

void WordBagLoader::add(std::string const& line) {
    PairsCounts const counts = pairs_letter_counts(line);
    if (letter_total(counts) < min_length_ || counts == PairsCounts{} ||
        !pairs_fits(bag_, counts))
        return;
    auto const [found, inserted] = seen_.emplace(counts, out_->size());
    if (inserted) out_->push_back(counts);
    if (!spellings_) return;
    if (inserted) spellings_->emplace_back();
    std::vector<std::string>& words = (*spellings_)[found->second];
    if (std::find(words.begin(), words.end(), line) == words.end())
        words.push_back(line);
}

bool load_word_bags(std::string_view program,
                    std::filesystem::path const& path,
                    PairsCounts const& bag, int min_length,
                    std::vector<PairsCounts>* out,
                    std::vector<std::vector<std::string>>* spellings) {
    WordBagLoader loader(bag, min_length, out, spellings);
    return read_lines(program, path,
        [&](std::string const& line, size_t) {
            loader.add(line);
            return true;
        });
}

bool load_inner_dictionary(std::string_view program, PairsExactCounter* out) {
    std::filesystem::path const root = require_workflow_root(program);
    if (root.empty()) return false;
    std::filesystem::path const path = root / WORKFLOW_DICT_PATH;
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
    out->finish();
    return true;
}

void filter_word_bags(std::vector<PairsCounts> const& from,
                      PairsCounts const& rest, std::vector<PairsCounts>* out) {
    out->clear();
    for (PairsCounts const& word_bag : from)
        if (pairs_fits(rest, word_bag)) out->push_back(word_bag);
}

bool load_rejected_pairs(std::string_view program, BagStatsArgs const& args,
                         DfsPairSet* out) {
    if (!args.drop_no) return true;
    if (args.sentence == CLASSIFIED_NO_SENTENCE)
        alert(program, "--sentence not supplied");
    std::filesystem::path const root = require_workflow_root(program);
    return !root.empty() && load_no_pairs(program, root, args.sentence, out);
}

bool load_bag_stats_inputs(std::string_view program, BagStatsArgs const& args,
                           BagStatsInputs* out,
                           std::vector<std::vector<std::string>>* spellings) {
    bool const pair_filter = args.search.pair_filter;
    DfsPairSet rejected;
    if (!load_rejected_pairs(program, args, &rejected) ||
        !load_pick_bags(program, args.rank, args.prospects_file,
                        args.pairs_file, out->bag, rejected,
                        args.min_pair_bag_length, &out->prospect_bags,
                        args.search.pairs == 0 ? nullptr : &out->pair_bags,
                        pair_filter ? &out->final_bags : nullptr,
                        pair_filter && spellings && args.search.words == 0
                            ? &out->final_rows : nullptr,
                        &out->has_prior) ||
        (args.search.words > 0 &&
         !load_word_bags(program, args.dictionary_file, out->bag,
                         args.min_word_bag_length, &out->word_bags,
                         spellings)) ||
        (args.search.measured && !pair_filter &&
         !load_inner_dictionary(program, &out->counter)))
        return false;
    out->steps.assign(args.search.pairs, &out->pair_bags);
    out->steps.insert(out->steps.end(), args.search.prospects,
                      &out->prospect_bags);
    return true;
}

size_t bag_stats_threads(BagStatsArgs const& args) {
    size_t threads = resolve_search_threads(args.threads);
    unsigned int const cores = std::thread::hardware_concurrency();
    if (cores != 0) threads = std::min(threads, size_t(cores));
    return threads;
}

std::string format_duration(double seconds) {
    long const s = std::lround(seconds);
    char out[32];
    if (s >= 3600)
        snprintf(out, sizeof out, "%ldh%02ldm", s / 3600, s / 60 % 60);
    else if (s >= 60)
        snprintf(out, sizeof out, "%ldm%02lds", s / 60, s % 60);
    else
        snprintf(out, sizeof out, "%lds", s);
    return out;
}

std::string PickWalk::pair() const {
    std::string out;
    for (size_t k = 0; k < steps_.size(); ++k) {
        size_t const j = (k + pair_steps_) % steps_.size();
        if (k > 0) out += " + ";
        out += (*steps_[j])[chosen_[j]].pair;
    }
    return out;
}

void PickWalk::picks(std::vector<uint32_t>* out) const {
    out->resize(steps_.size());
    for (size_t j = 0; j < steps_.size(); ++j)
        (*out)[j] = uint32_t(chosen_[j] +
                             (j < pair_steps_ ? 0 : pair_bags_));
}

uint64_t PickWalk::pairs() const {
    uint64_t out = 1;
    for (size_t j = 0; j < steps_.size(); ++j)
        out *= (*steps_[j])[chosen_[j]].pairs;
    return out;
}

bool PickWalk::repeats_bag(size_t step, PairsCounts const& counts) const {
    for (size_t j = 0; j < step; ++j)
        if (steps_[j] != steps_[step] &&
            (*steps_[j])[chosen_[j]].counts == counts)
            return true;
    return false;
}

std::vector<size_t> unit_blocks(size_t units) {
    std::vector<size_t> out(units + 1);
    for (size_t unit = 0; unit <= units; ++unit) out[unit] = unit;
    return out;
}

std::vector<size_t> census_blocks(
        std::vector<std::vector<PairBag> const*> const& steps, int pair_steps,
        PairsCounts const& bag, size_t threads, uint64_t rows) {
    size_t const units = walk_units(steps);
    std::vector<uint64_t> unit_rows(units);
    std::atomic<size_t> next{0};
    run_threads(threads, [&](size_t) {
        PickWalk walk(steps, pair_steps, 0);
        auto const enter = [](size_t, PairsCounts const&) {};
        for (size_t unit; (unit = next.fetch_add(1)) < units;) {
            uint64_t count = 0;
            walk.walk(unit, bag, enter,
                      [&](PairsCounts const&, int) { ++count; });
            unit_rows[unit] = count;
        }
    });
    std::vector<size_t> out{0};
    uint64_t block_rows = 0;
    for (size_t unit = 0; unit < units; ++unit) {
        if (block_rows > 0 && block_rows + unit_rows[unit] > rows) {
            out.push_back(unit);
            block_rows = 0;
        }
        block_rows += unit_rows[unit];
    }
    if (out.back() != units) out.push_back(units);
    return out;
}

PairsCounts picks_rest(WalkBase const& walk,
                       std::vector<size_t> const& picks) {
    PairsCounts rest = walk.bag;
    for (size_t j = 0; j < picks.size(); ++j)
        pairs_subtract(rest, (*walk.steps[j])[picks[j]].counts, &rest);
    return rest;
}

void name_picks(WalkBase const& walk, std::vector<size_t> const& picks,
                BagStatsRow* row) {
    size_t const pair_steps = size_t(walk.args.search.pairs);
    row->pair.clear();
    row->pairs = 1;
    for (size_t k = 0; k < picks.size(); ++k) {
        size_t const j = (k + pair_steps) % picks.size();
        PairBag const& pick = (*walk.steps[j])[picks[j]];
        if (k > 0) row->pair += " + ";
        row->pair += pick.pair;
        row->pairs *= pick.pairs;
    }
}

void write_walk_header(WalkBase const& walk, std::string const& letters) {
    BagStatsArgs const& args = walk.args;
    write_bag_stats_header(stdout, {letters, INNER_MIN_WORD_LENGTH,
                                    args.min_word_bag_length,
                                    args.min_pair_bag_length,
                                    search_text(args.search),
                                    args.prospects_file
                                        ? bag_stats_header_path(
                                              *args.prospects_file)
                                        : "",
                                    args.sentence,
                                    bag_stats_header_path(args.pairs_file),
                                    bag_stats_header_path(args.dictionary_file)});
}

BagStatsRankers::BagStatsRankers(BagStatsArgs const& args,
                                 BagStatsInputs const& inputs)
    : ranker(sort_flag_keys(args.rank.sorts), args.rank.by_length),
      pick_ranker(sort_flag_keys(args.rank.sorts),
                  pick_ranks(inputs.pair_bags, inputs.prospect_bags,
                             inputs.has_prior)),
      passes((args.rank.top != 0 || !args.rank.score.empty()) &&
             (args.rank.by_pick || ranker.needs_counts())) {}
