// combine-pairs.cpp - Print the maximal combinations of pairs that fit within
// a letter bag, with the letters each leaves over.

#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "classified.h"
#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "letter-bag.h"
#include "optparse.h"
#include "row-input.h"
#include "workflow-paths.h"

namespace {

int const DEFAULT_MIN_PAIRS = 2;
int const OPT_CV = 256;

struct Args {
  LetterBag bag;
  int min_pairs = DEFAULT_MIN_PAIRS;
  int sentence = CLASSIFIED_NO_SENTENCE;
  bool cv = false;
  std::string source;
};

void usage(FILE* out) {
  fputs("usage: combine-pairs [-u LETTERS]... [-m N] [--cv] LETTERS FILE|-\n"
        "       combine-pairs [-u LETTERS]... [-m N] [--cv] -s N LETTERS "
        "[FILE|-]\n",
        out);
  if (out != stdout) return;

  fputs("  print each combination of the words and word,word pairs in FILE\n"
        "  that fits within LETTERS and leaves no room for another, as\n"
        "  WORD WORD,WORD WORD,... REMAINING, by ascending count of REMAINING\n"
        "  letters; '-' reads standard input\n"
        "\noptions:\n", stdout);
  dfs_help_used_letters();
  dfs_help_option("-m, --min-pairs N",
      "print only combinations of at least N entries (default: %d)",
      DEFAULT_MIN_PAIRS);
  dfs_help_option("-s, --sentence N",
      "read $WFROOT/%s/sN/yes/yes.pairs when FILE is not given",
      WORKFLOW_CLASSIFIED_PATH);
  dfs_help_option("--cv",
      "add a column with the consonant/vowel ratio of REMAINING, y being a "
      "consonant, and sort by it ascending; 0 when no consonants are left "
      "and inf when only consonants are");
  dfs_help_option("-h, --help", "show this help");
}

bool parse_args(char* argv[], Args* out, bool* help) {
  static struct optparse_long const long_options[] = {
    { "used-letters", 'u', OPTPARSE_REQUIRED },
    { "min-pairs", 'm', OPTPARSE_REQUIRED },
    CLASSIFIED_SENTENCE_LONG_OPTION,
    { "cv", OPT_CV, OPTPARSE_NONE },
    { "help", 'h', OPTPARSE_NONE },
    { NULL, 0, OPTPARSE_NONE },
  };

  struct optparse options;
  optparse_init(&options, argv);

  std::string used_letters;
  int option;
  while ((option = optparse_long(&options, long_options, NULL)) != -1) {
    switch (option) {
      case 'u':
        used_letters += options.optarg;
        break;
      case 'm':
        if (!parse_count(options.optarg, "--min-pairs", &out->min_pairs))
          return false;
        break;
      case 's':
        if (!parse_classified_sentence(options.optarg, &out->sentence))
          return false;
        break;
      case OPT_CV:
        out->cv = true;
        break;
      case 'h':
        *help = true;
        return true;
      default:
        fprintf(stderr, "error: %s\n", options.errmsg);
        return false;
    }
  }

  char const* const letters = optparse_arg(&options);
  if (letters == NULL) return false;
  if (!make_letter_bag(letters, used_letters, &out->bag)) return false;

  char const* const source = optparse_arg(&options);
  if (source != NULL) out->source = source;
  return (source != NULL || out->sentence != CLASSIFIED_NO_SENTENCE) &&
      optparse_arg(&options) == NULL;
}

struct LetterNeed {
  uint8_t slot;
  uint8_t count;
};

struct Entry {
  std::string text;
  std::vector<LetterNeed> needs;
};

typedef std::vector<int> Counts;

struct Combination {
  std::string entries;
  std::string remaining;
  double cv;
};


bool entry_fits(Entry const& entry, Counts const& remaining) {
  for (LetterNeed const& need : entry.needs)
    if (need.count > remaining[need.slot]) return false;
  return true;
}

class Combiner {
 public:
  Combiner(std::vector<Entry> const& entries, std::vector<char> const& letters,
      Counts const& bag, size_t min_pairs,
      std::vector<Combination>* combinations)
      : entries_(entries), letters_(letters), remaining_(bag),
        min_pairs_(min_pairs), combinations_(combinations) {}

  void run() {
    std::vector<size_t> fitting(entries_.size());
    for (size_t i = 0; i < fitting.size(); ++i) fitting[i] = i;
    search(0, fitting);
  }

 private:
  void search(size_t start, std::vector<size_t> const& fitting) {
    for (size_t i : fitting) {
      if (i < start) continue;
      Entry const& entry = entries_[i];
      for (LetterNeed const& need : entry.needs)
        remaining_[need.slot] -= need.count;
      chosen_.push_back(i);

      std::vector<size_t> next;
      for (size_t j : fitting)
        if (j != i && entry_fits(entries_[j], remaining_)) next.push_back(j);
      if (next.empty()) {
        if (chosen_.size() >= min_pairs_) add();
      } else {
        search(i + 1, next);
      }

      chosen_.pop_back();
      for (LetterNeed const& need : entry.needs)
        remaining_[need.slot] += need.count;
    }
  }

  void add() {
    Combination combination;
    for (size_t i : chosen_) {
      if (!combination.entries.empty()) combination.entries += ',';
      combination.entries += entries_[i].text;
    }
    int counts[26] = { 0 };
    for (size_t s = 0; s < letters_.size(); ++s) {
      combination.remaining.append(size_t(remaining_[s]), letters_[s]);
      if (letters_[s] >= 'a' && letters_[s] <= 'z')
        counts[letters_[s] - 'a'] = remaining_[s];
    }
    combination.cv = cv_ratio(counts);
    combinations_->push_back(std::move(combination));
  }

  std::vector<Entry> const& entries_;
  std::vector<char> const& letters_;
  Counts remaining_;
  size_t const min_pairs_;
  std::vector<Combination>* const combinations_;
  std::vector<size_t> chosen_;
};

}  // namespace

int main(int argc, char* argv[]) {
  (void) argc;
  Args args;
  bool help = false;
  if (!parse_args(argv, &args, &help)) {
    usage(stderr);
    return 2;
  }
  if (help) {
    usage(stdout);
    return 0;
  }

  if (args.source.empty()) {
    char const* const root = require_workflow_root("combine-pairs");
    if (root == NULL ||
        !classified_pair_file(
            "combine-pairs", root, args.sentence, "yes", &args.source))
      return 1;
  }

  std::vector<char> letters;
  Counts bag;
  int slot[UCHAR_MAX + 1];
  for (int ch = 0; ch <= UCHAR_MAX; ++ch) {
    slot[ch] = -1;
    if (args.bag.counts[ch] == 0) continue;
    slot[ch] = int(letters.size());
    letters.push_back(char(ch));
    bag.push_back(args.bag.counts[ch]);
  }

  std::vector<Entry> entries;
  std::set<std::string> seen;
  if (!read_rows("combine-pairs", args.source.c_str(), "input", true,
          [&](DfsPairRow const& row, std::string const&) {
            std::string const letters_used = row.left + row.right;
            if (!fits_letter_bag(args.bag, letters_used) ||
                !seen.insert(row.entry()).second ||
                (!row.right.empty() && !seen.insert(row.entry(true)).second))
              return true;
            Entry entry;
            entry.text = row.entry();
            Counts need(letters.size(), 0);
            for (char ch : letters_used) ++need[slot[(unsigned char) ch]];
            for (size_t s = 0; s < need.size(); ++s)
              if (need[s] != 0)
                entry.needs.push_back({ uint8_t(s), uint8_t(need[s]) });
            entries.push_back(entry);
            return true;
          }))
    return 1;

  std::vector<Combination> combinations;
  Combiner(entries, letters, bag, size_t(args.min_pairs), &combinations)
      .run();
  std::stable_sort(combinations.begin(), combinations.end(),
      [](Combination const& a, Combination const& b) {
        return a.remaining.size() < b.remaining.size();
      });
  if (args.cv)
    std::stable_sort(combinations.begin(), combinations.end(),
        [](Combination const& a, Combination const& b) {
          return a.cv < b.cv;
        });

  size_t entries_width = 0;
  size_t remaining_width = 0;
  size_t cv_width = 0;
  std::vector<std::string> cvs;
  for (Combination const& combination : combinations) {
    entries_width = std::max(entries_width, combination.entries.size());
    remaining_width =
        std::max(remaining_width, combination.remaining.size());
    if (!args.cv) continue;
    char cv[32];
    snprintf(cv, sizeof cv, "%.2f", combination.cv);
    cvs.push_back(cv);
    cv_width = std::max(cv_width, cvs.back().size());
  }
  for (size_t i = 0; i < combinations.size(); ++i) {
    Combination const& combination = combinations[i];
    if (args.cv)
      std::cout << std::left << std::setw(int(entries_width))
                << combination.entries << ' ' << std::setw(int(remaining_width))
                << combination.remaining << ' ' << std::right
                << std::setw(int(cv_width)) << cvs[i] << '\n';
    else if (combination.remaining.empty())
      std::cout << combination.entries << '\n';
    else
      std::cout << std::left << std::setw(int(entries_width))
                << combination.entries << ' ' << combination.remaining
                << '\n';
  }
  if (!(std::cout << std::flush)) {
    fputs("combine-pairs: can't write output\n", stderr);
    return 1;
  }
  return 0;
}
