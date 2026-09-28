// freqsort.cpp - Rank dictionary words spellable from a letter bag by how
// unusual their letters are.

#include <stdio.h>
#include <string.h>

#include <iostream>
#include <string>

#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "freqsort-impl.h"
#include "optparse.h"
#include "row-input.h"

namespace {

void usage(FILE* out) {
  fputs("usage: freqsort [-i] [-w] [-n|-p|-a] [-v VALUE] [-m COUNT] LETTERS "
        "FILE|- [IGNORE...]\n", out);
  if (out != stdout) return;

  fputs(
      "  find every entry in FILE that can be spelled from LETTERS and rank\n"
      "  them by how unusual their letters are\n"
      "\n"
      "  The letters of each IGNORE are removed from LETTERS first. Each\n"
      "  remaining letter gets a factor: its share of the remaining letters\n"
      "  divided by its share of standard English text, so a letter that is\n"
      "  over-represented in what's left scores high. An entry's score\n"
      "  combines the factors of its letters (see -v). Entries print lowest\n"
      "  score first, so the most letter-hungry come last.\n"
      "\narguments:\n", stdout);
  dfs_help_option("LETTERS", "the available letters; case and non-letters "
                  "are ignored");
  dfs_help_option("FILE", "word list, one entry per line; '-' reads standard "
                  "input. A leading uppercase letter marks a proper noun. "
                  "Comma-separated word pairs (e.g. \"blue,origin\") are "
                  "allowed; only letters are matched, and the line prints "
                  "lowercased as written");
  dfs_help_option("IGNORE", "text whose letters are removed from LETTERS "
                  "before searching; must be spellable from LETTERS");
  fputs("\noptions:\n", stdout);
  dfs_help_option("-i", "sort by letter count first, then by score");
  dfs_help_option("-w", "print only the entries, without their scores");
  dfs_help_option("-p", "only proper nouns (entries starting uppercase)");
  dfs_help_option("-n", "only non-proper nouns");
  dfs_help_option("-a", "both (default)");
  dfs_help_option("-v VALUE", "how per-letter factors combine into a score, "
                  "default 1.1:");
  dfs_help_option("  NUMBER", "sum the factors, multiply the total by "
                  "NUMBER^(letter count), then average over the entry's "
                  "letters; above 1 this favors longer entries. 0 and 1 "
                  "both mean plain addition");
  dfs_help_option("  +NUMBER", "add each factor scaled by the running mean, "
                  "plus NUMBER per letter, then average over the entry's "
                  "letters");
  dfs_help_option("  log", "sum the logs of the factors: how much more "
                  "likely the entry's letters are drawn from the remaining "
                  "letters than from English. Can be negative");
  dfs_help_option("  glog, g", "reduction in the G statistic (log-likelihood "
                  "distance from English letter frequencies) of the "
                  "remaining letters once the entry's letters are removed. "
                  "Can be negative");
  dfs_help_option("  cv", "consonant/vowel ratio of the letters left once "
                  "the entry's letters are removed; y is a consonant. inf "
                  "when only consonants are left");
  dfs_help_option("-m COUNT", "skip entries with fewer than COUNT letters, "
                  "default 4");
  dfs_help_option("-h, --help", "show this help");
}

bool parse_value(char const* in, FreqsortOptions* out) {
  if (strcmp(in, "log") == 0) {
    out->mode = FREQSORT_SCORE_LOG;
    return true;
  }
  if (strcmp(in, "glog") == 0 || strcmp(in, "g") == 0) {
    out->mode = FREQSORT_SCORE_GLOG;
    return true;
  }
  if (strcmp(in, "cv") == 0) {
    out->mode = FREQSORT_SCORE_CV;
    return true;
  }
  char const* number = in;
  if (*number == '+') {
    out->mode = FREQSORT_SCORE_ADD;
    ++number;
  }
  if (!parse_double(number, "-v value", &out->value)) return false;
  if (out->mode == FREQSORT_SCORE_MULTIPLY && out->value == 0) out->value = 1;
  return true;
}

bool parse_args(char* argv[], FreqsortOptions* out, char const** dict,
                bool* help) {
  static struct optparse_long const long_options[] = {
    { "help", 'h', OPTPARSE_NONE },
    { NULL, 'i', OPTPARSE_NONE },
    { NULL, 'w', OPTPARSE_NONE },
    { NULL, 'n', OPTPARSE_NONE },
    { NULL, 'p', OPTPARSE_NONE },
    { NULL, 'a', OPTPARSE_NONE },
    { NULL, 'v', OPTPARSE_REQUIRED },
    { NULL, 'm', OPTPARSE_REQUIRED },
    { NULL, 0, OPTPARSE_NONE },
  };

  struct optparse options;
  optparse_init(&options, argv);

  int option;
  while ((option = optparse_long(&options, long_options, NULL)) != -1) {
    switch (option) {
      case 'h':
        *help = true;
        return true;
      case 'i':
        out->by_length = true;
        break;
      case 'w':
        out->words_only = true;
        break;
      case 'n':
        out->match = FREQSORT_MATCH_NON_PROPER;
        break;
      case 'p':
        out->match = FREQSORT_MATCH_PROPER;
        break;
      case 'a':
        out->match = FREQSORT_MATCH_ALL;
        break;
      case 'v':
        if (!parse_value(options.optarg, out)) return false;
        break;
      case 'm':
        if (!parse_count(options.optarg, "-m value", &out->min_letters))
          return false;
        break;
      default:
        fprintf(stderr, "error: %s\n", options.errmsg);
        return false;
    }
  }

  char const* const letters = optparse_arg(&options);
  *dict = optparse_arg(&options);
  if (letters == NULL || *dict == NULL) return false;

  out->letters = letters;
  for (char* arg; (arg = optparse_arg(&options)) != NULL;)
    out->used_letters += freqsort_letters(arg);
  return true;
}

bool load(std::istream& input, char const* dict, Freqsort* freqsort) {
  std::string line;
  while (std::getline(input, line)) freqsort->add(std::move(line));
  if (!input.eof()) {
    fprintf(stderr, "freqsort: can't read \"%s\"\n", dict);
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  (void) argc;
  FreqsortOptions options;
  char const* dict = NULL;
  bool help = false;
  Freqsort freqsort;
  if (!parse_args(argv, &options, &dict, &help) ||
      (!help && !freqsort.load(options))) {
    usage(stderr);
    return 2;
  }
  if (help) {
    usage(stdout);
    return 0;
  }
  if (!options.used_letters.empty())
    fprintf(stderr, "ignoring: %s\n", options.used_letters.c_str());

  if (!read_input_file("freqsort", dict, [&](std::istream& input) {
        return load(input, dict, &freqsort);
      }))
    return 1;

  if (freqsort.empty()) {
    fputs("no results\n", stderr);
    return 0;
  }
  for (std::string const& line : freqsort.lines())
    printf("%s\n", line.c_str());
  if (fflush(stdout) != 0) {
    fputs("freqsort: can't write output\n", stderr);
    return 1;
  }
  return 0;
}
