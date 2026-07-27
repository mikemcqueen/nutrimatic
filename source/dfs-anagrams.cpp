#include "dfs-class-list.h"
#include "dfs-diagnostic.h"
#include "dfs-output.h"
#include "dfs-search.h"
#include "index.h"
#include "optparse.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <string>
#include <thread>
#include <vector>

static bool clean_letters(char const* in, char const* what, std::string* out) {
  for (; *in != '\0'; ++in) {
    if (*in == ' ') continue;
    if ((*in < 'a' || *in > 'z') && (*in < '0' || *in > '9')) {
      fprintf(stderr, "error: bad character '%c' in %s\n", *in, what);
      return false;
    }
    out->push_back(*in);
  }
  return true;
}

static bool subtract_letters(std::string const& bag,
                             std::string const& used,
                             std::string* out) {
  int have[UCHAR_MAX + 1] = { 0 };
  for (size_t i = 0; i < bag.size(); ++i)
    ++have[(unsigned char) bag[i]];

  for (size_t i = 0; i < used.size(); ++i) {
    unsigned char const ch = (unsigned char) used[i];
    if (have[ch] == 0) {
      fprintf(stderr, "error: no '%c' left in \"%s\" to use\n",
              ch, bag.c_str());
      return false;
    }
    --have[ch];
  }

  out->clear();
  for (int ch = 0; ch <= UCHAR_MAX; ++ch)
    out->append(size_t(have[ch]), char(ch));

  if (out->empty()) {
    fputs("error: no letters left after removing used letters\n", stderr);
    return false;
  }
  return true;
}

static bool parse_count(char const* in, char const* what, int* out) {
  char* end;
  long const value = strtol(in, &end, 10);
  if (*in == '\0' || *end != '\0' || value < 0 || value > INT_MAX) {
    fprintf(stderr, "error: %s needs a count, not \"%s\"\n", what, in);
    return false;
  }
  *out = int(value);
  return true;
}

static int const DEFAULT_MIN_WORD_LEN = 4;
static int const DEFAULT_TOP = 10000;
static size_t const DEFAULT_SCORE_CACHE_MIB = 64;
static unsigned int const DEFAULT_MAX_PREPROCESS_THREADS = 20;
static size_t const MIB = size_t(1024) * size_t(1024);

static bool parse_mib(char const* in, char const* what, size_t* out) {
  if (*in == '\0' || *in == '-') {
    fprintf(stderr, "error: %s needs a count, not \"%s\"\n", what, in);
    return false;
  }
  errno = 0;
  char* end;
  unsigned long long const value = strtoull(in, &end, 10);
  if (*end != '\0' || errno == ERANGE ||
      value > static_cast<unsigned long long>(SIZE_MAX / MIB)) {
    fprintf(stderr, "error: %s needs a count, not \"%s\"\n", what, in);
    return false;
  }
  *out = size_t(value) * MIB;
  return true;
}

struct Args {
  char const* index_file;
  std::string letters;
  int min_word_len;
  int max_words;
  int top;
  int progress_factor;
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
      " [-u used-letters] [-m min-word-length] [-n top]"
      " [-p progress-factor] [--cache-size MiB]"
      " [--preprocess-threads N] [--search-threads N]"
      " [-d projection-depth]"
      " [-D|--dense-cache] [-F|--allow-cache-fallback]\n"
      "  -m defaults to %d; 0 for no minimum\n"
      "  -n defaults to %d\n"
      "  -C, --cache-size defaults to %zu MiB; 0 disables it with -F\n"
      "  --preprocess-threads defaults to 0: automatic for 26+ letters;"
      " 1 disables it\n"
      "  -S, --search-threads defaults to 1\n"
      "  -d, --projection-depth keeps this many rarest letter types exact in"
      " the projected cache; the default is the largest depth that fits -C\n"
      "  -D, --dense-cache requests an exact dense score cache instead of"
      " the default projected dense cache\n"
      "  -F, --allow-cache-fallback allows score-cache fallback when the"
      " requested table does not fit\n",
      program, DEFAULT_MIN_WORD_LEN, DEFAULT_TOP,
      DEFAULT_SCORE_CACHE_MIB);
}

static struct optparse_long const long_options[] = {
  { "used-letters", 'u', OPTPARSE_REQUIRED },
  { "min-word-length", 'm', OPTPARSE_REQUIRED },
  { "top", 'n', OPTPARSE_REQUIRED },
  { "progress-factor", 'p', OPTPARSE_REQUIRED },
  { "cache-size", 'C', OPTPARSE_REQUIRED },
  { "preprocess-threads", 'T', OPTPARSE_REQUIRED },
  { "search-threads", 'S', OPTPARSE_REQUIRED },
  { "projection-depth", 'd', OPTPARSE_REQUIRED },
  { "dense-cache", 'D', OPTPARSE_NONE },
  { "allow-cache-fallback", 'F', OPTPARSE_NONE },
  { NULL, 0, OPTPARSE_NONE },
};

static bool parse_args(char* argv[], Args* out) {
  out->min_word_len = DEFAULT_MIN_WORD_LEN;
  out->top = DEFAULT_TOP;
  out->progress_factor = 1;
  out->score_cache_bytes = DEFAULT_SCORE_CACHE_MIB * MIB;
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
      case 'p':
        if (!parse_count(options.optarg, "--progress-factor",
                         &out->progress_factor))
          return false;
        if (out->progress_factor < 1) {
          fputs("error: --progress-factor must be at least 1\n", stderr);
          return false;
        }
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
      case 'F':
        out->allow_cache_fallback = true;
        break;
      case 'D':
        out->dense_cache = true;
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

  if (!min_word_len_given &&
      out->min_word_len > int(out->letters.size()))
    out->min_word_len = int(out->letters.size());

  if (out->min_word_len > int(out->letters.size())) {
    fprintf(stderr,
        "error: no word of %d letters fits in the %zu left in \"%s\"\n",
        out->min_word_len, out->letters.size(), out->letters.c_str());
    return false;
  }

  out->max_words = out->min_word_len > 1
      ? int(out->letters.size()) / out->min_word_len
      : 0;
  return true;
}

int main(int argc, char* argv[]) {
  dfs_reset_diagnostic_clock();

  Args args;
  if (!parse_args(argv, &args)) return 2;

  size_t preprocess_threads = size_t(args.preprocess_threads);
  if (preprocess_threads == 0) {
    preprocess_threads = 1;
    if (args.letters.size() >= 26) {
      unsigned int const available = std::thread::hardware_concurrency();
      if (available > 1) {
        preprocess_threads = size_t(std::min(
            available, DEFAULT_MAX_PREPROCESS_THREADS));
      }
    }
  }
  dfs_diagnostic(
      stderr, "depth %d top %d threads %zu search threads %d cache %zu\n",
      args.exact_letters, args.top, preprocess_threads, args.search_threads,
      args.score_cache_bytes / MIB);

  FILE* fp = fopen(args.index_file, "rb");
  if (fp == NULL) {
    fprintf(stderr, "error: can't open \"%s\"\n", args.index_file);
    return 1;
  }

  if (args.max_words > 0) {
    dfs_diagnostic(
        stderr, "%zu letters \"%s\", words of %d+, at most %d word%s\n",
        args.letters.size(), args.letters.c_str(), args.min_word_len,
        args.max_words, args.max_words == 1 ? "" : "s");
  } else {
    dfs_diagnostic(stderr, "%zu letters \"%s\", no minimum word length\n",
                   args.letters.size(), args.letters.c_str());
  }

  IndexReader reader(fp);
  DfsClassList classes(&reader, args.letters, args.min_word_len);
  dfs_diagnostic(
      stderr, "phase 1 complete: %zu entries, %zu classes, %lld trie nodes\n",
      classes.entry_count(), classes.classes().size(),
      (long long) classes.nodes_visited());
  fflush(stderr);

  double const restart = 1e-6;
  DfsAnagramSearch search(
      &classes, args.letters, restart, reader.count(),
      args.score_cache_bytes, preprocess_threads,
      size_t(args.search_threads));
  DfsTopN output(&classes, size_t(args.top));
  if (!search.run(args.top == 0 ? NULL : &output, stderr,
                  args.progress_factor, args.allow_cache_fallback,
                  args.dense_cache, args.exact_letters))
    return 2;
  dfs_diagnostic(
      stderr, "phase 2 timing: %.1fs setup, %.1fs search, "
      "%llu successful bound transitions, %llu nextafter calls\n",
      search.phase_two_setup_seconds(),
      search.phase_two_search_seconds(),
      (unsigned long long) search.score_bound_transitions(),
      (unsigned long long) search.score_bound_nextafter_calls());
  if (search.score_bound_mode() ==
      DfsAnagramSearch::SCORE_BOUND_PROJECTED)
    dfs_diagnostic(
        stderr, "phase 2 projected work: %llu candidate tests, "
        "%llu fitting transitions\n",
        (unsigned long long) search.score_bound_candidate_tests(),
        (unsigned long long) search.score_bound_fitting_transitions());
  if (search.length_certificate_enabled()) {
    dfs_diagnostic(
        stderr, "phase 2 length certificate: %s, %zu table bytes\n",
        search.length_certificate_skipping() ? "active" : "shadow",
        search.length_certificate_table_bytes());
    dfs_diagnostic(
        stderr, "phase 2   %llu group tests, %llu rejected, "
        "%llu class scans kept, %llu skipped\n",
        (unsigned long long) search.length_certificate_group_tests(),
        (unsigned long long) search.length_certificate_group_rejects(),
        (unsigned long long) search.length_certificate_scans_kept(),
        (unsigned long long) search.length_certificate_scans_skipped());
  }
  if (search.search_threads_used() > 1)
    dfs_diagnostic(
        stderr, "phase 2 search parallelism: %d requested, %zu used, "
        "%llu tasks\n",
        args.search_threads, search.search_threads_used(),
        (unsigned long long) search.search_tasks_generated());
  dfs_diagnostic(
      stderr, "phase 2 score cache: %zu bound entries, %zu bound bytes\n",
      search.score_bound_entries(), search.score_bound_bytes_charged());
  dfs_diagnostic(
      stderr, "phase 2 complete: %lld nodes, %lld solutions, "
      "%zu spellings expanded, %zu retained\n",
      (long long) search.nodes_visited(),
      (long long) search.solutions_found(),
      output.spellings_expanded(), output.size());
  fflush(stderr);

  std::vector<DfsSpelling> const results = output.take_sorted_results();
  for (size_t i = 0; i < results.size(); ++i)
    printf("%#.4g %s\n", exp(results[i].log_score),
           results[i].text.c_str());
  return 0;
}
