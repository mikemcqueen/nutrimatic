// Prints the highest-corpus-frequency words/phrases makeable from a subset
// of a given letter bag. By default this is phase 1 of dfs-anagrams
// (DfsClassList) only. --require-completable adds shared phase-2 feasibility
// filtering, but ranking remains based on vocabulary frequency rather than a
// completion's score.

#include "dfs-class-list.h"
#include "dfs-cli-args.h"
#include "dfs-diagnostic.h"
#include "dfs-search.h"
#include "index.h"
#include "optparse.h"

#include <math.h>
#include <stdio.h>

#include <algorithm>
#include <string>
#include <vector>

static int const DEFAULT_TOP = 100;
static double const DEFAULT_WORD_BONUS = 0.0;

struct Args {
  char const* index_file;
  char const* dictionary_file;
  std::string letters;
  int min_word_len;
  int top;
  bool words_only;
  double word_bonus;
  bool require_completable;
  size_t score_cache_bytes;
  int preprocess_threads;
  int search_threads;
  int exact_letters;
  bool allow_cache_fallback;
  bool dense_cache;
};

static void usage(char const* program) {
  fprintf(stderr,
      "usage: %s input.index letters"
      " [-u used-letters] [--dict PATH] [-m min-word-length] [-n top]"
      " [-w|--words-only] [--require-completable]"
      " [-C|--cache-size MiB] [-T|--preprocess-threads N]"
      " [-S|--search-threads N]"
      " [-d|--projection-depth N]"
      " [-D|--dense-cache] [-F|--allow-cache-fallback]\n"
      "  -m defaults to %d; 0 for no minimum\n"
      "  -n defaults to %d; 0 for no limit\n"
      "  --dict PATH filters entries to words in the dictionary\n"
      "  -w, --words-only excludes multi-word phrases\n"
      "  --word-bonus N boosts multi-word members by (1/%.0g)^N, previewing\n"
      "    how much of a phase-2 restart penalty a bonus of N would offset;\n"
      "    defaults to %g (no boost)\n"
      "  --require-completable drops classes whose removal leaves a\n"
      "    remainder phase 2 can't fully turn into an anagram (subject to\n"
      "    -m), using one shared phase-2 cache plus exact validation\n"
      "  -C, --cache-size defaults to %zu MiB; 0 disables it with -F\n"
      "  -T, --preprocess-threads defaults to 0: automatic for 26+ letters;"
      " 1 disables it\n"
      "  -S, --search-threads defaults to 1\n"
      "  -d, --projection-depth keeps this many rarest letter types exact;\n"
      "    the default is the largest depth that fits -C\n"
      "  -D, --dense-cache requests an exact dense score cache\n"
      "  -F, --allow-cache-fallback allows score-cache fallback\n",
      program, DFS_DEFAULT_MIN_WORD_LEN, DEFAULT_TOP, RESTART,
      DEFAULT_WORD_BONUS, DFS_DEFAULT_SCORE_CACHE_MIB);
}

static int const OPT_DICT = 256;
static int const OPT_WORD_BONUS = 257;
static int const OPT_REQUIRE_COMPLETABLE = 258;

static struct optparse_long const long_options[] = {
  { "used-letters", 'u', OPTPARSE_REQUIRED },
  { "dict", OPT_DICT, OPTPARSE_REQUIRED },
  { "min-word-length", 'm', OPTPARSE_REQUIRED },
  { "top", 'n', OPTPARSE_REQUIRED },
  { "words-only", 'w', OPTPARSE_NONE },
  { "word-bonus", OPT_WORD_BONUS, OPTPARSE_REQUIRED },
  { "require-completable", OPT_REQUIRE_COMPLETABLE, OPTPARSE_NONE },
  { "cache-size", 'C', OPTPARSE_REQUIRED },
  { "preprocess-threads", 'T', OPTPARSE_REQUIRED },
  { "search-threads", 'S', OPTPARSE_REQUIRED },
  { "projection-depth", 'd', OPTPARSE_REQUIRED },
  { "dense-cache", 'D', OPTPARSE_NONE },
  { "allow-cache-fallback", 'F', OPTPARSE_NONE },
  { NULL, 0, OPTPARSE_NONE },
};

static bool parse_args(char* argv[], Args* out) {
  out->dictionary_file = NULL;
  out->min_word_len = DFS_DEFAULT_MIN_WORD_LEN;
  out->top = DEFAULT_TOP;
  out->words_only = false;
  out->word_bonus = DEFAULT_WORD_BONUS;
  out->require_completable = false;
  out->score_cache_bytes = DFS_DEFAULT_SCORE_CACHE_MIB * DFS_MIB;
  out->preprocess_threads = 0;
  out->search_threads = 1;
  out->exact_letters = -1;
  out->allow_cache_fallback = false;
  out->dense_cache = false;

  struct optparse options;
  optparse_init(&options, argv);

  std::string used;
  bool min_word_len_given = false;
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
        min_word_len_given = true;
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
      case OPT_REQUIRE_COMPLETABLE:
        out->require_completable = true;
        break;
      case 'C':
        if (!parse_mib(options.optarg, "--cache-size",
                       &out->score_cache_bytes))
          return false;
        break;
      case 'T':
        if (!parse_count(options.optarg, "--preprocess-threads",
                         &out->preprocess_threads))
          return false;
        break;
      case 'S':
        if (!parse_count(options.optarg, "--search-threads",
                         &out->search_threads))
          return false;
        if (out->search_threads < 1) {
          fputs("error: --search-threads must be at least 1\n", stderr);
          return false;
        }
        break;
      case 'd':
        if (!parse_count(options.optarg, "--projection-depth",
                         &out->exact_letters))
          return false;
        break;
      case 'D':
        out->dense_cache = true;
        break;
      case 'F':
        out->allow_cache_fallback = true;
        break;
      default:
        fprintf(stderr, "error: %s\n", options.errmsg);
        usage(argv[0]);
        return false;
    }
  }

  if (out->dense_cache && out->exact_letters >= 0) {
    fputs("error: --projection-depth cannot be used with --dense-cache\n",
          stderr);
    return false;
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
  return finalize_min_word_length(
      out->letters, min_word_len_given, &out->min_word_len);
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
  bool const include_phrases =
      args.require_completable || !args.words_only;
  DfsClassList classes(&reader, args.letters, args.min_word_len,
                       include_phrases, dictionary_filter);
  dfs_diagnostic(
      "phase 1 complete: %zu entries, %zu classes, %lld trie nodes\n",
      classes.entry_count(), classes.classes().size(),
      (long long) classes.nodes_visited());

  std::vector<bool> completable(
      classes.classes().size(), true);
  if (args.require_completable) {
    size_t const preprocess_threads = resolve_preprocess_threads(
        args.preprocess_threads, args.letters.size());
    dfs_diagnostic(
        "depth %d preprocess threads %zu search threads %d cache %zu\n",
        args.exact_letters, preprocess_threads,
        args.search_threads,
        args.score_cache_bytes / DFS_MIB);
    DfsAnagramSearch search(
        &classes, args.letters, RESTART, reader.count(),
        args.score_cache_bytes, preprocess_threads,
        size_t(args.search_threads),
        /*word_bonus=*/0.0);
    if (!search.find_completable_classes(
            &completable, /*progress_factor=*/1,
            args.allow_cache_fallback, args.dense_cache,
            args.exact_letters))
      return 2;
    if (search.search_threads_used() > 1)
      dfs_diagnostic(
          "phase 2 exact validation parallelism: "
          "%d requested, %zu used\n",
          args.search_threads, search.search_threads_used());
    dfs_diagnostic(
        "phase 2 timing: %.1fs setup, %.1fs exact validation\n",
        search.phase_two_setup_seconds(),
        search.phase_two_search_seconds());
    dfs_diagnostic(
        "phase 2 score cache: %zu bound entries, %zu bound bytes\n",
        search.score_bound_entries(), search.score_bound_bytes_charged());
  }

  double const bonus_factor = multi_word_bonus(args.word_bonus);
  std::vector<RankedMember> ranked;
  ranked.reserve(classes.entry_count());
  for (size_t class_index = 0;
       class_index < classes.classes().size(); ++class_index) {
    if (!completable[class_index]) continue;
    DfsAnagramClass const& anagram_class =
        classes.classes()[class_index];
    for (size_t member_index = 0;
         member_index < anagram_class.members.size(); ++member_index) {
      DfsClassMember const& member =
          anagram_class.members[member_index];
      if (args.words_only && member.word_count != 1) continue;
      double const score = member.word_count > 1
          ? double(member.count) * bonus_factor
          : double(member.count);
      ranked.push_back({score, &member});
    }
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
