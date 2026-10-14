// bag-stats.cpp - Rank combinations of pair letter bags by the exact word +
// pair completions they leave.

#include "bag-stats-impl.h"
#include "bag-stats-rank.h"
#include "classified.h"
#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "letter-bag.h"
#include "log.h"
#include "optparse.h"
#include "pairs-impl.h"
#include "row-input.h"
#include "workflow-paths.h"

#include "sub-bag-index.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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
constexpr int OPT_PROGRESS = 260;
constexpr int DEFAULT_PROGRESS_SECONDS = 60;
// The most sub-bags PairCountTable numbers, 4 bytes each.
constexpr size_t PAIR_COUNT_TABLE_MAX = size_t(1) << 28;
// How many first picks past the last one printed a worker may start, per
// thread.
constexpr size_t PRINT_WINDOW_PER_THREAD = 16;

struct Args {
    std::string letters;
    std::string used_letters;
    int sentence = CLASSIFIED_NO_SENTENCE;
    int min_word_bag_length = DEFAULT_MIN_WB_LENGTH;
    Search search;
    char const* prospects_file = NULL;
    int threads = 1;
    int progress = DEFAULT_PROGRESS_SECONDS;
    RankArgs rank;
    char const* pairs_file = NULL;
    char const* dictionary_file = NULL;
};

void usage(char const* program, FILE* out) {
    fprintf(out,
            "usage: %s [-u LETTERS] [-s N] [--min-wb-len N] [--ss STEPS] [--pp FILE]\n"
            "       [--threads N] [--progress N]\n"
            "       [-n N SORT... [--by-length | --by-pick]]\n"
            "       [--score W1,W2... [--score-detail] SORT...\n"
            "        [--by-length | --by-pick]]\n"
            "       [--best-pairs appear|lead]\n"
            "       [--prior FILE | [--p1 INDEX] [--clues CLUEDATA]\n"
            "        [--prior-weights SPEC]] [--top-segments]\n"
            "       LETTERS PAIRS DICT\n",
            program);
    if (out != stdout) return;

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
                    "the prospective pairs ppb picks from, rows as in PAIRS; "
                    "their bags are dropped from PAIRS");
    dfs_help_option("--threads N",
                    "count on N threads, at most the hardware threads, 0 for "
                    "all of them (default: 1)");
    dfs_help_option("--progress N",
                    "report each pass's progress to stderr every N seconds, "
                    "and its time when it took that long; 0 for never "
                    "(default: %d)", DEFAULT_PROGRESS_SECONDS);
    dfs_help_option("-n N",
                    "print only the N rows that rank first, in that order, "
                    "as bag-stats-sort ranks this run's full dump with the "
                    "same options; needs at least one SORT option");
    help_rank_output_options();
    dfs_help_option("-h, --help", "show this help");
    fputs("\nSORT options, only with -n or --score:\n", stdout);
    help_rank_sort_options();
}

bool parse_args(char* argv[], Args* out, bool* help) {
    static struct optparse_long const long_options[] = {
        { "used-letters", 'u', OPTPARSE_REQUIRED },
        CLASSIFIED_SENTENCE_LONG_OPTION,
        { "min-wb-len", OPT_MIN_WB_LENGTH, OPTPARSE_REQUIRED },
        { "ss", OPT_SEARCH, OPTPARSE_REQUIRED },
        { "pp", OPT_PROSPECTS, OPTPARSE_REQUIRED },
        { "threads", OPT_THREADS, OPTPARSE_REQUIRED },
        { "progress", OPT_PROGRESS, OPTPARSE_REQUIRED },
        BAG_STATS_RANK_LONG_OPTIONS,
        { "help", 'h', OPTPARSE_NONE },
        { NULL, 0, OPTPARSE_NONE },
    };

    struct optparse options;
    optparse_init(&options, argv);

    char const* search = DEFAULT_SEARCH;
    int option;
    while ((option = optparse_long(&options, long_options, NULL)) != -1) {
        RankOption const rank =
            parse_rank_option(PROGRAM, option, options.optarg, &out->rank);
        if (rank == RankOption::BAD) return false;
        if (rank == RankOption::PARSED) continue;
        switch (option) {
          case 'u':
            out->used_letters += options.optarg;
            break;
          case 's':
            if (!parse_classified_sentence(options.optarg, &out->sentence))
                return false;
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
          case OPT_PROGRESS:
            if (!parse_count(options.optarg, "--progress", &out->progress))
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
    out->pairs_file = optparse_arg(&options);
    out->dictionary_file = optparse_arg(&options);
    if (out->dictionary_file == NULL || optparse_arg(&options) != NULL)
        return false;
    out->letters = letters;
    if (!parse_search(PROGRAM, search, &out->search)) return false;
    if (out->search.pairs == 0) {
        fprintf(stderr, "%s: --ss needs at least one pb\n", PROGRAM);
        return false;
    }
    RankArgs const& rank = out->rank;
    bool const ranked = rank.top > 0 || !rank.score.empty();
    if (ranked != !rank.sorts.empty() ||
        ((rank.by_length || rank.by_pick) && !ranked)) {
        fprintf(stderr, "%s: -n or --score goes with sort options, and they, "
                "--by-length and --by-pick with it\n", PROGRAM);
        return false;
    }
    if (!check_rank_args(PROGRAM, rank, out->search)) return false;
    if ((out->search.prospects > 0) != (out->prospects_file != NULL)) {
        fprintf(stderr, "%s: --pp goes with a ppb step in --ss\n", PROGRAM);
        return false;
    }
    int const stdin_inputs = (strcmp(out->pairs_file, "-") == 0) +
        (strcmp(out->dictionary_file, "-") == 0) +
        (out->prospects_file != NULL && strcmp(out->prospects_file, "-") == 0) +
        (rank.prior_file != NULL && strcmp(rank.prior_file, "-") == 0);
    if (stdin_inputs > 1) {
        fprintf(stderr, "%s: only one of PAIRS, DICT, --pp and --prior can "
                "be -\n", PROGRAM);
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

// A full mixed-radix index makes a remainder's number cheap to compute.
// Unlike SubBagIndex it does not restrict the length of the sub-bag.
class RemainderIndex {
  public:
    explicit RemainderIndex(PairsCounts const& bag) {
        for (size_t i = 0; i < 26; ++i) {
            if (bag.values[i] == 0) continue;
            if (total_ > PAIR_COUNT_TABLE_MAX / (bag.values[i] + 1)) {
                total_ = 0;
                break;
            }
            digits_.push_back({i, total_});
            total_ *= bag.values[i] + 1;
        }
    }

    size_t total() const { return total_; }
    size_t number(PairsCounts const& bag) const {
        size_t out = 0;
        for (auto const& [letter, stride] : digits_)
            out += bag.values[letter] * stride;
        return out;
    }

  private:
    size_t total_ = 1;
    std::vector<std::pair<size_t, size_t>> digits_;
};

// Exact pair counts shared by every thread. Large searches fill a full
// table once by enumerating dictionary group pairs. Otherwise slots hold
// count + 1, or 0 while unknown, and are filled on demand; sub-bags outside
// that bounded-length index are counted every time.
class PairCountTable {
  public:
    PairCountTable(SubBagIndex index, PairsCounts const& bag,
                   PairsExactCounter const& counter, bool prepare,
                   size_t threads)
        : index_(std::move(index)), full_(bag), counter_(counter),
          prepared_(prepare && full_.total() != 0),
          counts_(static_cast<uint32_t*>(
              std::calloc(prepared_ ? full_.total() : index_.total(),
                          sizeof(uint32_t)))) {
        if (!prepared_ || !allocated()) return;
        // Enumerate dictionary group pairs once, accumulating all exact
        // counts. Their mixed-radix numbers add when their letters fit.
        auto const& keys = counter.groups().keys;
        std::vector<size_t> numbers;
        for (PairsCounts const& key : keys)
            numbers.push_back(full_.number(key));
        std::atomic<size_t> next{0};
        auto const work = [&] {
            PairsCounts remaining;
            for (size_t a; (a = next.fetch_add(1)) < keys.size();) {
                pairs_subtract(bag, keys[a], &remaining);
                uint64_t const size = counter.group_size(a);
                for (size_t b = a; b < keys.size(); ++b) {
                    if (!pairs_fits(remaining, keys[b])) continue;
                    uint64_t const count = a == b ? size * (size - 1) / 2
                        : size * counter.group_size(b);
                    if (count == 0) continue;
                    std::atomic_ref<uint32_t> slot(
                        counts_[numbers[a] + numbers[b]]);
                    uint32_t old = slot.load(std::memory_order_relaxed);
                    while (old != UINT32_MAX &&
                           !slot.compare_exchange_weak(old,
                               uint32_t(std::min<uint64_t>(UINT32_MAX,
                                                          old + count)),
                               std::memory_order_relaxed)) {}
                }
            }
        };
        std::vector<std::thread> workers;
        for (size_t t = 1; t < threads; ++t) workers.emplace_back(work);
        work();
        for (auto& worker : workers) worker.join();
    }

    bool allocated() const { return counts_ != nullptr; }
    bool needs_groups() const { return !prepared_; }

    // `groups`: the inner dictionary groups, holding every one that fits
    // `bag`.
    uint64_t count(PairsCounts const& bag,
                   PairsExactCounter::Groups const& groups) const {
        if (prepared_) {
            uint32_t const count = counts_[full_.number(bag)];
            return count == UINT32_MAX ? counter_.count(bag) : count;
        }
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
    RemainderIndex full_;
    PairsExactCounter const& counter_;
    bool prepared_;
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

// Statistics depend only on the remainder, not on which pair picks made
// it. Keep immutable records shared across workers and ranking passes.
// Together with the pair table, these allocations stay below 4 GiB.
class RestTable {
  public:
    RestTable(PairsCounts const& bag, bool enabled) : index_(bag) {
        if (!enabled || index_.total() == 0) return;
        capacity_ = std::min(index_.total(), size_t(1) << 25);
        slots_.reset(static_cast<uint32_t*>(
            std::calloc(index_.total(), sizeof(uint32_t))));
        rows_.reset(static_cast<BagStatsCounts*>(
            std::calloc(capacity_, sizeof(BagStatsCounts))));
        if (!slots_ || !rows_) {
            slots_.reset();
            rows_.reset();
        }
    }

    bool get(PairsCounts const& rest, BagStatsCounts* out) const {
        if (!slots_) return false;
        uint32_t const slot = std::atomic_ref<uint32_t>(
            slots_[index_.number(rest)]).load(std::memory_order_acquire);
        if (slot == 0 || slot == UINT32_MAX) return false;
        *out = rows_[slot - 1];
        return true;
    }

    void put(PairsCounts const& rest, BagStatsCounts const& counts) const {
        if (!slots_) return;
        std::atomic_ref<uint32_t> slot(slots_[index_.number(rest)]);
        uint32_t expected = 0;
        if (!slot.compare_exchange_strong(expected, UINT32_MAX,
                                           std::memory_order_relaxed))
            return;
        size_t const i = next_.fetch_add(1, std::memory_order_relaxed);
        if (i >= capacity_) return;
        rows_[i] = counts;
        slot.store(uint32_t(i + 1), std::memory_order_release);
    }

  private:
    struct Free {
        template <typename T> void operator()(T* p) const { std::free(p); }
    };
    RemainderIndex index_;
    size_t capacity_ = 0;
    std::unique_ptr<uint32_t[], Free> slots_;
    std::unique_ptr<BagStatsCounts[], Free> rows_;
    mutable std::atomic<size_t> next_{0};
};

// Fills a remainder's counts over every combination of one word bag per
// word step, word bags in non-decreasing order.
class RestCounter {
  public:
    RestCounter(int words, int min_word_bag_length,
                PairCountTable const& table)
        : words_(words), min_word_bag_length_(min_word_bag_length),
          table_(table) {}

    bool needs_groups() const { return table_.needs_groups(); }

    // `fitting`: the word bags that fit `rest`; `groups`: the inner
    // dictionary groups, holding every one that fits `rest`.
    void count(PairsCounts const& rest,
               std::vector<PairsCounts> const& fitting,
               PairsExactCounter::Groups const& groups,
               BagStatsCounts* out) {
        fitting_ = &fitting;
        groups_ = &groups;
        visit(1, 0, rest, true, out);
    }

  private:
    void visit(int word, size_t start, PairsCounts const& rest, bool is_min,
               BagStatsCounts* out) {
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
              PairsExactCounter const& counter, RestCounter rest_counter,
              RestTable const& rest_table)
        : steps_(steps), pair_steps_(size_t(pair_steps)),
          word_bags_(word_bags), counter_(counter), chosen_(steps.size()),
          fitting_(steps.size()), groups_(steps.size()),
          rest_counter_(std::move(rest_counter)), rest_table_(rest_table) {}

    // Calls visit(row, pair_length) for each row whose first pick is
    // `unit`, in order, with the row's counts but not its PAIR or PAIRS.
    template <typename Visit>
    void walk(size_t unit, PairsCounts const& bag, Visit const& visit) {
        PairsCounts rest;
        if (!pairs_subtract(bag, (*steps_[0])[unit].counts, &rest)) return;
        chosen_[0] = unit;
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

    // Whether the current row's picks, one list index per pair step, are
    // `picks`.
    bool at(std::vector<size_t> const& picks) const {
        return chosen_ == picks;
    }

    // The current row's picks, one per pair step: a PAIRS bag's list index,
    // or a --pp bag's past every PAIRS bag.
    void picks(std::vector<uint32_t>* out) const {
        out->resize(steps_.size());
        for (size_t j = 0; j < steps_.size(); ++j)
            (*out)[j] = uint32_t(chosen_[j] +
                                 (j < pair_steps_ ? 0 : steps_[0]->size()));
    }

    // The RestTable lookups and hits since the last call, as {hits,
    // lookups}.
    std::pair<uint64_t, uint64_t> take_rest_counts() {
        std::pair<uint64_t, uint64_t> const out{rest_hits_, rest_lookups_};
        rest_hits_ = rest_lookups_ = 0;
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
            ++rest_lookups_;
            if (rest_table_.get(rest, &row)) {
                ++rest_hits_;
            } else {
                filter(step - 1, rest);
                rest_counter_.count(rest, fitting_[step - 1],
                                    groups_[step - 1], &row);
                rest_table_.put(rest, row);
            }
            visit(row, size_t(letters - letter_total(rest)));
            return;
        }
        filter(step - 1, rest);
        std::vector<PairBag> const& bags = *steps_[step];
        size_t const start = steps_[step - 1] == steps_[step]
            ? chosen_[step - 1] + 1 : 0;
        PairsCounts next;
        for (size_t i = start; i < bags.size(); ++i) {
            if (!pairs_subtract(rest, bags[i].counts, &next) ||
                repeats_bag(step, bags[i].counts))
                continue;
            chosen_[step] = i;
            descend(step + 1, next, letters, visit);
        }
    }

    void filter(size_t step, PairsCounts const& rest) {
        filter_word_bags(step == 0 ? word_bags_ : fitting_[step - 1],
                          rest, &fitting_[step]);
        if (rest_counter_.needs_groups())
            counter_.filter(step == 0 ? counter_.groups() : groups_[step - 1],
                             rest, &groups_[step]);
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
    RestTable const& rest_table_;
    uint64_t rest_lookups_ = 0;
    uint64_t rest_hits_ = 0;
};

// Runs work(thread) on `threads` threads, the calling thread 0.
template <typename Work>
void run_threads(size_t threads, Work const& work) {
    std::vector<std::thread> workers;
    for (size_t t = 1; t < threads; ++t) workers.emplace_back(work, t);
    work(size_t(0));
    for (std::thread& worker : workers) worker.join();
}

// "1h02m", "34m10s" or "12s".
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

// Reports a pass's finished units to stderr every `seconds`, 0 for never,
// and when it ends, its time if it took that long.
class Progress {
  public:
    using Clock = std::chrono::steady_clock;

    Progress(int seconds, int pass, int passes, size_t units)
        : interval_(std::chrono::seconds(seconds)), pass_(pass),
          passes_(passes), units_(units), start_(Clock::now()),
          next_report_(seconds > 0 ? (start_ + interval_).time_since_epoch()
                                         .count()
                                   : INT64_MAX) {}

    ~Progress() {
        double const elapsed = seconds_since_start(Clock::now());
        if (interval_.count() == 0 || elapsed < interval_.count()) return;
        fprintf(stderr, "%s: pass %d/%d: done in %s%s\n", PROGRAM, pass_,
                passes_, format_duration(elapsed).c_str(),
                cache_text().c_str());
    }

    // Counts one finished unit, and the cache lookups `walker` made since
    // its last one.
    void finished(RowWalker* walker) {
        auto const [hits, lookups] = walker->take_rest_counts();
        cache_hits_.fetch_add(hits, std::memory_order_relaxed);
        cache_lookups_.fetch_add(lookups, std::memory_order_relaxed);
        size_t const done = done_.fetch_add(1, std::memory_order_relaxed) + 1;
        Clock::time_point const now = Clock::now();
        int64_t due = next_report_.load(std::memory_order_relaxed);
        if (now.time_since_epoch().count() < due ||
            !next_report_.compare_exchange_strong(
                due, (now + interval_).time_since_epoch().count(),
                std::memory_order_relaxed))
            return;
        double const elapsed = seconds_since_start(now);
        fprintf(stderr, "%s: pass %d/%d: %zu/%zu pair bags (%.1f%%), %s, "
                "~%s left%s\n", PROGRAM, pass_, passes_, done, units_,
                100.0 * double(done) / double(units_),
                format_duration(elapsed).c_str(),
                format_duration(elapsed * double(units_ - done) /
                                double(done)).c_str(),
                cache_text().c_str());
    }

  private:
    // ", cache hits N/M (P%)" on the second pass, else empty.
    std::string cache_text() const {
        if (pass_ != 2) return "";
        uint64_t const hits = cache_hits_.load(std::memory_order_relaxed);
        uint64_t const lookups =
            cache_lookups_.load(std::memory_order_relaxed);
        char out[96];
        snprintf(out, sizeof out, ", cache hits %llu/%llu (%.1f%%)",
                 static_cast<unsigned long long>(hits),
                 static_cast<unsigned long long>(lookups),
                 lookups == 0 ? 0.0 : 100.0 * double(hits) / double(lookups));
        return out;
    }

    double seconds_since_start(Clock::time_point now) const {
        return std::chrono::duration<double>(now - start_).count();
    }

    Clock::duration const interval_;
    int const pass_;
    int const passes_;
    size_t const units_;
    Clock::time_point const start_;
    std::atomic<size_t> done_{0};
    std::atomic<uint64_t> cache_hits_{0};
    std::atomic<uint64_t> cache_lookups_{0};
    std::atomic<int64_t> next_report_;
};

// What every pass shares.
struct Walk {
    std::vector<std::vector<PairBag> const*> const& steps;
    std::vector<PairBag> const& prospect_bags;
    PairsCounts const& bag;
    std::vector<PairsCounts> const& word_bags;
    Args const& args;
    PairsExactCounter const& counter;
    PairCountTable const& table;
    RestTable const& rest_table;
    size_t threads;
    // How many passes walk the rows: 2 when ranking needs the sort value
    // counts first.
    int passes;

    RowWalker walker() const {
        return RowWalker(steps, args.search.pairs, word_bags, counter,
                         RestCounter(args.search.words,
                                     args.min_word_bag_length, table),
                         rest_table);
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
    Progress progress(walk.args.progress, 1, walk.passes, units);

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
                append_row(&buffer, row, walk.args.rank.top_segments);
            });
            {
                std::lock_guard<std::mutex> lock(mutex);
                lines[unit] = std::move(buffer);
                done[unit] = 1;
            }
            changed.notify_all();
            progress.finished(&walker);
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
template <typename R>
void count_sort_values(Walk const& walk, R* ranker) {
    std::atomic<size_t> next{0};
    std::vector<typename R::Part> parts(walk.threads, ranker->part());
    {
        Progress progress(walk.args.progress, 1, walk.passes, walk.units());
        run_threads(walk.threads, [&](size_t t) {
            RowWalker walker = walk.walker();
            std::vector<uint32_t> picks;
            for (size_t unit; (unit = next.fetch_add(1)) < walk.units();) {
                walker.walk(unit, walk.bag,
                            [&](BagStatsRow const& row, size_t length) {
                    if constexpr (R::NEEDS_PICKS) walker.picks(&picks);
                    ranker->add(&parts[t], row, length, picks);
                });
                progress.finished(&walker);
            }
        });
    }
    ranker->finish(parts);
}

// The `top` rows that rank first, in order.
template <typename R>
std::vector<RankedRow> top_rows(Walk const& walk, R const& ranker,
                                size_t top) {
    std::atomic<size_t> next{0};
    std::vector<std::vector<RankedRow>> heaps(walk.threads);
    Progress progress(walk.args.progress, walk.passes, walk.passes,
                      walk.units());
    run_threads(walk.threads, [&](size_t t) {
        RowWalker walker = walk.walker();
        std::vector<RankedRow>& heap = heaps[t];
        std::vector<uint32_t> picks;
        for (size_t unit; (unit = next.fetch_add(1)) < walk.units();) {
            uint64_t seq = 0;
            walker.walk(unit, walk.bag, [&](BagStatsRow& row, size_t length) {
                if constexpr (R::NEEDS_PICKS) walker.picks(&picks);
                RankedRow ranked{ranker.key(row, length, picks), unit, seq++,
                                 {}, {}};
                if (heap.size() == top && !ranks_before(ranked, heap.front()))
                    return;
                if (walk.args.rank.best_pairs == BestPairs::APPEAR) {
                    walker.picks(&ranked.best);
                } else if constexpr (R::NEEDS_PICKS) {
                    if (walk.args.rank.best_pairs == BestPairs::LEAD)
                        ranked.best = ranker.leaders(row, picks);
                }
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
            progress.finished(&walker);
        }
    });
    std::vector<RankedRow> out;
    for (std::vector<RankedRow>& heap : heaps)
        std::move(heap.begin(), heap.end(), std::back_inserter(out));
    std::sort(out.begin(), out.end(), ranks_before);
    if (out.size() > top) out.resize(top);
    return out;
}

// The row whose picks are `picks`, with its unit and walk sequence, its
// pair picks' letter count and its picks as RowWalker::picks() numbers them;
// false when they don't make a row.
bool find_row(Walk const& walk, std::vector<size_t> const& picks,
              RankedRow* out, size_t* out_length,
              std::vector<uint32_t>* out_picks) {
    RowWalker walker = walk.walker();
    uint64_t seq = 0;
    bool found = false;
    walker.walk(picks[0], walk.bag, [&](BagStatsRow& row, size_t length) {
        if (!found && walker.at(picks)) {
            row.pair = walker.pair();
            row.pairs = walker.pairs();
            *out = {{}, picks[0], seq, std::move(row), {}};
            *out_length = length;
            walker.picks(out_picks);
            found = true;
        }
        ++seq;
    });
    return found;
}

// Where `target`, its key set, comes among all rows.
template <typename R>
ScoreRank score_rank(Walk const& walk, R const& ranker,
                     RankedRow const& target) {
    std::atomic<size_t> next{0};
    std::vector<ScoreRank> parts(walk.threads);
    Progress progress(walk.args.progress, walk.passes, walk.passes,
                      walk.units());
    run_threads(walk.threads, [&](size_t t) {
        RowWalker walker = walk.walker();
        ScoreRank& part = parts[t];
        std::vector<uint32_t> picks;
        for (size_t unit; (unit = next.fetch_add(1)) < walk.units();) {
            uint64_t seq = 0;
            walker.walk(unit, walk.bag,
                        [&](BagStatsRow const& row, size_t length) {
                if constexpr (R::NEEDS_PICKS) walker.picks(&picks);
                RankedRow const ranked{ranker.key(row, length, picks), unit,
                                       seq++, {}, {}};
                add_score_rank(ranked, target, &part);
            });
            progress.finished(&walker);
        }
    });
    ScoreRank out;
    for (ScoreRank const& part : parts) {
        out.before += part.before;
        out.larger += part.larger;
        out.tied += part.tied;
        out.rows += part.rows;
    }
    return out;
}

// Prints the --score row and its rank.
template <typename R>
int print_score(Walk const& walk, R* ranker, bool passes) {
    std::vector<size_t> picks;
    if (!score_picks(PROGRAM, walk.args.rank.score, walk.args.search,
                     walk.steps, walk.bag, &picks))
        return 1;
    RankedRow target;
    size_t length;
    std::vector<uint32_t> target_picks;
    if (!find_row(walk, picks, &target, &length, &target_picks)) {
        fprintf(stderr, "%s: no row has the --score picks\n", PROGRAM);
        return 1;
    }
    if (passes) count_sort_values(walk, ranker);
    target.key = ranker->key(target.row, length, target_picks);
    ScoreRank const rank = score_rank(walk, *ranker, target);
    print_score_rank(target.row, rank);
    if constexpr (R::NEEDS_PICKS) {
        if (walk.args.rank.score_detail) {
            std::vector<PairBag const*> bags;
            for (size_t j = 0; j < picks.size(); ++j)
                bags.push_back(&(*walk.steps[j])[picks[j]]);
            print_score_detail(walk.args.rank.sorts, *ranker, target, bags,
                               target_picks);
        }
    }
    return 0;
}

// Prints the -n rows, or with --best-pairs each recognized pick the first
// time a row recognizes it.
template <typename R>
void print_top(Walk const& walk, R* ranker, bool passes) {
    if (passes) count_sort_values(walk, ranker);
    std::string line;
    std::unordered_set<uint32_t> seen;
    for (RankedRow const& ranked :
         top_rows(walk, *ranker, size_t(walk.args.rank.top))) {
        line.clear();
        if (walk.args.rank.best_pairs == BestPairs::NONE)
            append_row(&line, ranked.row, walk.args.rank.top_segments);
        append_best_pairs(&line, &seen, ranked.best, *walk.steps[0],
                          walk.prospect_bags);
        fwrite(line.data(), 1, line.size(), stdout);
    }
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

    if (args.sentence == CLASSIFIED_NO_SENTENCE)
        alert(PROGRAM, "--sentence not supplied");
    char const* const root = require_workflow_root(PROGRAM);
    if (root == NULL) return 1;
    DfsPairSet rejected;
    if (!load_no_pairs(PROGRAM, root, args.sentence, &rejected)) return 1;

    PairsCounts const bag = pairs_letter_counts(letters.c_str());
    std::vector<PairBag> prospect_bags;
    std::vector<PairBag> pair_bags;
    std::vector<PairsCounts> word_bags;
    PairsExactCounter counter(bag, INNER_MIN_WORD_LENGTH);
    bool has_prior;
    if (!load_pick_bags(PROGRAM, args.rank, args.prospects_file,
                        args.pairs_file, bag, rejected, &prospect_bags,
                        &pair_bags, &has_prior) ||
        !load_word_bags(args.dictionary_file, bag, &word_bags) ||
        !load_inner_dictionary(&counter))
        return 1;
    DfsPairSet().swap(rejected);

    std::vector<std::vector<PairBag> const*> steps;
    steps.insert(steps.end(), args.search.pairs, &pair_bags);
    steps.insert(steps.end(), args.search.prospects, &prospect_bags);
    size_t threads = resolve_search_threads(args.threads);
    unsigned int const cores = std::thread::hardware_concurrency();
    if (cores != 0) threads = std::min(threads, size_t(cores));
    PairCountTable const table(
        pair_count_index(letters, longest_leftover(bag, steps, word_bags,
                                                   args.search.words)),
        bag, counter, steps.size() > 1, threads);
    if (!table.allocated()) {
        fprintf(stderr, "%s: can't allocate the pair count table\n", PROGRAM);
        return 1;
    }

    RankArgs const& rank = args.rank;
    std::vector<BagStatsCount const*> const keys = sort_flag_keys(rank.sorts);
    Ranker ranker(keys, rank.by_length);
    PickRanker pick_ranker(keys,
                           pick_ranks(pair_bags, prospect_bags, has_prior));
    bool const passes = (rank.top != 0 || !rank.score.empty()) &&
        (rank.by_pick || ranker.needs_counts());
    RestTable const rest_table(bag, steps.size() > 1 || passes);
    Walk const walk{steps, prospect_bags, bag, word_bags, args, counter, table,
                    rest_table, threads, passes ? 2 : 1};

    setvbuf(stdout, nullptr, _IOFBF, 1 << 22);
    if (!rank.score.empty())
        return rank.by_pick ? print_score(walk, &pick_ranker, passes)
                            : print_score(walk, &ranker, passes);
    if (!rank.top_segments && rank.best_pairs == BestPairs::NONE)
        write_bag_stats_header(stdout, {letters, INNER_MIN_WORD_LENGTH,
                                        args.min_word_bag_length,
                                        search_text(args.search),
                                        bag_stats_header_path(args.prospects_file),
                                        args.sentence,
                                        bag_stats_header_path(args.pairs_file),
                                        bag_stats_header_path(args.dictionary_file)});
    if (rank.top == 0) {
        print_rows(walk);
        return 0;
    }
    if (rank.by_pick) print_top(walk, &pick_ranker, passes);
    else print_top(walk, &ranker, passes);
    return 0;
}
