#include "dfs-class-list.h"
#include "dfs-output.h"
#include "dfs-search.h"
#include "index.h"
#include "optparse.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <string>
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
static size_t const DEFAULT_CANDIDATE_CACHE_MIB = 64;
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
  size_t candidate_cache_bytes;
};

static void usage(char const* program) {
  fprintf(stderr,
      "usage: %s input.index letters"
      " [-u used-letters] [-m min-word-length] [-n top]"
      " [-p progress-factor] [--candidate-cache-mib MiB]\n"
      "  -m defaults to %d; 0 for no minimum\n"
      "  -n defaults to %d\n"
      "  --candidate-cache-mib defaults to %zu; 0 disables it\n",
      program, DEFAULT_MIN_WORD_LEN, DEFAULT_TOP,
      DEFAULT_CANDIDATE_CACHE_MIB);
}

static struct optparse_long const long_options[] = {
  { "used-letters", 'u', OPTPARSE_REQUIRED },
  { "min-word-length", 'm', OPTPARSE_REQUIRED },
  { "top", 'n', OPTPARSE_REQUIRED },
  { "progress-factor", 'p', OPTPARSE_REQUIRED },
  { "candidate-cache-mib", 'C', OPTPARSE_REQUIRED },
  { NULL, 0, OPTPARSE_NONE },
};

static bool parse_args(char* argv[], Args* out) {
  out->min_word_len = DEFAULT_MIN_WORD_LEN;
  out->top = DEFAULT_TOP;
  out->progress_factor = 1;
  out->candidate_cache_bytes = DEFAULT_CANDIDATE_CACHE_MIB * MIB;

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
        if (!parse_mib(options.optarg, "--candidate-cache-mib",
                       &out->candidate_cache_bytes))
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
  Args args;
  if (!parse_args(argv, &args)) return 2;

  FILE* fp = fopen(args.index_file, "rb");
  if (fp == NULL) {
    fprintf(stderr, "error: can't open \"%s\"\n", args.index_file);
    return 1;
  }

  if (args.max_words > 0) {
    fprintf(stderr, "# %zu letters \"%s\", words of %d+, at most %d word%s\n",
            args.letters.size(), args.letters.c_str(), args.min_word_len,
            args.max_words, args.max_words == 1 ? "" : "s");
  } else {
    fprintf(stderr, "# %zu letters \"%s\", no minimum word length\n",
            args.letters.size(), args.letters.c_str());
  }

  IndexReader reader(fp);
  DfsClassList classes(&reader, args.letters, args.min_word_len);
  fprintf(stderr,
          "# phase 1 complete: %zu entries, %zu classes, %lld trie nodes\n",
          classes.entry_count(), classes.classes().size(),
          (long long) classes.nodes_visited());
  fflush(stderr);

  double const restart = 1e-6;
  DfsAnagramSearch search(
      &classes, args.letters, restart, reader.count(),
      args.candidate_cache_bytes);
  DfsTopN output(&classes, size_t(args.top));
  search.run(args.top == 0 ? NULL : &output, stderr, args.progress_factor);
  fprintf(stderr,
          "# phase 2 timing: %.6f s setup, %.6f s search, "
          "%llu successful bound transitions, %llu nextafter calls\n",
          search.phase_two_setup_seconds(),
          search.phase_two_search_seconds(),
          (unsigned long long) search.score_bound_transitions(),
          (unsigned long long) search.score_bound_nextafter_calls());
  fprintf(stderr,
          "# phase 2 complete: %lld nodes, %lld solutions, "
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
