// bag-stats.cpp - Rank combinations of pair letter bags by the exact word +
// pair completions they leave.

#include "bag-stats-impl.h"
#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "letter-bag.h"
#include "optparse.h"
#include "pairs-impl.h"
#include "row-input.h"
#include "workflow-paths.h"

#include "sub-bag-index.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr char PROGRAM[] = "bag-stats";
constexpr int INNER_MIN_WORD_LENGTH = 3;
constexpr int DEFAULT_MIN_WB_LENGTH = 5;
constexpr char DEFAULT_SEARCH[] = "pb,wb,p";
constexpr int OPT_MIN_WB_LENGTH = 256;
constexpr int OPT_SEARCH = 257;
constexpr int OPT_PROSPECTS = 258;
constexpr int OPT_THREADS = 259;
constexpr int OPT_BY_LENGTH = 260;
constexpr int OPT_SORT = 261;
constexpr int SORT_FLAG_COUNT =
    sizeof(BAG_STATS_SORT_FLAGS) / sizeof(BAG_STATS_SORT_FLAGS[0]);
// The most sub-bags PairCountTable numbers, 4 bytes each.
constexpr size_t PAIR_COUNT_TABLE_MAX = size_t(1) << 28;
// How many first picks past the last one printed a worker may start, per
// thread.
constexpr size_t PRINT_WINDOW_PER_THREAD = 16;

// How many picks of each kind a search sequence makes before its `p`.
struct Search {
    int prospects = 0;
    int pairs = 0;
    int words = 0;
};

struct Args {
    std::string letters;
    std::string used_letters;
    int min_word_bag_length = DEFAULT_MIN_WB_LENGTH;
    Search search;
    char const* prospects_file = NULL;
    int threads = 1;
    // -n; 0 prints every row.
    int top = 0;
    // One per sort option, NULL for the MIN_WB fraction.
    std::vector<BagStatsCount const*> keys;
    bool by_length = false;
    char const* pairs_file = NULL;
    char const* dictionary_file = NULL;
};

struct PairBag {
    PairsCounts counts;
    std::string pair;
    uint64_t pairs = 0;
};

void usage(char const* program, FILE* out) {
    fprintf(out,
            "usage: %s [-u LETTERS] [--min-wb-len N] [--ss STEPS] [--pp FILE]\n"
            "       [--threads N] [-n N SORT... [--by-length]]\n"
            "       LETTERS PAIRS DICT\n",
            program);
    if (out != stdout) return;

    fprintf(stdout,
          "  for each combination of pair letter bags that fits LETTERS, count\n"
          "  the combinations of DICT word bags that fit what is left, and the\n"
          "  exact pairs (pairs -m%d -x -c) of what is left after each; PAIRS,\n"
          "  DICT or the --pp FILE may be - for stdin\n"
          "\n"
          "  output: %s\n"
          "\noptions:\n", INNER_MIN_WORD_LENGTH, BAG_STATS_COLUMNS);
    dfs_help_used_letters();
    dfs_help_option("--min-wb-len N",
                    "MIN_WB counts the fitting DICT word bag combinations "
                    "whose every bag has at least N letters (default %d), "
                    "MIN_WB_WITH_PAIRS those of them whose remainder has at "
                    "least %d exact pairs; bag-stats-sort --min-wb-frac sorts "
                    "by their ratio",
                    DEFAULT_MIN_WB_LENGTH, int(BAG_STATS_MIN_WB_PAIRS));
    dfs_help_option("--ss STEPS",
                    "the search sequence, comma-separated: ppb picks a pair "
                    "bag from the --pp FILE, pb one from PAIRS, wb a word bag "
                    "from DICT, and p, last and once, counts the exact pairs "
                    "left; at least one pb and one wb; order doesn't matter; "
                    "each row names its ppb and pb picks (default %s)",
                    DEFAULT_SEARCH);
    dfs_help_option("--pp FILE",
                    "the prospective pairs ppb picks from, rows as in PAIRS");
    dfs_help_option("--threads N",
                    "count on N threads, at most the hardware threads, 0 for "
                    "all of them (default: 1)");
    dfs_help_option("-n N",
                    "print only the N rows that bag-stats-sort, given the "
                    "same SORT options and --by-length, would print first, in "
                    "that order; needs at least one SORT option");
    dfs_help_option("-h, --help", "show this help");
    fputs("\nSORT options, only with -n:\n", stdout);
    bag_stats_help_sort_flags();
    dfs_help_option("--by-length",
                    "rank each row's sort value only among the rows whose "
                    "pair picks have as many letters, as bag-stats-sort "
                    "--by-length does");
}

bool parse_search(char const* text, Search* out) {
    std::vector<std::string> steps(1);
    for (char const* c = text; *c; ++c) {
        if (*c == ',') steps.emplace_back();
        else steps.back() += *c;
    }
    if (steps.back() != "p") {
        fprintf(stderr, "%s: --ss has to end with p\n", PROGRAM);
        return false;
    }
    steps.pop_back();

    Search search;
    for (std::string const& step : steps) {
        if (step == "ppb") ++search.prospects;
        else if (step == "pb") ++search.pairs;
        else if (step == "wb") ++search.words;
        else {
            fprintf(stderr, "%s: --ss step \"%s\" isn't ppb, pb or wb\n",
                    PROGRAM, step.c_str());
            return false;
        }
    }
    if (search.pairs == 0 || search.words == 0) {
        fprintf(stderr, "%s: --ss needs at least one pb and one wb\n",
                PROGRAM);
        return false;
    }
    *out = search;
    return true;
}

// The steps of `search` in canonical order.
std::string search_text(Search const& search) {
    std::string out;
    for (int i = 0; i < search.prospects; ++i) out += "ppb,";
    for (int i = 0; i < search.pairs; ++i) out += "pb,";
    for (int i = 0; i < search.words; ++i) out += "wb,";
    return out + "p";
}

bool parse_args(char* argv[], Args* out, bool* help) {
    static struct optparse_long const long_options[] = {
        { "used-letters", 'u', OPTPARSE_REQUIRED },
        { "min-wb-len", OPT_MIN_WB_LENGTH, OPTPARSE_REQUIRED },
        { "ss", OPT_SEARCH, OPTPARSE_REQUIRED },
        { "pp", OPT_PROSPECTS, OPTPARSE_REQUIRED },
        { "threads", OPT_THREADS, OPTPARSE_REQUIRED },
        { NULL, 'n', OPTPARSE_REQUIRED },
        { "by-length", OPT_BY_LENGTH, OPTPARSE_NONE },
        { BAG_STATS_SORT_FLAGS[0].option, OPT_SORT + 0, OPTPARSE_NONE },
        { BAG_STATS_SORT_FLAGS[1].option, OPT_SORT + 1, OPTPARSE_NONE },
        { BAG_STATS_SORT_FLAGS[2].option, OPT_SORT + 2, OPTPARSE_NONE },
        { BAG_STATS_SORT_FLAGS[3].option, OPT_SORT + 3, OPTPARSE_NONE },
        { BAG_STATS_SORT_FLAGS[4].option, OPT_SORT + 4, OPTPARSE_NONE },
        { BAG_STATS_SORT_FLAGS[5].option, OPT_SORT + 5, OPTPARSE_NONE },
        { "help", 'h', OPTPARSE_NONE },
        { NULL, 0, OPTPARSE_NONE },
    };
    static_assert(SORT_FLAG_COUNT == 6);

    struct optparse options;
    optparse_init(&options, argv);

    char const* search = DEFAULT_SEARCH;
    int option;
    while ((option = optparse_long(&options, long_options, NULL)) != -1) {
        if (option >= OPT_SORT && option < OPT_SORT + SORT_FLAG_COUNT) {
            out->keys.push_back(
                bag_stats_sort_key(BAG_STATS_SORT_FLAGS[option - OPT_SORT]));
            continue;
        }
        switch (option) {
          case 'u':
            out->used_letters += options.optarg;
            break;
          case OPT_MIN_WB_LENGTH:
            if (!parse_count(options.optarg, "--min-wb-len",
                             &out->min_word_bag_length))
                return false;
            break;
          case OPT_SEARCH:
            search = options.optarg;
            break;
          case OPT_PROSPECTS:
            out->prospects_file = options.optarg;
            break;
          case OPT_THREADS:
            if (!parse_count(options.optarg, "--threads", &out->threads))
                return false;
            break;
          case 'n':
            if (!parse_count(options.optarg, "-n", &out->top)) return false;
            if (out->top == 0) {
                fprintf(stderr, "%s: -n needs N of at least 1\n", PROGRAM);
                return false;
            }
            break;
          case OPT_BY_LENGTH:
            out->by_length = true;
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
    out->pairs_file = optparse_arg(&options);
    out->dictionary_file = optparse_arg(&options);
    if (out->dictionary_file == NULL || optparse_arg(&options) != NULL)
        return false;
    out->letters = letters;
    if (!parse_search(search, &out->search)) return false;
    if ((out->top > 0) != !out->keys.empty() ||
        (out->by_length && out->top == 0)) {
        fprintf(stderr, "%s: -n goes with sort options, and they with it\n",
                PROGRAM);
        return false;
    }
    if ((out->search.prospects > 0) != (out->prospects_file != NULL)) {
        fprintf(stderr, "%s: --pp goes with a ppb step in --ss\n", PROGRAM);
        return false;
    }
    int const stdin_inputs = (strcmp(out->pairs_file, "-") == 0) +
        (strcmp(out->dictionary_file, "-") == 0) +
        (out->prospects_file != NULL && strcmp(out->prospects_file, "-") == 0);
    if (stdin_inputs > 1) {
        fprintf(stderr, "%s: only one of PAIRS, DICT and --pp can be -\n",
                PROGRAM);
        return false;
    }
    return true;
}

bool remaining_letters(Args const& args, std::string* out) {
    std::string bag;
    std::string remove;
    return clean_letters(args.letters.c_str(), "letters", &bag) &&
        clean_letters(args.used_letters.c_str(), "used letters", &remove) &&
        subtract_letters(bag, remove, out);
}

bool load_pair_bags(char const* path, PairsCounts const& bag,
                    std::vector<PairBag>* out) {
    std::unordered_set<std::string> rows;
    std::unordered_map<PairsCounts, size_t, PairsCountsHash> indices;
    return read_rows(PROGRAM, path, "pairs", false,
        [&](DfsPairRow const& row, std::string const&) {
            PairsCounts const counts =
                pairs_letter_counts((row.left + row.right).c_str());
            if (!pairs_fits(bag, counts)) return true;
            std::string const key = row.left < row.right
                ? row.left + "," + row.right : row.right + "," + row.left;
            if (!rows.insert(key).second) return true;
            auto const inserted = indices.emplace(counts, out->size());
            if (inserted.second) out->push_back({counts, row.left + " " + row.right});
            ++(*out)[inserted.first->second].pairs;
            return true;
        });
}

bool load_word_bags(char const* path, PairsCounts const& bag,
                    std::vector<PairsCounts>* out) {
    std::unordered_set<PairsCounts, PairsCountsHash> seen;
    return read_lines(PROGRAM, path,
        [&](std::string const& line, size_t) {
            PairsCounts const counts = pairs_letter_counts(line.c_str());
            if (counts == PairsCounts{} || !pairs_fits(bag, counts) ||
                !seen.insert(counts).second)
                return true;
            out->push_back(counts);
            return true;
        });
}

bool load_inner_dictionary(PairsExactCounter* out) {
    char const* const root = require_workflow_root(PROGRAM);
    if (root == NULL) return false;
    std::string const path =
        (std::filesystem::path(root) / WORKFLOW_DICT_PATH).string();
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
    return true;
}

int letter_total(PairsCounts const& counts) {
    int total = 0;
    for (uint8_t value : counts.values) total += value;
    return total;
}

// The exact pair counts of the sub-bags of R, shared by every thread and
// filled as each is first asked for. A slot holds its count plus one, 0
// while unknown. Sub-bags the index doesn't number are counted every time.
class PairCountTable {
  public:
    PairCountTable(SubBagIndex index, PairsExactCounter const& counter)
        : index_(std::move(index)), counter_(counter),
          counts_(static_cast<uint32_t*>(
              std::calloc(index_.total(), sizeof(uint32_t)))) {}

    bool allocated() const { return counts_ != nullptr; }

    // `groups`: the inner dictionary groups, holding every one that fits
    // `bag`.
    uint64_t count(PairsCounts const& bag,
                   PairsExactCounter::Groups const& groups) const {
        LetterCounts letters;
        for (int i = 0; i < 26; ++i) letters[i] = bag.values[i];
        size_t number;
        if (!index_.index_of(letters, &number))
            return counter_.count(bag, groups);
        std::atomic_ref<uint32_t> slot(counts_.get()[number]);
        uint32_t const known = slot.load(std::memory_order_relaxed);
        if (known != 0) return known - 1;
        uint64_t const count = counter_.count(bag, groups);
        if (count < UINT32_MAX)
            slot.store(uint32_t(count + 1), std::memory_order_relaxed);
        return count;
    }

  private:
    struct Free {
        void operator()(uint32_t* p) const { std::free(p); }
    };

    SubBagIndex index_;
    PairsExactCounter const& counter_;
    std::unique_ptr<uint32_t[], Free> counts_;
};

// The index for PairCountTable: the sub-bags of `letters` up to
// `max_length` letters, fewer when that numbers more than
// PAIR_COUNT_TABLE_MAX of them.
SubBagIndex pair_count_index(std::string letters, size_t max_length) {
    std::sort(letters.begin(), letters.end());
    while (true) {
        SubBagIndex index(letters, max_length);
        if (index.fits() && index.total() <= PAIR_COUNT_TABLE_MAX)
            return index;
        --max_length;
    }
}

// The longest leftover a row can ask a pair count for: R less the shortest
// bag of each pair step's list and the shortest word bag per word step.
size_t longest_leftover(
        PairsCounts const& bag,
        std::vector<std::vector<PairBag> const*> const& steps,
        std::vector<PairsCounts> const& word_bags, int words) {
    int left = letter_total(bag);
    for (std::vector<PairBag> const* step : steps) {
        int shortest = left;
        for (PairBag const& pair_bag : *step)
            shortest = std::min(shortest, letter_total(pair_bag.counts));
        left -= shortest;
    }
    int shortest = left;
    for (PairsCounts const& word_bag : word_bags)
        shortest = std::min(shortest, letter_total(word_bag));
    return size_t(std::max(0, left - words * shortest));
}

// Writes the bags of `from` that fit `rest` to `out`.
void filter_word_bags(std::vector<PairsCounts> const& from,
                      PairsCounts const& rest, std::vector<PairsCounts>* out) {
    out->clear();
    for (PairsCounts const& word_bag : from)
        if (pairs_fits(rest, word_bag)) out->push_back(word_bag);
}

// Fills a remainder's counts over every combination of one word bag per
// word step, word bags in non-decreasing order.
class RestCounter {
  public:
    RestCounter(int words, int min_word_bag_length,
                PairCountTable const& table)
        : words_(words), min_word_bag_length_(min_word_bag_length),
          table_(table) {}

    // `fitting`: the word bags that fit `rest`; `groups`: the inner
    // dictionary groups, holding every one that fits `rest`.
    void count(PairsCounts const& rest,
               std::vector<PairsCounts> const& fitting,
               PairsExactCounter::Groups const& groups, BagStatsRow* out) {
        fitting_ = &fitting;
        groups_ = &groups;
        visit(1, 0, rest, true, out);
    }

  private:
    void visit(int word, size_t start, PairsCounts const& rest, bool is_min,
               BagStatsRow* out) {
        PairsCounts word_rest;
        std::vector<PairsCounts> const& fitting = *fitting_;
        for (size_t i = start; i < fitting.size(); ++i) {
            PairsCounts const& word_bag = fitting[i];
            if (!pairs_subtract(rest, word_bag, &word_rest)) continue;
            bool const min = is_min &&
                letter_total(word_bag) >= min_word_bag_length_;
            if (word < words_) {
                visit(word + 1, i, word_rest, min, out);
                continue;
            }
            ++out->word_bags;
            out->min_word_bags += min;
            if (word_rest == PairsCounts{}) {
                ++out->solo;
                continue;
            }
            if (letter_total(word_rest) < 2 * INNER_MIN_WORD_LENGTH) continue;
            uint64_t const count = table_.count(word_rest, *groups_);
            if (count == 0) continue;
            ++out->with_pairs;
            out->total_pairs += count;
            out->max_pairs = std::max(out->max_pairs, count);
            out->min_with_pairs += min && count >= BAG_STATS_MIN_WB_PAIRS;
        }
    }

    int words_;
    int min_word_bag_length_;
    PairCountTable const& table_;
    std::vector<PairsCounts> const* fitting_ = nullptr;
    PairsExactCounter::Groups const* groups_ = nullptr;
};

// Walks the rows: every combination of one pick per pair step that fits,
// with picks from one list in increasing order and no bag picked twice.
// The pb steps come first, so a row's first pick, its unit, is a PAIRS bag.
// Each pick narrows the word bags and inner dictionary groups that fit what
// is left.
class RowWalker {
  public:
    RowWalker(std::vector<std::vector<PairBag> const*> const& steps,
              int pair_steps, std::vector<PairsCounts> const& word_bags,
              PairsExactCounter const& counter, RestCounter rest_counter)
        : steps_(steps), pair_steps_(size_t(pair_steps)),
          word_bags_(word_bags), counter_(counter), chosen_(steps.size()),
          fitting_(steps.size()), groups_(steps.size()),
          rest_counter_(std::move(rest_counter)) {}

    // Calls visit(row, pair_length) for each row whose first pick is
    // `unit`, in order, with the row's counts but not its PAIR or PAIRS.
    template <typename Visit>
    void walk(size_t unit, PairsCounts const& bag, Visit const& visit) {
        PairsCounts rest;
        if (!pairs_subtract(bag, (*steps_[0])[unit].counts, &rest)) return;
        chosen_[0] = unit;
        filter_word_bags(word_bags_, rest, &fitting_[0]);
        counter_.filter(counter_.groups(), rest, &groups_[0]);
        descend(1, rest, letter_total(bag), visit);
    }

    // The current row's PAIR: its ppb picks, then its pb picks.
    std::string pair() const {
        std::string out;
        for (size_t k = 0; k < steps_.size(); ++k) {
            size_t const j = (k + pair_steps_) % steps_.size();
            if (k > 0) out += " + ";
            out += (*steps_[j])[chosen_[j]].pair;
        }
        return out;
    }

    // The current row's PAIRS.
    uint64_t pairs() const {
        uint64_t out = 1;
        for (size_t j = 0; j < steps_.size(); ++j)
            out *= (*steps_[j])[chosen_[j]].pairs;
        return out;
    }

  private:
    template <typename Visit>
    void descend(size_t step, PairsCounts const& rest, int letters,
                 Visit const& visit) {
        if (step == steps_.size()) {
            BagStatsRow row{};
            rest_counter_.count(rest, fitting_[step - 1], groups_[step - 1],
                                &row);
            visit(row, size_t(letters - letter_total(rest)));
            return;
        }
        std::vector<PairBag> const& bags = *steps_[step];
        size_t const start = steps_[step - 1] == steps_[step]
            ? chosen_[step - 1] + 1 : 0;
        PairsCounts next;
        for (size_t i = start; i < bags.size(); ++i) {
            if (!pairs_subtract(rest, bags[i].counts, &next) ||
                repeats_bag(step, bags[i].counts))
                continue;
            chosen_[step] = i;
            filter_word_bags(fitting_[step - 1], next, &fitting_[step]);
            counter_.filter(groups_[step - 1], next, &groups_[step]);
            descend(step + 1, next, letters, visit);
        }
    }

    bool repeats_bag(size_t step, PairsCounts const& counts) const {
        for (size_t j = 0; j < step; ++j)
            if (steps_[j] != steps_[step] &&
                (*steps_[j])[chosen_[j]].counts == counts)
                return true;
        return false;
    }

    std::vector<std::vector<PairBag> const*> const& steps_;
    size_t pair_steps_;
    std::vector<PairsCounts> const& word_bags_;
    PairsExactCounter const& counter_;
    std::vector<size_t> chosen_;
    // The word bags and inner dictionary groups that fit what is left after
    // each pick.
    std::vector<std::vector<PairsCounts>> fitting_;
    std::vector<PairsExactCounter::Groups> groups_;
    RestCounter rest_counter_;
};

struct LengthValueHash {
    size_t operator()(std::pair<size_t, double> const& key) const {
        return std::hash<size_t>()(key.first) * 31 +
            std::hash<double>()(key.second);
    }
};

// How many rows have each (length group, sort value), one map per sort key.
using SortValueCounts = std::vector<
    std::unordered_map<std::pair<size_t, double>, uint64_t, LengthValueHash>>;

// A sorted distribution of values, each with how many rows are below it and
// at it.
class Distribution {
  public:
    // From `counts`, each value once, with how many rows have it.
    explicit Distribution(std::vector<std::pair<double, uint64_t>> counts) {
        std::sort(counts.begin(), counts.end());
        for (auto const& [value, count] : counts) {
            values_.push_back(value);
            below_.push_back(size_);
            at_.push_back(count);
            size_ += count;
        }
    }

    // The mid-rank percentile of `value`, one of the values.
    double percentile(double value) const {
        size_t const i = size_t(
            std::lower_bound(values_.begin(), values_.end(), value) -
            values_.begin());
        return bag_stats_percentile(below_[i], below_[i] + at_[i], size_);
    }

  private:
    std::vector<double> values_;
    std::vector<uint64_t> below_;
    std::vector<uint64_t> at_;
    uint64_t size_ = 0;
};

// Ranks rows as bag-stats-sort ranks them, from how many rows have each sort
// value rather than from the rows themselves.
class Ranker {
  public:
    Ranker(std::vector<BagStatsCount const*> keys, bool by_length)
        : keys_(std::move(keys)), by_length_(by_length) {}

    // Whether key() needs finish() first: a row's rank then depends on the
    // other rows.
    bool needs_counts() const { return by_length_ || keys_.size() > 1; }

    SortValueCounts counts() const { return SortValueCounts(keys_.size()); }

    void add(SortValueCounts* counts, BagStatsRow const& row,
             size_t length) const {
        for (size_t k = 0; k < keys_.size(); ++k)
            ++(*counts)[k][{group(length),
                            bag_stats_sort_value(keys_[k], row)}];
    }

    void finish(std::vector<SortValueCounts> const& parts) {
        for (size_t k = 0; k < keys_.size(); ++k) {
            std::unordered_map<size_t,
                std::vector<std::pair<double, uint64_t>>> by_group;
            std::unordered_map<std::pair<size_t, double>, uint64_t,
                               LengthValueHash> merged;
            for (SortValueCounts const& part : parts)
                for (auto const& [key, count] : part[k]) merged[key] += count;
            for (auto const& [key, count] : merged)
                by_group[key.first].emplace_back(key.second, count);
            groups_.emplace_back();
            for (auto& [length, counts] : by_group)
                groups_.back().emplace(length,
                                       Distribution(std::move(counts)));
            if (keys_.size() == 1) continue;

            std::unordered_map<double, uint64_t> ranked;
            for (auto const& [key, count] : merged)
                ranked[first_rank(k, key.first, key.second)] += count;
            overall_.emplace_back(std::vector<std::pair<double, uint64_t>>(
                ranked.begin(), ranked.end()));
        }
    }

    // The row's sort key, larger first, as bag-stats-sort's sort_keys().
    std::pair<double, double> key(BagStatsRow const& row,
                                  size_t length) const {
        std::pair<double, double> out{-1, 0};
        for (size_t k = 0; k < keys_.size(); ++k) {
            double value = first_rank(k, group(length),
                                      bag_stats_sort_value(keys_[k], row));
            if (keys_.size() > 1) value = overall_[k].percentile(value);
            out.first = std::max(out.first, value);
            out.second += value;
        }
        return out;
    }

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

// A row kept for -n, with where it came in the walk to break ties as
// bag-stats-sort's stable sort does.
struct RankedRow {
    std::pair<double, double> key;
    size_t unit;
    uint64_t seq;
    BagStatsRow row;
};

bool ranks_before(RankedRow const& a, RankedRow const& b) {
    if (a.key != b.key) return a.key > b.key;
    if (a.unit != b.unit) return a.unit < b.unit;
    return a.seq < b.seq;
}

// Runs work(thread) on `threads` threads, the calling thread 0.
template <typename Work>
void run_threads(size_t threads, Work const& work) {
    std::vector<std::thread> workers;
    for (size_t t = 1; t < threads; ++t) workers.emplace_back(work, t);
    work(size_t(0));
    for (std::thread& worker : workers) worker.join();
}

// What every pass shares.
struct Walk {
    std::vector<std::vector<PairBag> const*> const& steps;
    PairsCounts const& bag;
    std::vector<PairsCounts> const& word_bags;
    Args const& args;
    PairsExactCounter const& counter;
    PairCountTable const& table;
    size_t threads;

    RowWalker walker() const {
        return RowWalker(steps, args.search.pairs, word_bags, counter,
                         RestCounter(args.search.words,
                                     args.min_word_bag_length, table));
    }

    size_t units() const { return steps[0]->size(); }
};

// Prints every row, in walk order, while later units are counted.
void print_rows(Walk const& walk) {
    size_t const units = walk.units();
    size_t const window = PRINT_WINDOW_PER_THREAD * walk.threads;
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> lines(units);
    std::vector<char> done(units, 0);
    size_t next = 0;
    size_t printed = 0;

    auto const work = [&](size_t) {
        RowWalker walker = walk.walker();
        std::string buffer;
        while (true) {
            size_t unit;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [&] {
                    return next >= units || next < printed + window;
                });
                if (next >= units) return;
                unit = next++;
            }
            buffer.clear();
            walker.walk(unit, walk.bag, [&](BagStatsRow& row, size_t) {
                row.pair = walker.pair();
                row.pairs = walker.pairs();
                append_bag_stats_row(&buffer, row);
            });
            {
                std::lock_guard<std::mutex> lock(mutex);
                lines[unit] = std::move(buffer);
                done[unit] = 1;
            }
            changed.notify_all();
        }
    };
    std::vector<std::thread> workers;
    for (size_t t = 0; t < walk.threads; ++t) workers.emplace_back(work, t);
    for (size_t unit = 0; unit < units; ++unit) {
        std::string line;
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [&] { return done[unit] != 0; });
            line = std::move(lines[unit]);
        }
        fwrite(line.data(), 1, line.size(), stdout);
        {
            std::lock_guard<std::mutex> lock(mutex);
            printed = unit + 1;
        }
        changed.notify_all();
    }
    for (std::thread& worker : workers) worker.join();
}

// Fills `ranker` with how many rows have each sort value.
void count_sort_values(Walk const& walk, Ranker* ranker) {
    std::atomic<size_t> next{0};
    std::vector<SortValueCounts> parts(walk.threads, ranker->counts());
    run_threads(walk.threads, [&](size_t t) {
        RowWalker walker = walk.walker();
        for (size_t unit; (unit = next.fetch_add(1)) < walk.units();)
            walker.walk(unit, walk.bag,
                        [&](BagStatsRow const& row, size_t length) {
                ranker->add(&parts[t], row, length);
            });
    });
    ranker->finish(parts);
}

// The `top` rows that rank first, in order.
std::vector<RankedRow> top_rows(Walk const& walk, Ranker const& ranker,
                                size_t top) {
    std::atomic<size_t> next{0};
    std::vector<std::vector<RankedRow>> heaps(walk.threads);
    run_threads(walk.threads, [&](size_t t) {
        RowWalker walker = walk.walker();
        std::vector<RankedRow>& heap = heaps[t];
        for (size_t unit; (unit = next.fetch_add(1)) < walk.units();) {
            uint64_t seq = 0;
            walker.walk(unit, walk.bag, [&](BagStatsRow& row, size_t length) {
                RankedRow ranked{ranker.key(row, length), unit, seq++, {}};
                if (heap.size() == top && !ranks_before(ranked, heap.front()))
                    return;
                row.pair = walker.pair();
                row.pairs = walker.pairs();
                ranked.row = std::move(row);
                if (heap.size() == top) {
                    std::pop_heap(heap.begin(), heap.end(), ranks_before);
                    heap.pop_back();
                }
                heap.push_back(std::move(ranked));
                std::push_heap(heap.begin(), heap.end(), ranks_before);
            });
        }
    });
    std::vector<RankedRow> out;
    for (std::vector<RankedRow>& heap : heaps)
        std::move(heap.begin(), heap.end(), std::back_inserter(out));
    std::sort(out.begin(), out.end(), ranks_before);
    if (out.size() > top) out.resize(top);
    return out;
}

}  // namespace

int main(int argc, char* argv[]) {
    Args args;
    bool help = false;
    std::string letters;
    if (!parse_args(argv, &args, &help) ||
        (!help && !remaining_letters(args, &letters))) {
        usage(argv[0], stderr);
        return 2;
    }
    if (help) {
        usage(argv[0], stdout);
        return 0;
    }

    PairsCounts const bag = pairs_letter_counts(letters.c_str());
    std::vector<PairBag> prospect_bags;
    std::vector<PairBag> pair_bags;
    std::vector<PairsCounts> word_bags;
    PairsExactCounter counter(bag, INNER_MIN_WORD_LENGTH);
    if ((args.prospects_file != NULL &&
         !load_pair_bags(args.prospects_file, bag, &prospect_bags)) ||
        !load_pair_bags(args.pairs_file, bag, &pair_bags) ||
        !load_word_bags(args.dictionary_file, bag, &word_bags) ||
        !load_inner_dictionary(&counter))
        return 1;

    std::vector<std::vector<PairBag> const*> steps;
    steps.insert(steps.end(), args.search.pairs, &pair_bags);
    steps.insert(steps.end(), args.search.prospects, &prospect_bags);
    PairCountTable const table(
        pair_count_index(letters, longest_leftover(bag, steps, word_bags,
                                                   args.search.words)),
        counter);
    if (!table.allocated()) {
        fprintf(stderr, "%s: can't allocate the pair count table\n", PROGRAM);
        return 1;
    }

    size_t threads = resolve_search_threads(args.threads);
    unsigned int const cores = std::thread::hardware_concurrency();
    if (cores != 0) threads = std::min(threads, size_t(cores));
    Walk const walk{steps, bag, word_bags, args, counter, table, threads};

    setvbuf(stdout, nullptr, _IOFBF, 1 << 22);
    write_bag_stats_header(stdout, {letters, INNER_MIN_WORD_LENGTH,
                                    args.min_word_bag_length,
                                    search_text(args.search),
                                    args.prospects_file == NULL
                                        ? "" : args.prospects_file,
                                    args.pairs_file, args.dictionary_file});
    if (args.top == 0) {
        print_rows(walk);
        return 0;
    }
    Ranker ranker(args.keys, args.by_length);
    if (ranker.needs_counts()) count_sort_values(walk, &ranker);
    for (RankedRow const& ranked : top_rows(walk, ranker, size_t(args.top)))
        write_bag_stats_row(stdout, ranked.row);
    return 0;
}
