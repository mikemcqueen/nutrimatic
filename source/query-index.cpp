// Prints the highest-corpus-frequency words/phrases makeable from a subset
// of a given letter bag. This is phase 1 of dfs-anagrams (DfsClassList)
// only: it ranks vocabulary by raw frequency, not by completed-anagram
// score, and does not require using every letter.

#include "dfs-class-list.h"
#include "dfs-cli-args.h"
#include "dfs-diagnostic.h"
#include "index.h"
#include "optparse.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>

#include <algorithm>
#include <string>
#include <vector>

static int const DEFAULT_MIN_WORD_LEN = 1;
static int const DEFAULT_TOP = 100;
static double const DEFAULT_WORD_BONUS = 0.0;

// Matches the restart cost dfs-anagrams.cpp hardcodes for phase 2, since
// --word-bonus previews how a member would fare against that penalty once
// phase 2 actually chains it into a multi-class match.
static double const RESTART = 1e-6;

struct Args {
  char const* index_file;
  char const* dictionary_file;
  std::string letters;
  int min_word_len;
  int top;
  bool words_only;
  double word_bonus;
};

static void usage(char const* program) {
  fprintf(stderr,
      "usage: %s input.index letters"
      " [-u used-letters] [--dict PATH] [-m min-word-length] [-n top]"
      " [-w|--words-only]\n"
      "  -m defaults to %d; 0 for no minimum\n"
      "  -n defaults to %d; 0 for no limit\n"
      "  --dict PATH filters entries to words in the dictionary\n"
      "  -w, --words-only excludes multi-word phrases\n"
      "  --word-bonus N boosts multi-word members by (1/%.0g)^N, previewing\n"
      "    how much of a phase-2 restart penalty a bonus of N would offset;\n"
      "    defaults to %g (no boost)\n",
      program, DEFAULT_MIN_WORD_LEN, DEFAULT_TOP, RESTART, DEFAULT_WORD_BONUS);
}

static int const OPT_DICT = 256;
static int const OPT_WORD_BONUS = 257;

static struct optparse_long const long_options[] = {
  { "used-letters", 'u', OPTPARSE_REQUIRED },
  { "dict", OPT_DICT, OPTPARSE_REQUIRED },
  { "min-word-length", 'm', OPTPARSE_REQUIRED },
  { "top", 'n', OPTPARSE_REQUIRED },
  { "words-only", 'w', OPTPARSE_NONE },
  { "word-bonus", OPT_WORD_BONUS, OPTPARSE_REQUIRED },
  { NULL, 0, OPTPARSE_NONE },
};

static bool parse_args(char* argv[], Args* out) {
  out->dictionary_file = NULL;
  out->min_word_len = DEFAULT_MIN_WORD_LEN;
  out->top = DEFAULT_TOP;
  out->words_only = false;
  out->word_bonus = DEFAULT_WORD_BONUS;

  struct optparse options;
  optparse_init(&options, argv);

  std::string used;
  int opt;
  while ((opt = optparse_long(&options, long_options, NULL)) != -1) {
    switch (opt) {
      case 'u':
        used += options.optarg;
        break;
      case OPT_DICT:
        out->dictionary_file = options.optarg;
        break;
      case 'm':
        if (!parse_count(options.optarg, "--min-word-length",
                         &out->min_word_len))
          return false;
        break;
      case 'n':
        if (!parse_count(options.optarg, "--top", &out->top))
          return false;
        break;
      case 'w':
        out->words_only = true;
        break;
      case OPT_WORD_BONUS:
        if (!parse_double(options.optarg, "--word-bonus", &out->word_bonus))
          return false;
        break;
      default:
        fprintf(stderr, "error: %s\n", options.errmsg);
        usage(argv[0]);
        return false;
    }
  }

  char const* index_file = optparse_arg(&options);
  char const* letters = optparse_arg(&options);
  if (index_file == NULL || letters == NULL ||
      optparse_arg(&options) != NULL) {
    usage(argv[0]);
    return false;
  }

  std::string bag;
  std::string remove;
  if (!clean_letters(letters, "letters", &bag)) return false;
  if (!clean_letters(used.c_str(), "used letters", &remove)) return false;

  out->index_file = index_file;
  if (!subtract_letters(bag, remove, &out->letters)) return false;
  return true;
}

// (1/RESTART)^word_bonus, applied once to any member spanning more than one
// word: at word_bonus=1 this cancels a phase-2 restart's cost outright (in
// log space, -1 * log(RESTART) exactly offsets the + log(RESTART) a restart
// adds); at 0.5 it offsets half of it, and so on. See dfs-search.cpp's
// restart_log_rate for the penalty this is meant to preview offsetting.
static double word_bonus_factor(double word_bonus) {
  return pow(RESTART, -word_bonus);
}

struct RankedMember {
  double score;
  DfsClassMember const* member;
};

static bool ranked_order(RankedMember const& a, RankedMember const& b) {
  if (a.score != b.score) return a.score > b.score;
  return a.member->text < b.member->text;
}

int main(int argc, char* argv[]) {
  dfs_reset_diagnostic_clock();
  dfs_set_diagnostic_stream(stderr);

  Args args;
  if (!parse_args(argv, &args)) return 2;

  DfsDictionary dictionary;
  DfsDictionary const* dictionary_filter = NULL;
  if (args.dictionary_file != NULL) {
    if (!load_dictionary(args.dictionary_file, &dictionary)) return 1;
    dictionary_filter = &dictionary;
  }

  FILE* fp = fopen(args.index_file, "rb");
  if (fp == NULL) {
    fprintf(stderr, "error: can't open \"%s\"\n", args.index_file);
    return 1;
  }

  IndexReader reader(fp);
  DfsClassList classes(&reader, args.letters, args.min_word_len,
                       !args.words_only, dictionary_filter);
  dfs_diagnostic(
      "phase 1 complete: %zu entries, %zu classes, %lld trie nodes\n",
      classes.entry_count(), classes.classes().size(),
      (long long) classes.nodes_visited());

  std::vector<DfsClassMember> flattened;
  flattened.reserve(classes.entry_count());
  for (DfsAnagramClass const& anagram_class : classes.classes())
    flattened.insert(flattened.end(),
                     anagram_class.members.begin(),
                     anagram_class.members.end());

  double const bonus_factor = word_bonus_factor(args.word_bonus);
  std::vector<RankedMember> ranked;
  ranked.reserve(flattened.size());
  for (DfsClassMember const& member : flattened) {
    double const score = member.word_count > 1
        ? double(member.count) * bonus_factor
        : double(member.count);
    ranked.push_back({score, &member});
  }

  size_t const top = args.top == 0 ? ranked.size()
                                    : std::min(ranked.size(), size_t(args.top));
  std::partial_sort(ranked.begin(), ranked.begin() + top,
                    ranked.end(), ranked_order);

  for (size_t i = 0; i < top; ++i) {
    if (args.word_bonus == 0.0)
      printf("%lld %s\n", (long long) ranked[i].member->count,
             ranked[i].member->text.c_str());
    else
      printf("%#.4g %s\n", ranked[i].score, ranked[i].member->text.c_str());
  }
  return 0;
}
