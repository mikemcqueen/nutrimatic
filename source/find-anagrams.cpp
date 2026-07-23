#include "index.h"
#include "optparse.h"
#include "search.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

// Accepts any arrangement of a bag of letters, optionally requiring every word
// to be at least min_word_len long.
//
// The bag itself is tracked as a mixed-radix counter over the per-letter
// counts, so the whole 2^n-ish state space is arithmetic rather than stored.
// The word-length constraint rides along as a second digit above it: a state
// is (bag + product * word), where "word" is the length of the word in
// progress clamped at min_len, since every length at or above the minimum
// behaves identically.  With min_len 0 that digit is always 0 and the state
// space is exactly what it was before -m existed.
class AnagramFilter: public SearchFilter {
 public:
  AnagramFilter(char const* letters, int min_word_len) {
    // Requiring words of at least one letter is no constraint at all, since
    // empty words can't occur; normalizing it away keeps the state space small.
    min_len = (min_word_len > 1) ? min_word_len : 0;

    for (size_t i = 0; i < sizeof(count) / sizeof(State); ++i) count[i] = 0;
    while (*letters) ++count[(unsigned char) *letters++];

    product = 1;
    for (size_t i = 0; i < sizeof(count) / sizeof(State); ++i) {
      if (0 == count[i]) {
        value[i] = 0;
      } else if (product * count[i] < product) {
        fputs("anagram too long\n", stderr);
        exit(1);
      } else {
	++count[i];
        value[i] = product;
        product *= count[i];
        count[i] *= value[i];
      }
    }

    if (product > (INT_MAX - 1) / (min_len + 1)) {
      fputs("anagram too long\n", stderr);
      exit(1);
    }
    terminal = product * (min_len + 1);
  }

  bool is_accepting(State state) const {
    return (state == terminal);
  }

  bool has_transition(State from, char ch, State* to) const {
    // Every letter is spoken for once the terminal state is reached, so
    // nothing can follow a complete anagram.
    if (from == terminal) return false;

    State bag = from % product, word = from / product;

    if (ch == ' ') {
      if (word < min_len) return false;  // word is too short: abandon this path
      *to = (bag == product - 1) ? terminal : bag;  // word length resets to 0
      return true;
    }

    State v = value[(unsigned char) ch];
    if (v == 0) return false;

    State next = bag + v;
    if (next % count[(unsigned char) ch] < v) return false;

    if (word < min_len) ++word;
    *to = next + product * word;
    return true;
  }

 private:
  State count[256];
  State value[256];
  State product;
  State terminal;
  State min_len;
};

// Copies the alphanumerics of "in" to "out", dropping spaces so that
// multi-word arguments like "word1 word2" are accepted.  Anything else is an
// error; in particular a space left in the letter bag would be counted into
// AnagramFilter's radix but could never be consumed (has_transition handles
// ' ' before it looks at value[]), so the search would silently never accept.
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

// Removes one occurrence of each letter of "used" from "bag", writing what's
// left to "out" (sorted, which AnagramFilter doesn't care about).  Errors if
// the bag runs out of some letter, or if nothing is left to search for.
static bool subtract_letters(std::string const& bag, std::string const& used,
                             std::string* out) {
  int have[UCHAR_MAX + 1] = { 0 };
  for (size_t i = 0; i < bag.size(); ++i) ++have[(unsigned char) bag[i]];

  for (size_t i = 0; i < used.size(); ++i) {
    unsigned char ch = used[i];
    if (have[ch] == 0) {
      fprintf(stderr, "error: no '%c' left in \"%s\" to use\n",
          ch, bag.c_str());
      return false;
    }
    --have[ch];
  }

  out->clear();
  for (int c = 0; c <= UCHAR_MAX; ++c) out->append(have[c], (char) c);

  if (out->empty()) {
    fprintf(stderr, "error: no letters left after removing used letters\n");
    return false;
  }
  return true;
}

// Parses a non-negative decimal count, e.g. the argument to -m or -p.
static bool parse_count(char const* in, char const* what, int* out) {
  char* end;

  long value = strtol(in, &end, 10);
  if (*in == '\0' || *end != '\0' || value < 0 || value > INT_MAX) {
    fprintf(stderr, "error: %s needs a count, not \"%s\"\n", what, in);
    return false;
  }
  *out = (int) value;
  return true;
}

struct Args {
  char const* index_file;
  std::string letters;  // the bag, with any used letters already removed
  int min_word_len;
  int progress_factor;  // multiplies the 100k-step progress interval
};

static void usage(char const* program) {
  fprintf(stderr,
      "usage: %s input.index letters"
      " [-u used-letters] [-m min-word-length] [-p progress-factor]\n",
      program);
}

static struct optparse_long const long_options[] = {
  { "used-letters", 'u', OPTPARSE_REQUIRED },
  { "min-word-length", 'm', OPTPARSE_REQUIRED },
  { "progress-factor", 'p', OPTPARSE_REQUIRED },
  { NULL, 0, OPTPARSE_NONE },
};

static bool parse_args(char *argv[], Args* out) {
  out->min_word_len = 0;
  out->progress_factor = 1;

  struct optparse options;
  optparse_init(&options, argv);

  std::string used;
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
        break;
      case 'p':
        if (!parse_count(options.optarg, "--progress-factor",
                         &out->progress_factor))
          return false;
        if (out->progress_factor < 1) {
          fputs("error: --progress-factor must be at least 1", stderr);
          return false;
        }
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

  std::string bag, remove;
  if (!clean_letters(letters, "letters", &bag)) return false;
  if (!clean_letters(used.c_str(), "used letters", &remove)) return false;

  out->index_file = index_file;
  if (!subtract_letters(bag, remove, &out->letters)) return false;

  if (out->min_word_len > (int) out->letters.size()) {
    fprintf(stderr,
        "error: no word of %d letters fits in the %zu left in \"%s\"\n",
        out->min_word_len, out->letters.size(), out->letters.c_str());
    return false;
  }
  return true;
}

int main(int argc, char *argv[]) {
  Args args;
  if (!parse_args(argv, &args)) return 2;

  FILE *fp = fopen(args.index_file, "rb");
  if (fp == NULL) {
    fprintf(stderr, "error: can't open \"%s\"\n", args.index_file);
    return 1;
  }

  IndexReader reader(fp);
  AnagramFilter filter(args.letters.c_str(), args.min_word_len);
  SearchDriver driver(&reader, &filter, 0, 1e-6);
  PrintAll(&driver, stderr, args.progress_factor);
  return 0;
}
