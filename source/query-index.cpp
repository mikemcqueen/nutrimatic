// Prints the highest-corpus-frequency words/phrases makeable from a subset
// of a given letter bag. By default this is phase 1 of dfs-anagrams
// (DfsClassList) only. --require-completable adds shared phase-2 feasibility
// filtering. --score instead scores an exact sequence of index entries.

#include "dfs-class-list.h"
#include "dfs-cli-args.h"
#include "dfs-diagnostic.h"
#include "dfs-score.h"
#include "dfs-search-stats.h"
#include "dfs-search.h"
#include "index.h"
#include "optparse.h"

#include <ctype.h>
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
  std::string score_sequence;
  int min_word_len;
  int top;
  bool words_only;
  double segment_penalty;
  double word_bonus;
  bool require_completable;
  int search_threads;
  bool score;
  char const* score_incompatible_option;
};

static void usage(char const* program) {
  fprintf(stderr,
      "usage: %s input.index letters"
      " [--score] [-P|--segment-penalty P] [--word-bonus N]"
      " [-u used-letters] [--dict PATH] [-m min-word-length] [-n top]"
      " [-w|--words-only] [--require-completable]"
      " [-S|--search-threads N]\n"
      "  --score treats letters as a comma-separated sequence of exact index\n"
      "    entries and prints its DFS-model score\n"
      "  -P, --segment-penalty P divides the score by P for each selected"
      " index entry after the first; P must be at least 1 and defaults to"
      " %.0f\n"
      "    k entries score as product(count) / (corpus-total * P)^(k-1)\n"
      "  -m defaults to %d; 0 for no minimum\n"
      "  -n defaults to %d; 0 for no limit\n"
      "  --dict PATH filters entries to words in the dictionary\n"
      "  -w, --words-only excludes multi-word phrases\n"
      "  --word-bonus N boosts multi-word members by %.0f^N;\n"
      "    defaults to %g (no boost)\n"
      "  --require-completable drops classes whose removal leaves a\n"
      "    remainder phase 2 can't fully turn into an anagram (subject to\n"
      "    -m), using shared exact validation without a score cache\n"
      "  -S, --search-threads defaults to 1\n",
      program, DFS_DEFAULT_SEGMENT_PENALTY, DFS_DEFAULT_MIN_WORD_LEN,
      DEFAULT_TOP, DFS_WORD_BONUS_BASE, DEFAULT_WORD_BONUS);
}

static int const OPT_DICT = 256;
static int const OPT_WORD_BONUS = 257;
static int const OPT_REQUIRE_COMPLETABLE = 258;
static int const OPT_SCORE = 259;

static struct optparse_long const long_options[] = {
  { "used-letters", 'u', OPTPARSE_REQUIRED },
  { "dict", OPT_DICT, OPTPARSE_REQUIRED },
  { "min-word-length", 'm', OPTPARSE_REQUIRED },
  { "top", 'n', OPTPARSE_REQUIRED },
  { "words-only", 'w', OPTPARSE_NONE },
  { "segment-penalty", 'P', OPTPARSE_REQUIRED },
  { "word-bonus", OPT_WORD_BONUS, OPTPARSE_REQUIRED },
  { "score", OPT_SCORE, OPTPARSE_NONE },
  { "require-completable", OPT_REQUIRE_COMPLETABLE, OPTPARSE_NONE },
  { "search-threads", 'S', OPTPARSE_REQUIRED },
  { NULL, 0, OPTPARSE_NONE },
};

static void mark_score_incompatible(Args* args, char const* option) {
  if (args->score_incompatible_option == NULL)
    args->score_incompatible_option = option;
}

static bool parse_args(char* argv[], Args* out) {
  out->dictionary_file = NULL;
  out->min_word_len = DFS_DEFAULT_MIN_WORD_LEN;
  out->top = DEFAULT_TOP;
  out->words_only = false;
  out->segment_penalty = DFS_DEFAULT_SEGMENT_PENALTY;
  out->word_bonus = DEFAULT_WORD_BONUS;
  out->require_completable = false;
  out->search_threads = 1;
  out->score = false;
  out->score_incompatible_option = NULL;

  struct optparse options;
  optparse_init(&options, argv);

  std::string used;
  bool min_word_len_given = false;
  int opt;
  while ((opt = optparse_long(&options, long_options, NULL)) != -1) {
    switch (opt) {
      case 'u':
        used += options.optarg;
        mark_score_incompatible(out, "--used-letters");
        break;
      case OPT_DICT:
        out->dictionary_file = options.optarg;
        mark_score_incompatible(out, "--dict");
        break;
      case 'm':
        if (!parse_count(options.optarg, "--min-word-length",
                         &out->min_word_len))
          return false;
        min_word_len_given = true;
        mark_score_incompatible(out, "--min-word-length");
        break;
      case 'n':
        if (!parse_count(options.optarg, "--top", &out->top))
          return false;
        mark_score_incompatible(out, "--top");
        break;
      case 'w':
        out->words_only = true;
        mark_score_incompatible(out, "--words-only");
        break;
      case 'P':
        if (!parse_segment_penalty(
                options.optarg, &out->segment_penalty))
          return false;
        break;
      case OPT_WORD_BONUS:
        if (!parse_double(options.optarg, "--word-bonus", &out->word_bonus))
          return false;
        break;
      case OPT_SCORE:
        out->score = true;
        break;
      case OPT_REQUIRE_COMPLETABLE:
        out->require_completable = true;
        mark_score_incompatible(out, "--require-completable");
        break;
      case 'S':
        if (!parse_count(options.optarg, "--search-threads",
                         &out->search_threads))
          return false;
        if (out->search_threads < 1) {
          fputs("error: --search-threads must be at least 1\n", stderr);
          return false;
        }
        mark_score_incompatible(out, "--search-threads");
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
  out->index_file = index_file;

  if (out->score) {
    if (out->score_incompatible_option != NULL) {
      fprintf(stderr, "error: %s cannot be used with --score\n",
              out->score_incompatible_option);
      return false;
    }
    out->score_sequence = letters;
    return true;
  }

  std::string bag;
  std::string remove;
  if (!clean_letters(letters, "letters", &bag)) return false;
  if (!clean_letters(used.c_str(), "used letters", &remove)) return false;
  if (!subtract_letters(bag, remove, &out->letters)) return false;
  if (!check_bag_length(out->letters)) return false;
  return finalize_min_word_length(
      out->letters, min_word_len_given, &out->min_word_len);
}

// With the default --word-bonus 0 the two multi_word groups collapse and
// log(count) descending is integer count descending: over this corpus's count
// range log is injective in double, so an integer compare reproduces the
// score order exactly, ties included, and the text tie-break still decides
// them.
static bool count_order(DfsPackedMember const& a, DfsPackedMember const& b) {
  if (a.count != b.count) return a.count > b.count;
  return dfs_member_text_compare(a, b) < 0;
}

static bool is_phrase(DfsPackedMember const& member) {
  return member.word_count > 1;
}

// Rewriting this as count * exp(bonus) versus count would round differently
// from log(count) + bonus, breaking exact ties differently and so changing the
// text tie-break. It stays the identical expression, applied O(E) times in the
// merge instead of O(E log E) times in a sort.
struct ScoreOrder {
  DfsScoreModel const* model;

  bool operator()(DfsPackedMember const& a, DfsPackedMember const& b) const {
    double const a_score =
        model->first_segment_log_score(a.count, is_phrase(a));
    double const b_score =
        model->first_segment_log_score(b.count, is_phrase(b));
    if (a_score != b_score) return a_score > b_score;
    return dfs_member_text_compare(a, b) < 0;
  }
};

static bool parse_score_sequence(
    std::string const& sequence, std::vector<std::string>* entries) {
  size_t start = 0;
  for (;;) {
    size_t const comma = sequence.find(',', start);
    size_t first = start;
    size_t last = comma == std::string::npos ? sequence.size() : comma;
    while (first < last && isspace((unsigned char) sequence[first])) ++first;
    while (last > first && isspace((unsigned char) sequence[last - 1])) --last;

    std::string const entry = sequence.substr(first, last - first);
    if (entry.empty()) {
      fputs("error: empty entry in --score sequence\n", stderr);
      return false;
    }
    for (size_t i = 0; i < entry.size(); ++i) {
      char const ch = entry[i];
      bool const letter_or_digit =
          (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
      bool const single_internal_space =
          ch == ' ' && i != 0 && i + 1 != entry.size() &&
          entry[i - 1] != ' ';
      if (!letter_or_digit && !single_internal_space) {
        fprintf(stderr, "error: malformed index entry \"%s\"\n",
                entry.c_str());
        return false;
      }
    }
    entries->push_back(entry);
    if (comma == std::string::npos) return true;
    start = comma + 1;
  }
}

static bool print_sequence_score(
    IndexReader const& reader, Args const& args,
    std::vector<std::string> const& entries) {
  std::vector<int64_t> counts;
  counts.reserve(entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    int64_t count;
    // Aggregate, not exact: phase 1 scores an entry by the count on its
    // trailing-space node, which includes every longer phrase continuing it.
    // Using the exact residual here would silently disagree with the score
    // dfs-anagrams prints for the same sequence.
    if (!reader.aggregate_entry_count(entries[i], &count)) {
      fprintf(stderr, "error: index has no entry \"%s\"\n",
              entries[i].c_str());
      return false;
    }
    counts.push_back(count);
  }

  DfsScoreModel const model(
      args.segment_penalty, reader.count(), args.word_bonus);
  double log_score = model.first_segment_log_score(
      counts[0], entries[0].find(' ') != std::string::npos);
  for (size_t i = 1; i < entries.size(); ++i)
    log_score = model.append_segment_log_score(
        log_score, counts[i],
        entries[i].find(' ') != std::string::npos);

  printf("%#.4g %s\n", model.displayed_score(log_score),
         args.score_sequence.c_str());
  return true;
}

int main(int argc, char* argv[]) {
  dfs_reset_diagnostic_clock();
  dfs_set_diagnostic_stream(stderr);

  Args args;
  if (!parse_args(argv, &args)) return 2;

  if (args.score) {
    std::vector<std::string> entries;
    if (!parse_score_sequence(args.score_sequence, &entries)) return 2;

    FILE* fp = fopen(args.index_file, "rb");
    if (fp == NULL) {
      fprintf(stderr, "error: can't open \"%s\"\n", args.index_file);
      return 1;
    }
    IndexReader reader(fp);
    return print_sequence_score(reader, args, entries) ? 0 : 2;
  }

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

  std::vector<bool> completable(classes.classes().size(), true);
  if (args.require_completable) {
    dfs_diagnostic(
        "search threads %d cache 0 segment penalty %.17g\n",
        args.search_threads, args.segment_penalty);
    DfsAnagramSearch search(
        &classes, args.letters, args.segment_penalty, reader.count(),
        /*score_cache_bytes=*/0, /*preprocess_threads=*/1,
        size_t(args.search_threads),
        /*word_bonus=*/0.0);
    DfsSearchStats stats;
    if (!search.find_completable_classes(
            &completable, &stats, /*progress_factor=*/1,
            /*allow_cache_fallback=*/true, /*exact_letters=*/-1))
      return 2;
    DfsSearchStats::Execution const& run = stats.execution;
    DfsSearchStats::Bounds const& bounds = stats.bounds;
    if (run.search_threads > 1)
      dfs_diagnostic(
          "phase 2 exact validation parallelism: "
          "%d requested, %zu used\n",
          args.search_threads, run.search_threads);
    dfs_diagnostic(
        "phase 2 timing: %.1fs setup, %.1fs exact validation\n",
        run.setup_seconds, run.search_seconds);
    dfs_diagnostic(
        "phase 2 score cache: %zu bound entries, %zu bound bytes\n",
        bounds.entries, bounds.bytes_charged);
  }

  DfsScoreModel const model(
      args.segment_penalty, reader.count(), args.word_bonus);
  // The class -> member grouping has no reader left: phase 2 touched it once at
  // setup and its search is already destroyed, and printing needs only each
  // member's count and text.
  DfsMemberSpan const survivors =
      classes.retain_members(completable, args.words_only);
  size_t const top = args.top == 0
      ? survivors.count
      : std::min(survivors.count, size_t(args.top));

  DfsPackedMember* const first = survivors.data;
  DfsPackedMember* const last = first + survivors.count;
  auto const print_row = [&](DfsPackedMember const& row) {
    if (args.word_bonus == 0.0)
      printf("%lld %.*s\n", (long long) row.count,
             int(row.text_length), row.text);
    else
      printf("%#.4g %.*s\n",
             model.displayed_score(model.first_segment_log_score(
                 row.count, is_phrase(row))),
             int(row.text_length), row.text);
  };

  if (args.word_bonus == 0.0) {
    std::partial_sort(first, first + top, last, count_order);
    for (size_t i = 0; i < top; ++i) print_row(first[i]);
  } else {
    DfsPackedMember* const mid = std::partition(first, last, is_phrase);
    size_t const phrase_top = std::min(top, size_t(mid - first));
    size_t const word_top = std::min(top, size_t(last - mid));
    std::partial_sort(first, first + phrase_top, mid, count_order);
    std::partial_sort(mid, mid + word_top, last, count_order);

    ScoreOrder const order = { &model };
    size_t phrase = 0;
    size_t word = 0;
    for (size_t printed = 0; printed < top; ++printed) {
      bool take_phrase;
      if (phrase == phrase_top)
        take_phrase = false;
      else if (word == word_top)
        take_phrase = true;
      else
        take_phrase = order(first[phrase], mid[word]);
      print_row(take_phrase ? first[phrase++] : mid[word++]);
    }
  }
  return 0;
}
