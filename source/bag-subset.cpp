// bag-subset.cpp - Count the words and pairs that contain each subset of a
// letter bag, or with -r that fit once it is removed.

#include <limits.h>
#include <stdio.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "classified.h"
#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "letter-bag.h"
#include "optparse.h"
#include "row-input.h"
#include "workflow-paths.h"

namespace {

int const DEFAULT_MAX_LETTERS = 15;
int const DEFAULT_LENGTH = 4;

typedef std::vector<int> Counts;

struct Args {
  LetterBag bag;
  int max_letters = DEFAULT_MAX_LETTERS;
  int length = DEFAULT_LENGTH;
  int sentence = CLASSIFIED_NO_SENTENCE;
  bool reverse = false;
  char const* source = NULL;
};

struct SubsetCount {
  std::string subset;
  size_t count;
};

void usage(FILE* out) {
  fputs("usage: bag-subset [-u LETTERS]... [-s N] [-x N] [-l LENGTH] [-r] "
        "LETTERS FILE|-\n", out);
  if (out != stdout) return;

  fputs("  for each subset of LETTERS up to LENGTH letters, count the words\n"
        "  and word,word pairs in FILE that fit within LETTERS and contain\n"
        "  every letter of the subset; print SUBSET COUNT lines by ascending\n"
        "  COUNT\n"
        "  entries must be in $WFROOT/.wf/dict/words.filtered, and pairs not\n"
        "  in $WFROOT/.wf/classified/no/no.pairs; '-' reads standard input\n"
        "\noptions:\n", stdout);
  dfs_help_used_letters();
  classified_help_sentence_no();
  dfs_help_option("-x, --max-letters N",
      "count only entries of at most N letters; 0 for no limit "
      "(default: %d)", DEFAULT_MAX_LETTERS);
  dfs_help_option("-l, --length LENGTH",
      "use subsets of at most LENGTH letters (default: %d)",
      DEFAULT_LENGTH);
  dfs_help_option("-r, --reverse",
      "instead count the entries that fit within LETTERS once the subset is "
      "removed");
  dfs_help_option("-h, --help", "show this help");
}

bool parse_args(char* argv[], Args* out, bool* help) {
  static struct optparse_long const long_options[] = {
    { "used-letters", 'u', OPTPARSE_REQUIRED },
    CLASSIFIED_SENTENCE_LONG_OPTION,
    { "max-letters", 'x', OPTPARSE_REQUIRED },
    { "length", 'l', OPTPARSE_REQUIRED },
    { "reverse", 'r', OPTPARSE_NONE },
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
      case 's':
        if (!parse_classified_sentence(options.optarg, &out->sentence))
          return false;
        break;
      case 'x':
        if (!parse_count(options.optarg, "--max-letters", &out->max_letters))
          return false;
        break;
      case 'l':
        if (!parse_count(options.optarg, "--length", &out->length))
          return false;
        if (out->length == 0) {
          fputs("bag-subset: --length must be positive\n", stderr);
          return false;
        }
        break;
      case 'r':
        out->reverse = true;
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

  out->source = optparse_arg(&options);
  return out->source != NULL && optparse_arg(&options) == NULL;
}

bool fits(Counts const& need, Counts const& have) {
  for (size_t i = 0; i < need.size(); ++i)
    if (need[i] > have[i]) return false;
  return true;
}

void each_subset(
    std::vector<char> const& letters, Counts const& bag, int length,
    size_t index, Counts* subset, std::string* text,
    std::function<void(Counts const&, std::string const&)> const& visit) {
  if (!text->empty()) visit(*subset, *text);
  if (int(text->size()) == length) return;
  for (size_t i = index; i < letters.size(); ++i) {
    if ((*subset)[i] == bag[i]) continue;
    ++(*subset)[i];
    text->push_back(letters[i]);
    each_subset(letters, bag, length, i, subset, text, visit);
    text->pop_back();
    --(*subset)[i];
  }
}

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

  char const* const root = require_workflow_root("bag-subset");
  if (root == NULL) return 1;
  std::filesystem::path const dict_path =
      std::filesystem::path(root) / WORKFLOW_DICT_PATH;
  DfsDictionary dictionary;
  if (!load_dictionary(dict_path.c_str(), &dictionary)) return 1;

  DfsPairSet rejected;
  if (!load_global_no_pairs("bag-subset", root, &rejected)) return 1;
  if (args.sentence != CLASSIFIED_NO_SENTENCE &&
      !load_sentence_no_pairs("bag-subset", root, args.sentence, &rejected))
    return 1;

  std::vector<char> letters;
  Counts bag;
  int slot[UCHAR_MAX + 1];
  std::fill(slot, slot + UCHAR_MAX + 1, -1);
  for (int ch = 0; ch <= UCHAR_MAX; ++ch) {
    if (args.bag.counts[ch] == 0) continue;
    slot[ch] = int(letters.size());
    letters.push_back(char(ch));
    bag.push_back(args.bag.counts[ch]);
  }

  std::map<Counts, size_t> entries;
  if (!read_rows("bag-subset", args.source, "input", true,
          [&](DfsPairRow const& row, std::string const&) {
            std::string const text = row.left + row.right;
            if ((args.max_letters != 0 &&
                    text.size() > size_t(args.max_letters)) ||
                !dictionary.contains(row.left) ||
                (!row.right.empty() && !dictionary.contains(row.right)) ||
                rejected.contains(row.entry()))
              return true;
            Counts need(letters.size(), 0);
            for (char ch : text) {
              int const i = slot[(unsigned char) ch];
              if (i < 0 || ++need[i] > bag[i]) return true;
            }
            ++entries[need];
            return true;
          }))
    return 1;

  std::vector<SubsetCount> results;
  Counts subset(letters.size(), 0);
  Counts candidate(letters.size(), 0);
  std::string text;
  each_subset(letters, bag, args.length, 0, &subset, &text,
      [&](Counts const& chosen, std::string const& name) {
        for (size_t i = 0; i < bag.size(); ++i)
          candidate[i] = bag[i] - chosen[i];
        size_t count = 0;
        for (auto const& [need, number] : entries)
          if (args.reverse ? fits(need, candidate) : fits(chosen, need))
            count += number;
        results.push_back({ name, count });
      });

  std::sort(results.begin(), results.end(),
      [](SubsetCount const& a, SubsetCount const& b) {
        if (a.count != b.count) return a.count < b.count;
        if (a.subset.size() != b.subset.size())
          return a.subset.size() < b.subset.size();
        return a.subset < b.subset;
      });
  size_t subset_width = 0;
  size_t count_width = 0;
  for (SubsetCount const& result : results) {
    subset_width = std::max(subset_width, result.subset.size());
    count_width =
        std::max(count_width, std::to_string(result.count).size());
  }
  for (SubsetCount const& result : results)
    std::cout << std::left << std::setw(int(subset_width)) << result.subset
              << ' ' << std::right << std::setw(int(count_width))
              << result.count << '\n';
  if (!(std::cout << std::flush)) {
    fputs("bag-subset: can't write output\n", stderr);
    return 1;
  }
  return 0;
}
