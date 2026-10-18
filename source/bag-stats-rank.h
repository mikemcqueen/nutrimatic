#ifndef NUTRIMATIC_BAG_STATS_RANK_H
#define NUTRIMATIC_BAG_STATS_RANK_H

// Pick bags and the ranking bag-stats and bag-stats-sort share.

#include <stdint.h>
#include <stdio.h>

#include <cmath>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "bag-stats-impl.h"
#include "dfs-class-list.h"
#include "dfs-cli-args.h"
#include "optparse.h"
#include "pairs-impl.h"

// How many picks of each kind a search sequence makes before its last step,
// and which last step it is: p, counting DICT pairs, or pf, counting PAIRS
// rows (pair_filter).
struct Search {
    int prospects = 0;
    int pairs = 0;
    int words = 0;
    bool pair_filter = false;
};

// Parses --ss STEPS into `out`. False, diagnosed prefixed by `program`, when
// it isn't one.
bool parse_search(char const* program, char const* text, Search* out);

// The steps of `search` in canonical order.
std::string search_text(Search const& search);

// --best-pairs: which of a row's picks it recognizes as best pairs.
enum class BestPairs { NONE, APPEAR, LEAD };

// The ranking options bag-stats and bag-stats-sort share.
struct RankArgs {
    // -n; 0 prints every row.
    int top = 0;
    // The sort options, in command line order.
    std::vector<BagStatsSortFlag const*> sorts;
    bool by_length = false;
    bool by_pick = false;
    char const* prior_file = NULL;
    char const* p1 = NULL;
    char const* clues = NULL;
    char const* prior_weights = NULL;
    bool top_segments = false;
    // --score, the row's picks as PAIR names them, each as its two words.
    std::vector<std::string> score;
    bool score_detail = false;
    BestPairs best_pairs = BestPairs::NONE;
};

enum {
    OPT_RANK_BY_LENGTH = 512,
    OPT_RANK_TOP_SEGMENTS,
    OPT_RANK_SCORE,
    OPT_RANK_PRIOR,
    OPT_RANK_BY_PICK,
    OPT_RANK_P1,
    OPT_RANK_CLUES,
    OPT_RANK_PRIOR_WEIGHTS,
    OPT_RANK_SCORE_DETAIL,
    OPT_RANK_BEST_PAIRS,
    OPT_RANK_SORT,
};

// The optparse_long entries of RankArgs' options.
#define BAG_STATS_RANK_LONG_OPTIONS \
    { NULL, 'n', OPTPARSE_REQUIRED }, \
    { "by-length", OPT_RANK_BY_LENGTH, OPTPARSE_NONE }, \
    { "top-segments", OPT_RANK_TOP_SEGMENTS, OPTPARSE_NONE }, \
    { "score", OPT_RANK_SCORE, OPTPARSE_REQUIRED }, \
    { "prior", OPT_RANK_PRIOR, OPTPARSE_REQUIRED }, \
    { "by-pick", OPT_RANK_BY_PICK, OPTPARSE_NONE }, \
    { "p1", OPT_RANK_P1, OPTPARSE_REQUIRED }, \
    { "clues", OPT_RANK_CLUES, OPTPARSE_REQUIRED }, \
    { "prior-weights", OPT_RANK_PRIOR_WEIGHTS, OPTPARSE_REQUIRED }, \
    { "score-detail", OPT_RANK_SCORE_DETAIL, OPTPARSE_NONE }, \
    { "best-pairs", OPT_RANK_BEST_PAIRS, OPTPARSE_REQUIRED }, \
    { BAG_STATS_SORT_FLAGS[0].option, OPT_RANK_SORT + 0, OPTPARSE_NONE }, \
    { BAG_STATS_SORT_FLAGS[1].option, OPT_RANK_SORT + 1, OPTPARSE_NONE }, \
    { BAG_STATS_SORT_FLAGS[2].option, OPT_RANK_SORT + 2, OPTPARSE_NONE }, \
    { BAG_STATS_SORT_FLAGS[3].option, OPT_RANK_SORT + 3, OPTPARSE_NONE }, \
    { BAG_STATS_SORT_FLAGS[4].option, OPT_RANK_SORT + 4, OPTPARSE_NONE }, \
    { BAG_STATS_SORT_FLAGS[5].option, OPT_RANK_SORT + 5, OPTPARSE_NONE }

enum class RankOption { NOT_ONE, PARSED, BAD };

// Applies `option`, with `arg`, to `out` when it is one of
// BAG_STATS_RANK_LONG_OPTIONS. A bad argument is diagnosed, prefixed by
// `program`.
RankOption parse_rank_option(char const* program, int option, char const* arg,
                             RankArgs* out);

// The checks of `args` both programs make, given the search sequence they
// rank rows of. False, diagnosed prefixed by `program`, when one fails.
bool check_rank_args(char const* program, RankArgs const& args,
                     Search const& search);

// Prints the --help lines of --top-segments, --score, --score-detail and
// --best-pairs.
void help_rank_output_options();

// Prints the --help lines of the SORT options, --by-length, --by-pick and the
// prior options, and the --prior-weights terms.
void help_rank_sort_options();

struct PairBag {
    PairsCounts counts;
    std::string pair;
    uint64_t pairs = 0;
    // The best prior of its rows, from --prior or --p1 and --clues;
    // -INFINITY when none has one.
    double prior = -INFINITY;
};

// The --prior FILE's scores by pair_key().
bool load_priors(char const* program, char const* path,
                 std::unordered_map<std::string, double>* out);

// `prior` sets a row's prior and returns true, or returns false when it has
// none.
using PairPrior = std::function<bool(DfsPairRow const&, double*)>;

// Collects pair bags into `out`: those of the rows added that fit `bag`, in
// order of each bag's first row, which names it, counting each distinct pair
// once. With `rows`, also each bag's distinct pairs as LEFT,RIGHT, in order.
class PairBagLoader {
  public:
    PairBagLoader(PairsCounts const& bag, PairPrior const& prior,
                  std::vector<PairBag>* out,
                  std::vector<std::vector<std::string>>* rows = nullptr)
        : bag_(bag), prior_(prior), out_(out), bag_rows_(rows) {}

    void add(DfsPairRow const& row);

  private:
    PairsCounts bag_;
    PairPrior prior_;
    std::vector<PairBag>* out_;
    std::vector<std::vector<std::string>>* bag_rows_;
    std::unordered_set<std::string> rows_;
    std::unordered_map<PairsCounts, size_t, PairsCountsHash> indices_;
};

int letter_total(PairsCounts const& counts);

// Appends to `out` the bags of the rows of `path` that fit `bag` and aren't
// `rejected`, as PairBagLoader collects them, with `rows`.
bool load_pair_bags(char const* program, char const* path,
                    PairsCounts const& bag, DfsPairSet const& rejected,
                    PairPrior const& prior, std::vector<PairBag>* out,
                    std::vector<std::vector<std::string>>* rows = nullptr);

// Whether pb may not pick a bag of PAIRS: one of `prospect_bags`' bags, or
// one of fewer than `min_length` letters.
class PairPickFilter {
  public:
    PairPickFilter(std::vector<PairBag> const& prospect_bags, int min_length);

    bool dropped(PairBag const& pair_bag) const;

  private:
    std::unordered_set<PairsCounts, PairsCountsHash> prospects_;
    int min_length_;
};

// load_pair_bags() of PAIRS into `pair_bags`, less PairPickFilter's, and,
// unless `prospects_file` is NULL, of it into `prospect_bags`, their priors
// from `args`' --prior or --p1, --clues and --prior-weights, freed before it
// returns. Unless `final_bags` is null, also every bag of PAIRS into it, with
// `final_rows` as load_pair_bags() takes `rows`. `pair_bags` may be null to
// skip it, and with `final_bags` null too PAIRS isn't read. `has_prior`:
// whether `args` gives one.
bool load_pick_bags(char const* program, RankArgs const& args,
                    char const* prospects_file, char const* pairs_file,
                    PairsCounts const& bag, DfsPairSet const& rejected,
                    int min_pair_length, std::vector<PairBag>* prospect_bags,
                    std::vector<PairBag>* pair_bags,
                    std::vector<PairBag>* final_bags,
                    std::vector<std::vector<std::string>>* final_rows,
                    bool* has_prior);

struct LengthValueHash {
    size_t operator()(std::pair<size_t, double> const& key) const {
        return std::hash<size_t>()(key.first) * 31 +
            std::hash<double>()(key.second);
    }
};

// How many rows have each (length group, sort value), one map per sort key.
using SortValueCounts = std::vector<
    std::unordered_map<std::pair<size_t, double>, uint64_t, LengthValueHash>>;

// A sorted distribution of values, each with how many rows are below it.
class Distribution {
  public:
    Distribution() = default;

    // From `counts`, values with how many rows have them; a value may repeat.
    explicit Distribution(std::vector<std::pair<double, uint64_t>> counts);

    // The mid-rank percentile of `value`, one of the values.
    double percentile(double value) const;

    uint64_t size() const { return below_.back(); }

    // How many rows are above `value`, one of the values, and how many have
    // it.
    std::pair<uint64_t, uint64_t> above_and_tied(double value) const;

  private:
    std::vector<double> values_;
    // below_[i] rows are below values_[i]; the last is every row.
    std::vector<uint64_t> below_;
};

// Ranks rows by their sort values, from how many rows have each sort value
// rather than from the rows themselves.
class Ranker {
  public:
    Ranker(std::vector<BagStatsCount const*> keys, bool by_length)
        : keys_(std::move(keys)), by_length_(by_length) {}

    // Whether key() needs finish() first: a row's rank then depends on the
    // other rows.
    bool needs_counts() const { return by_length_ || keys_.size() > 1; }

    // Whether add() and key() read the row's picks.
    static constexpr bool NEEDS_PICKS = false;

    using Part = SortValueCounts;

    Part part() const { return Part(keys_.size()); }

    void add(Part* part, BagStatsRow const& row, size_t length,
             std::span<uint32_t const>) const;

    void finish(std::vector<Part>& parts);

    // The row's sort key, larger first: its sort value, its percentile
    // within its length with --by-length, and with more than one sort key,
    // its best percentile among all rows, the sum of them breaking ties.
    std::pair<double, double> key(BagStatsRow const& row, size_t length,
                                  std::span<uint32_t const>) const;

  private:
    size_t group(size_t length) const { return by_length_ ? length : 0; }

    // `value` for key `k`, as its percentile within its group with
    // --by-length.
    double first_rank(size_t k, size_t group, double value) const {
        return by_length_ ? groups_[k].at(group).percentile(value) : value;
    }

    std::vector<BagStatsCount const*> keys_;
    bool by_length_;
    std::vector<std::unordered_map<size_t, Distribution>> groups_;
    std::vector<Distribution> overall_;
};

// Ranks rows for --by-pick: each sort value as its percentile among the rows
// sharing a pick, weighted by the picks' prior ranks.
class PickRanker {
  public:
    // `ranks`: each pick's P, its prior's percentile from the top, 1 for
    // every pick without a prior.
    PickRanker(std::vector<BagStatsCount const*> keys,
               std::vector<double> ranks)
        : keys_(std::move(keys)), ranks_(std::move(ranks)) {}

    bool needs_counts() const { return true; }

    static constexpr bool NEEDS_PICKS = true;

    // How many rows sharing a pick have a sort value: `group` is the sort
    // key's index times the number of picks, plus the pick.
    struct Entry {
        double value;
        uint32_t group;
        uint32_t count;
    };

    struct Part {
        std::vector<Entry> entries;
        size_t limit = size_t(1) << 20;
    };

    Part part() const { return {}; }

    void add(Part* part, BagStatsRow const& row, size_t,
             std::span<uint32_t const> picks) const;

    void finish(std::vector<Part>& parts);

    // Larger first: the smallest P * Q * sqrt(P) * Q^(1/4) of each other pick
    // over the row's picks, negated, then the sum of those, negated; each is
    // sqrt(P) * Q^(3/4) of its pick times sqrt(P) * Q^(1/4) of every pick.
    std::pair<double, double> key(BagStatsRow const& row, size_t,
                                  std::span<uint32_t const> picks) const;

    // The row's picks whose own factor is the smallest, the one its key has.
    std::vector<uint32_t> leaders(BagStatsRow const& row,
                                  std::span<uint32_t const> picks) const;

    // What key() makes of one of a row's picks.
    struct PickDetail {
        double p;
        uint64_t rows;
        // Each sort key's value, and among the pick's rows, the rows above
        // it, the rows with it and its Q, and the term with that Q.
        std::vector<double> values;
        std::vector<std::pair<uint64_t, uint64_t>> above_and_tied;
        std::vector<double> qs;
        std::vector<double> terms;
        double q;
        double term;
    };

    // key()'s parts for each of the row's picks, in `picks` order.
    std::vector<PickDetail> detail(BagStatsRow const& row,
                                   std::span<uint32_t const> picks) const;

  private:
    static std::pair<double, double> factors(double p, double q);

    std::pair<double, double> pick_factors(BagStatsRow const& row,
                                           uint32_t pick) const;

    double pick_q(BagStatsRow const& row, uint32_t pick, size_t k) const;

    static void compact(std::vector<Entry>* entries);

    std::vector<BagStatsCount const*> keys_;
    std::vector<double> ranks_;
    std::vector<Distribution> groups_;
};

// Each pick's P for PickRanker: 1 less the mid-rank percentile of its bag's
// prior among its list's bags, PAIRS bags then --pp bags; all 1 without
// `prior`.
std::vector<double> pick_ranks(std::vector<PairBag> const& pair_bags,
                               std::vector<PairBag> const& prospect_bags,
                               bool prior);

// The sort keys of `sorts`.
std::vector<BagStatsCount const*> sort_flag_keys(
    std::vector<BagStatsSortFlag const*> const& sorts);

// A ranked row, with where it came in the walk, its first pick's list index
// and its place among that pick's rows, to break ties in walk order.
struct RankedRow {
    std::pair<double, double> key;
    size_t unit;
    uint64_t seq;
    BagStatsRow row;
    // With --best-pairs, the picks the row recognizes, as RowWalker::picks()
    // numbers them.
    std::vector<uint32_t> best;
};

bool ranks_before(RankedRow const& a, RankedRow const& b);

// Where the --score row comes among all rows.
struct ScoreRank {
    // Rows -n would print before it.
    uint64_t before = 0;
    // Rows whose sort key is larger.
    uint64_t larger = 0;
    // Rows with its sort key, itself included.
    uint64_t tied = 0;
    uint64_t rows = 0;
};

// Adds `ranked` to `rank`, the ScoreRank of `target`.
inline void add_score_rank(RankedRow const& ranked, RankedRow const& target,
                           ScoreRank* rank) {
    ++rank->rows;
    rank->before += ranks_before(ranked, target);
    rank->larger += ranked.key > target.key;
    rank->tied += ranked.key == target.key;
}

// The list index of each --score pick of `score`, in walk step order, pb
// picks, then ppb picks, each kind in increasing order; `steps` is each
// step's list. False, diagnosed prefixed by `program`, when a pick isn't a
// bag of its list or the picks' bags don't fit `letters` together.
bool score_picks(char const* program, std::vector<std::string> const& score,
                 Search const& search,
                 std::vector<std::vector<PairBag> const*> const& steps,
                 PairsCounts const& letters, std::vector<size_t>* out);

// Appends `row` as a bag-stats row, or with --top-segments as a top-segments
// row.
void append_row(std::string* out, BagStatsRow const& row, bool top_segments);

// Prints the --score row and below it its rank line.
void print_score_rank(BagStatsRow const& row, ScoreRank const& rank);

// Prints --score-detail for the --score row `target`, `bags` its picks' bags
// in walk step order, `row_picks` its picks as PickRanker numbers them, and
// `sorts` the sort options.
void print_score_detail(std::vector<BagStatsSortFlag const*> const& sorts,
                        PickRanker const& ranker, RankedRow const& target,
                        std::vector<PairBag const*> const& bags,
                        std::span<uint32_t const> row_picks);

// Appends each of `best`, picks as RowWalker::picks() numbers them, that
// `seen` doesn't hold yet as PAIRS writes it, and adds it to `seen`.
void append_best_pairs(std::string* out, std::unordered_set<uint32_t>* seen,
                       std::vector<uint32_t> const& best,
                       std::vector<PairBag> const& pair_bags,
                       std::vector<PairBag> const& prospect_bags);

#endif
