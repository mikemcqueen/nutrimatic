// bag-stats.cpp - Rank combinations of pair letter bags by the exact word +
// pair completions they leave.

#include "bag-stats-impl.h"
#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "letter-bag.h"
#include "optparse.h"
#include "pair-evidence.h"
#include "pairs-impl.h"
#include "row-input.h"
#include "workflow-paths.h"

#include "sub-bag-index.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iterator>
#include <map>
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
constexpr int OPT_TOP_SEGMENTS = 261;
constexpr int OPT_SCORE = 262;
constexpr int OPT_PRIOR = 263;
constexpr int OPT_BY_PICK = 264;
constexpr int OPT_P1 = 265;
constexpr int OPT_CLUES = 266;
constexpr int OPT_PRIOR_WEIGHTS = 267;
constexpr int OPT_SORT = 268;
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
    bool by_pick = false;
    char const* prior_file = NULL;
    char const* p1 = NULL;
    char const* clues = NULL;
    char const* prior_weights = NULL;
    bool top_segments = false;
    // --score, the row's picks as PAIR names them, each as its two words.
    std::vector<std::string> score;
    char const* pairs_file = NULL;
    char const* dictionary_file = NULL;
};

struct PairBag {
    PairsCounts counts;
    std::string pair;
    uint64_t pairs = 0;
    // The best prior of its rows, from --prior or --p1 and --clues;
    // -INFINITY when none has one.
    double prior = -INFINITY;
};

void usage(char const* program, FILE* out) {
    fprintf(out,
            "usage: %s [-u LETTERS] [--min-wb-len N] [--ss STEPS] [--pp FILE]\n"
            "       [--threads N] [-n N SORT... [--by-length | --by-pick]]\n"
            "       [--score W1,W2... SORT... [--by-length | --by-pick]]\n"
            "       [--prior FILE | [--p1 INDEX] [--clues CLUEDATA]\n"
            "        [--prior-weights SPEC]] [--top-segments]\n"
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
    dfs_help_option("--top-segments",
                    "print no header, and each row as \"0 PAIR,PAIR...\", "
                    "its pair picks, for top-segments; bag-stats-sort can't "
                    "read it");
    dfs_help_option("--score W1,W2",
                    "print only the row whose pair picks are these, one "
                    "--score per ppb and pb step in PAIR order, ppb picks "
                    "first, and below it \"rank R of N, tied A-B\": where -n, "
                    "given the same SORT options and --by-length, would print "
                    "it, and the rows sharing its sort key; each pick is the "
                    "bag of its two words");
    dfs_help_option("-h, --help", "show this help");
    fputs("\nSORT options, only with -n or --score:\n", stdout);
    bag_stats_help_sort_flags();
    dfs_help_option("--by-length",
                    "rank each row's sort value only among the rows whose "
                    "pair picks have as many letters, as bag-stats-sort "
                    "--by-length does");
    dfs_help_option("--by-pick",
                    "rank each row's sort value among the rows that share "
                    "each of its pair picks, Q the best mid-rank percentile "
                    "from the top over picks and SORT options; with --prior, "
                    "a pick's Q is weighted by P, its prior's percentile from "
                    "the top among its list's bags, as P * Q * sqrt(P of each "
                    "other pick), and rows sort by the smallest; needs two or "
                    "more ppb and pb steps; bag-stats-sort has no equivalent");
    dfs_help_option("--prior FILE",
                    "\"SCORE word,word\" lines, as pair-prior prints them, "
                    "larger better; a pick bag scores its best row, below "
                    "every score when none is in FILE; only with --by-pick");
    dfs_help_option("--p1 INDEX",
                    "score each pair's prior from its phase 1 verdicts in "
                    "INDEX, as make-p1-index writes it, as pair-prior does; "
                    "only with --by-pick, not with --prior");
    dfs_help_option("--clues CLUEDATA",
                    "score each pair's prior from the cluer clue index "
                    "beside CLUEDATA too, or alone");
    dfs_help_option("--prior-weights SPEC",
                    "how --p1 and --clues score a pair, as pair-prior "
                    "--prior-weights (default %s, less the terms of an index "
                    "not given)", PRIOR_WEIGHTS_DEFAULT);
    fputs("\n--prior-weights terms:\n", stdout);
    help_prior_terms();
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
        { "top-segments", OPT_TOP_SEGMENTS, OPTPARSE_NONE },
        { "score", OPT_SCORE, OPTPARSE_REQUIRED },
        { "prior", OPT_PRIOR, OPTPARSE_REQUIRED },
        { "by-pick", OPT_BY_PICK, OPTPARSE_NONE },
        { "p1", OPT_P1, OPTPARSE_REQUIRED },
        { "clues", OPT_CLUES, OPTPARSE_REQUIRED },
        { "prior-weights", OPT_PRIOR_WEIGHTS, OPTPARSE_REQUIRED },
        { BAG_STATS_SORT_FLAGS[0].option, OPT_SORT + 0, OPTPARSE_NONE },
        { BAG_STATS_SORT_FLAGS[1].option, OPT_SORT + 1, OPTPARSE_NONE },
        { BAG_STATS_SORT_FLAGS[2].option, OPT_SORT + 2, OPTPARSE_NONE },
        { BAG_STATS_SORT_FLAGS[3].option, OPT_SORT + 3, OPTPARSE_NONE },
        { BAG_STATS_SORT_FLAGS[4].option, OPT_SORT + 4, OPTPARSE_NONE },
        { BAG_STATS_SORT_FLAGS[5].option, OPT_SORT + 5, OPTPARSE_NONE },
        { BAG_STATS_SORT_FLAGS[6].option, OPT_SORT + 6, OPTPARSE_NONE },
        { "help", 'h', OPTPARSE_NONE },
        { NULL, 0, OPTPARSE_NONE },
    };
    static_assert(SORT_FLAG_COUNT == 7);

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
          case OPT_TOP_SEGMENTS:
            out->top_segments = true;
            break;
          case OPT_SCORE:
            out->score.push_back(options.optarg);
            break;
          case OPT_PRIOR:
            out->prior_file = options.optarg;
            break;
          case OPT_BY_PICK:
            out->by_pick = true;
            break;
          case OPT_P1:
            out->p1 = options.optarg;
            break;
          case OPT_CLUES:
            out->clues = options.optarg;
            break;
          case OPT_PRIOR_WEIGHTS:
            out->prior_weights = options.optarg;
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
    bool const ranked = out->top > 0 || !out->score.empty();
    if (out->top > 0 && !out->score.empty()) {
        fprintf(stderr, "%s: -n and --score don't go together\n", PROGRAM);
        return false;
    }
    if (ranked != !out->keys.empty() || (out->by_length && !ranked)) {
        fprintf(stderr, "%s: -n or --score goes with sort options, and they "
                "with it\n", PROGRAM);
        return false;
    }
    if (out->by_pick &&
        (!ranked || out->by_length ||
         out->search.prospects + out->search.pairs < 2)) {
        fprintf(stderr, "%s: --by-pick goes with -n or --score, without "
                "--by-length, and with two or more ppb and pb steps\n",
                PROGRAM);
        return false;
    }
    bool const evidence = out->p1 != NULL || out->clues != NULL;
    if ((out->prior_file != NULL || evidence) && !out->by_pick) {
        fprintf(stderr, "%s: --prior, --p1 and --clues go with --by-pick\n",
                PROGRAM);
        return false;
    }
    if (out->prior_file != NULL && evidence) {
        fprintf(stderr, "%s: --prior doesn't go with --p1 or --clues\n",
                PROGRAM);
        return false;
    }
    if (out->prior_weights != NULL && !evidence) {
        fprintf(stderr, "%s: --prior-weights goes with --p1 or --clues\n",
                PROGRAM);
        return false;
    }
    if (!out->score.empty() &&
        (out->top_segments ||
         int(out->score.size()) != out->search.prospects + out->search.pairs)) {
        fprintf(stderr, "%s: --score names one pick per ppb and pb step, "
                "without --top-segments\n", PROGRAM);
        return false;
    }
    if ((out->search.prospects > 0) != (out->prospects_file != NULL)) {
        fprintf(stderr, "%s: --pp goes with a ppb step in --ss\n", PROGRAM);
        return false;
    }
    int const stdin_inputs = (strcmp(out->pairs_file, "-") == 0) +
        (strcmp(out->dictionary_file, "-") == 0) +
        (out->prospects_file != NULL && strcmp(out->prospects_file, "-") == 0) +
        (out->prior_file != NULL && strcmp(out->prior_file, "-") == 0);
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

// The --prior FILE's scores by pair_key().
bool load_priors(char const* path,
                 std::unordered_map<std::string, double>* out) {
    return read_lines(PROGRAM, path,
        [&](std::string const& line, size_t number) {
            char* end;
            double const score = strtod(line.c_str(), &end);
            size_t const space = size_t(end - line.c_str());
            if (end == line.c_str() || !std::isfinite(score) ||
                space >= line.size() || line[space] != ' ') {
                fprintf(stderr, "%s: %s:%zu isn't \"SCORE word,word\" with "
                        "a finite SCORE\n", PROGRAM, path, number);
                return false;
            }
            DfsPairRow row;
            if (!parse_pair_row(line.substr(space + 1), "prior", path, number,
                                false, &row))
                return false;
            auto const [entry, inserted] =
                out->emplace(pair_key(row.left, row.right), score);
            if (!inserted) entry->second = std::max(entry->second, score);
            return true;
        });
}

// `prior` sets a row's prior and returns true, or returns false when it has
// none.
using PairPrior = std::function<bool(DfsPairRow const&, double*)>;

bool load_pair_bags(char const* path, PairsCounts const& bag,
                    PairPrior const& prior, std::vector<PairBag>* out) {
    std::unordered_set<std::string> rows;
    std::unordered_map<PairsCounts, size_t, PairsCountsHash> indices;
    return read_rows(PROGRAM, path, "pairs", false,
        [&](DfsPairRow const& row, std::string const&) {
            PairsCounts const counts =
                pairs_letter_counts((row.left + row.right).c_str());
            if (!pairs_fits(bag, counts)) return true;
            std::string const key = pair_key(row.left, row.right);
            if (!rows.insert(key).second) return true;
            auto const inserted = indices.emplace(counts, out->size());
            if (inserted.second) out->push_back({counts, row.left + " " + row.right});
            PairBag& pair_bag = (*out)[inserted.first->second];
            ++pair_bag.pairs;
            double score;
            if (prior && prior(row, &score))
                pair_bag.prior = std::max(pair_bag.prior, score);
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
            if (!rest_table_.get(rest, &row)) {
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
    Distribution() = default;

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

    // Whether add() and key() read the row's picks.
    static constexpr bool NEEDS_PICKS = false;

    using Part = SortValueCounts;

    Part part() const { return Part(keys_.size()); }

    void add(Part* part, BagStatsRow const& row, size_t length,
             std::vector<uint32_t> const&) const {
        for (size_t k = 0; k < keys_.size(); ++k)
            ++(*part)[k][{group(length), bag_stats_sort_value(keys_[k], row)}];
    }

    void finish(std::vector<Part>& parts) {
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
    std::pair<double, double> key(BagStatsRow const& row, size_t length,
                                  std::vector<uint32_t> const&) const {
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
        size_t group;
        double value;
        uint64_t count;
    };

    struct Part {
        std::vector<Entry> entries;
        size_t limit = size_t(1) << 20;
    };

    Part part() const { return {}; }

    void add(Part* part, BagStatsRow const& row, size_t,
             std::vector<uint32_t> const& picks) const {
        for (size_t k = 0; k < keys_.size(); ++k) {
            double const value = bag_stats_sort_value(keys_[k], row);
            for (uint32_t pick : picks)
                part->entries.push_back({k * ranks_.size() + pick, value, 1});
        }
        if (part->entries.size() < part->limit) return;
        compact(&part->entries);
        if (part->entries.size() > part->limit / 2) part->limit *= 2;
    }

    void finish(std::vector<Part>& parts) {
        std::vector<Entry> all;
        for (Part& part : parts) {
            all.insert(all.end(), part.entries.begin(), part.entries.end());
            std::vector<Entry>().swap(part.entries);
        }
        compact(&all);
        groups_.assign(keys_.size() * ranks_.size(), Distribution());
        for (size_t i = 0; i < all.size();) {
            size_t j = i;
            std::vector<std::pair<double, uint64_t>> counts;
            for (; j < all.size() && all[j].group == all[i].group; ++j)
                counts.emplace_back(all[j].value, all[j].count);
            groups_[all[i].group] = Distribution(std::move(counts));
            i = j;
        }
    }

    // Larger first: the smallest P * Q * sqrt(P of each other pick) over the
    // row's picks, negated, then the sum of those, negated.
    std::pair<double, double> key(BagStatsRow const& row, size_t,
                                  std::vector<uint32_t> const& picks) const {
        double best = INFINITY;
        double sum = 0;
        for (size_t i = 0; i < picks.size(); ++i) {
            double q = 1;
            for (size_t k = 0; k < keys_.size(); ++k)
                q = std::min(q, 1 - groups_[k * ranks_.size() + picks[i]]
                                        .percentile(bag_stats_sort_value(
                                            keys_[k], row)));
            double score = ranks_[picks[i]] * q;
            for (size_t j = 0; j < picks.size(); ++j)
                if (j != i) score *= std::sqrt(ranks_[picks[j]]);
            best = std::min(best, score);
            sum += score;
        }
        return {-best, -sum};
    }

  private:
    // Sorts `entries` and merges those of one group and value.
    static void compact(std::vector<Entry>* entries) {
        std::sort(entries->begin(), entries->end(),
                  [](Entry const& a, Entry const& b) {
                      return a.group != b.group ? a.group < b.group
                                                : a.value < b.value;
                  });
        size_t out = 0;
        for (Entry const& entry : *entries) {
            if (out > 0 && (*entries)[out - 1].group == entry.group &&
                (*entries)[out - 1].value == entry.value)
                (*entries)[out - 1].count += entry.count;
            else
                (*entries)[out++] = entry;
        }
        entries->resize(out);
    }

    std::vector<BagStatsCount const*> keys_;
    std::vector<double> ranks_;
    std::vector<Distribution> groups_;
};

// Each pick's P for PickRanker: 1 less the mid-rank percentile of its bag's
// prior among its list's bags, PAIRS bags then --pp bags; all 1 without
// --prior, --p1 or --clues.
std::vector<double> pick_ranks(std::vector<PairBag> const& pair_bags,
                               std::vector<PairBag> const& prospect_bags,
                               bool prior) {
    std::vector<double> out;
    for (std::vector<PairBag> const* bags : {&pair_bags, &prospect_bags}) {
        if (!prior) {
            out.insert(out.end(), bags->size(), 1.0);
            continue;
        }
        std::map<double, uint64_t> priors;
        for (PairBag const& bag : *bags) ++priors[bag.prior];
        Distribution const distribution(
            std::vector<std::pair<double, uint64_t>>(priors.begin(),
                                                     priors.end()));
        for (PairBag const& bag : *bags)
            out.push_back(1 - distribution.percentile(bag.prior));
    }
    return out;
}

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
    RestTable const& rest_table;
    size_t threads;

    RowWalker walker() const {
        return RowWalker(steps, args.search.pairs, word_bags, counter,
                         RestCounter(args.search.words,
                                     args.min_word_bag_length, table),
                         rest_table);
    }

    size_t units() const { return steps[0]->size(); }
};

// Appends `row` as a bag-stats row, or with --top-segments as a top-segments
// row.
void append_row(std::string* out, BagStatsRow const& row, bool top_segments) {
    if (!top_segments) {
        append_bag_stats_row(out, row);
        return;
    }
    *out += "0 ";
    for (size_t i = 0; i < row.pair.size();) {
        if (row.pair.compare(i, 3, " + ") == 0) {
            *out += ',';
            i += 3;
        } else {
            *out += row.pair[i++];
        }
    }
    *out += '\n';
}

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
                append_row(&buffer, row, walk.args.top_segments);
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
template <typename R>
void count_sort_values(Walk const& walk, R* ranker) {
    std::atomic<size_t> next{0};
    std::vector<typename R::Part> parts(walk.threads, ranker->part());
    run_threads(walk.threads, [&](size_t t) {
        RowWalker walker = walk.walker();
        std::vector<uint32_t> picks;
        for (size_t unit; (unit = next.fetch_add(1)) < walk.units();)
            walker.walk(unit, walk.bag,
                        [&](BagStatsRow const& row, size_t length) {
                if constexpr (R::NEEDS_PICKS) walker.picks(&picks);
                ranker->add(&parts[t], row, length, picks);
            });
    });
    ranker->finish(parts);
}

// The `top` rows that rank first, in order.
template <typename R>
std::vector<RankedRow> top_rows(Walk const& walk, R const& ranker,
                                size_t top) {
    std::atomic<size_t> next{0};
    std::vector<std::vector<RankedRow>> heaps(walk.threads);
    run_threads(walk.threads, [&](size_t t) {
        RowWalker walker = walk.walker();
        std::vector<RankedRow>& heap = heaps[t];
        std::vector<uint32_t> picks;
        for (size_t unit; (unit = next.fetch_add(1)) < walk.units();) {
            uint64_t seq = 0;
            walker.walk(unit, walk.bag, [&](BagStatsRow& row, size_t length) {
                if constexpr (R::NEEDS_PICKS) walker.picks(&picks);
                RankedRow ranked{ranker.key(row, length, picks), unit, seq++,
                                 {}};
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

// The list index of each --score pick, in walk step order: pb picks, then
// ppb picks, each kind in increasing order. False, with a message, when a
// pick isn't a bag of its list.
bool score_picks(Args const& args,
                 std::vector<std::vector<PairBag> const*> const& steps,
                 std::vector<size_t>* out) {
    size_t const prospects = size_t(args.search.prospects);
    std::vector<size_t> picks(steps.size());
    for (size_t i = 0; i < args.score.size(); ++i) {
        std::string const& name = args.score[i];
        size_t const comma = name.find(',');
        // ppb picks come first in --score and last in the steps.
        size_t const step = i < prospects ? steps.size() - prospects + i
            : i - prospects;
        std::vector<PairBag> const& bags = *steps[step];
        PairsCounts const counts = pairs_letter_counts(name.c_str());
        auto const found = std::find_if(bags.begin(), bags.end(),
            [&](PairBag const& bag) { return bag.counts == counts; });
        if (comma == std::string::npos || comma == 0 ||
            comma + 1 == name.size() ||
            name.find(',', comma + 1) != std::string::npos ||
            found == bags.end()) {
            fprintf(stderr, "%s: --score %s isn't two words whose bag is "
                    "in %s and fits the letters\n", PROGRAM, name.c_str(),
                    i < prospects ? "--pp" : "PAIRS");
            return false;
        }
        picks[step] = size_t(found - bags.begin());
    }
    std::sort(picks.begin(), picks.begin() + args.search.pairs);
    std::sort(picks.begin() + args.search.pairs, picks.end());
    *out = std::move(picks);
    return true;
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
            *out = {{}, picks[0], seq, std::move(row)};
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
                                       seq++, {}};
                ++part.rows;
                part.before += ranks_before(ranked, target);
                part.larger += ranked.key > target.key;
                part.tied += ranked.key == target.key;
            });
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
    if (!score_picks(walk.args, walk.steps, &picks)) return 1;
    RankedRow target;
    size_t length;
    std::vector<uint32_t> target_picks;
    if (!find_row(walk, picks, &target, &length, &target_picks)) {
        fprintf(stderr, "%s: the --score picks don't fit the letters "
                "together\n", PROGRAM);
        return 1;
    }
    if (passes) count_sort_values(walk, ranker);
    target.key = ranker->key(target.row, length, target_picks);
    ScoreRank const rank = score_rank(walk, *ranker, target);
    write_bag_stats_row(stdout, target.row);
    printf("rank %llu of %llu, tied %llu-%llu\n",
           (unsigned long long)(rank.before + 1),
           (unsigned long long)rank.rows,
           (unsigned long long)(rank.larger + 1),
           (unsigned long long)(rank.larger + rank.tied));
    return 0;
}

// Prints the -n rows.
template <typename R>
void print_top(Walk const& walk, R* ranker, bool passes) {
    if (passes) count_sort_values(walk, ranker);
    std::string line;
    for (RankedRow const& ranked :
         top_rows(walk, *ranker, size_t(walk.args.top))) {
        line.clear();
        append_row(&line, ranked.row, walk.args.top_segments);
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

    PairsCounts const bag = pairs_letter_counts(letters.c_str());
    std::vector<PairBag> prospect_bags;
    std::vector<PairBag> pair_bags;
    std::vector<PairsCounts> word_bags;
    PairsExactCounter counter(bag, INNER_MIN_WORD_LENGTH);
    std::unordered_map<std::string, double> priors;
    PairEvidenceSource evidence;
    PriorWeights weights;
    PairPrior prior;
    if (args.prior_file != NULL) {
        if (!load_priors(args.prior_file, &priors)) return 1;
        prior = [&](DfsPairRow const& row, double* out) {
            auto const found = priors.find(pair_key(row.left, row.right));
            if (found == priors.end()) return false;
            *out = found->second;
            return true;
        };
    } else if (args.p1 != NULL || args.clues != NULL) {
        std::string error;
        if (!open_prior(args.p1, args.clues, args.prior_weights, false,
                        &weights, &evidence, &error)) {
            fprintf(stderr, "%s: %s\n", PROGRAM, error.c_str());
            return 1;
        }
        prior = [&](DfsPairRow const& row, double* out) {
            *out = prior_score(evidence.lookup(row.left, row.right), weights);
            return true;
        };
    }
    if ((args.prospects_file != NULL &&
         !load_pair_bags(args.prospects_file, bag, prior, &prospect_bags)) ||
        !load_pair_bags(args.pairs_file, bag, prior, &pair_bags) ||
        !load_word_bags(args.dictionary_file, bag, &word_bags) ||
        !load_inner_dictionary(&counter))
        return 1;

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

    Ranker ranker(args.keys, args.by_length);
    PickRanker pick_ranker(args.keys,
                           pick_ranks(pair_bags, prospect_bags,
                                      bool(prior)));
    bool const passes = (args.top != 0 || !args.score.empty()) &&
        (args.by_pick || ranker.needs_counts());
    RestTable const rest_table(bag, steps.size() > 1 || passes);
    Walk const walk{steps, bag, word_bags, args, counter, table, rest_table,
                    threads};

    setvbuf(stdout, nullptr, _IOFBF, 1 << 22);
    if (!args.score.empty())
        return args.by_pick ? print_score(walk, &pick_ranker, passes)
                            : print_score(walk, &ranker, passes);
    if (!args.top_segments)
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
    if (args.by_pick) print_top(walk, &pick_ranker, passes);
    else print_top(walk, &ranker, passes);
    return 0;
}
