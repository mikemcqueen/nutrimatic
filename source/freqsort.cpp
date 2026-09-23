// freqsort.cpp - Rank dictionary words spellable from a letter bag by how
// unusual their letters are.

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "optparse.h"
#include "row-input.h"

namespace {

enum MatchType { MATCH_ALL, MATCH_PROPER, MATCH_NON_PROPER };
enum ScoreMode { SCORE_MULTIPLY, SCORE_ADD, SCORE_LOG, SCORE_GLOG, SCORE_CV };

double const ENGLISH_PERCENT[26] = {
  8.167, 1.492, 2.782, 4.253, 12.702, 2.228, 2.015, 6.094, 6.966,
  0.153, 0.772, 4.025, 2.406, 6.749, 7.507, 1.929, 0.095, 5.987,
  6.327, 9.056, 2.758, 0.978, 2.360, 0.150, 1.974, 0.074,
};

struct Counts {
  int values[26] = { 0 };
};

struct Args {
  bool by_length = false;
  bool words_only = false;
  MatchType match = MATCH_ALL;
  ScoreMode mode = SCORE_MULTIPLY;
  double value = 1.1;
  int min_letters = 4;
  Counts letters;
  int total = 0;
  char const* dict = NULL;
};

struct Entry {
  std::string line;
  double score;
  int letters;
};

void usage(FILE* out) {
  fputs("usage: freqsort [-i] [-w] [-n|-p|-a] [-v VALUE] [-m COUNT] LETTERS "
        "DICT [IGNORE...]\n", out);
  if (out != stdout) return;

  fputs(
      "  find every entry in DICT that can be spelled from LETTERS and rank\n"
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
  dfs_help_option("DICT", "word list, one entry per line; '-' reads standard "
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

std::string lower_letters(char const* in) {
  std::string out;
  for (; *in != '\0'; ++in) {
    int const ch = tolower((unsigned char) *in);
    if (ch >= 'a' && ch <= 'z') out.push_back(char(ch));
  }
  return out;
}

bool parse_value(char const* in, Args* out) {
  if (strcmp(in, "log") == 0) {
    out->mode = SCORE_LOG;
    return true;
  }
  if (strcmp(in, "glog") == 0 || strcmp(in, "g") == 0) {
    out->mode = SCORE_GLOG;
    return true;
  }
  if (strcmp(in, "cv") == 0) {
    out->mode = SCORE_CV;
    return true;
  }
  char const* number = in;
  if (*number == '+') {
    out->mode = SCORE_ADD;
    ++number;
  }
  if (!parse_double(number, "-v value", &out->value)) return false;
  if (out->mode == SCORE_MULTIPLY && out->value == 0) out->value = 1;
  return true;
}

bool parse_args(char* argv[], Args* out, bool* help) {
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
        out->match = MATCH_NON_PROPER;
        break;
      case 'p':
        out->match = MATCH_PROPER;
        break;
      case 'a':
        out->match = MATCH_ALL;
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
  out->dict = optparse_arg(&options);
  if (letters == NULL || out->dict == NULL) return false;

  std::string ignore;
  for (char* arg; (arg = optparse_arg(&options)) != NULL;)
    ignore += lower_letters(arg);

  std::string remaining;
  if (!subtract_letters(lower_letters(letters), ignore, &remaining))
    return false;
  if (!ignore.empty()) fprintf(stderr, "ignoring: %s\n", ignore.c_str());
  for (char ch : remaining) ++out->letters.values[ch - 'a'];
  out->total = int(remaining.size());
  return true;
}

double g_statistic(Counts const& counts, int total) {
  double g = 0;
  for (int i = 0; i < 26; ++i) {
    int const observed = counts.values[i];
    if (observed == 0) continue;
    double const expected = total * ENGLISH_PERCENT[i] / 100;
    g += observed * log(observed / expected);
  }
  return 2 * g;
}

double cv_ratio(Counts const& counts) {
  int vowels = 0;
  int consonants = 0;
  for (int i = 0; i < 26; ++i) {
    if (strchr("aeiou", 'a' + i) != NULL)
      vowels += counts.values[i];
    else
      consonants += counts.values[i];
  }
  if (consonants == 0) return 0;
  if (vowels == 0) return INFINITY;
  return double(consonants) / vowels;
}

bool score(Args const& args, std::string const& word, double* out) {
  Counts left = args.letters;
  double sum = 0;
  double mean = 1;
  double log_sum = 0;
  int count = 0;
  for (char c : word) {
    if ((unsigned char) c >= 0x80) return false;
    if (c < 'a' || c > 'z') continue;
    int& available = left.values[c - 'a'];
    if (available == 0) return false;
    double const factor = (100.0 * available / args.total) /
        ENGLISH_PERCENT[c - 'a'];
    --available;

    if (args.mode == SCORE_LOG) {
      log_sum += log(factor);
    } else if (args.mode == SCORE_ADD) {
      sum += factor * mean + args.value;
    } else {
      sum += factor;
    }
    ++count;
    if (args.mode == SCORE_ADD) mean = sum / count;
  }

  if (args.mode == SCORE_LOG) {
    *out = log_sum;
  } else if (args.mode == SCORE_CV) {
    *out = cv_ratio(left);
  } else if (args.mode == SCORE_GLOG) {
    *out = g_statistic(args.letters, args.total) -
        g_statistic(left, args.total - count);
  } else {
    if (args.mode == SCORE_MULTIPLY) sum *= pow(args.value, count);
    *out = count == 0 ? sum : sum / count;
  }
  return true;
}

int count_letters(std::string const& word) {
  int count = 0;
  for (char c : word)
    if (c >= 'a' && c <= 'z') ++count;
  return count;
}

bool load(std::istream& input, Args const& args,
          std::map<std::string, Entry>* entries) {
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) continue;
    bool const proper = isupper((unsigned char) line[0]);
    if ((args.match == MATCH_PROPER && !proper) ||
        (args.match == MATCH_NON_PROPER && proper))
      continue;

    for (char& c : line) c = char(tolower((unsigned char) c));
    int const letters = count_letters(line);
    if (letters < args.min_letters) continue;

    double value;
    if (!score(args, line, &value)) continue;
    char rounded[32];
    snprintf(rounded, sizeof rounded, "%.4e", value);
    (*entries)[line] = { line, strtod(rounded, NULL), letters };
  }
  if (!input.eof()) {
    fprintf(stderr, "freqsort: can't read \"%s\"\n", args.dict);
    return false;
  }
  return true;
}

void print(Args const& args, std::map<std::string, Entry> const& entries) {
  std::vector<Entry> sorted;
  sorted.reserve(entries.size());
  size_t width = 0;
  for (auto const& [line, entry] : entries) {
    sorted.push_back(entry);
    width = std::max(width, line.size());
  }
  std::stable_sort(sorted.begin(), sorted.end(),
      [&args](Entry const& a, Entry const& b) {
        if (args.by_length && a.letters != b.letters)
          return a.letters < b.letters;
        return a.score < b.score;
      });

  for (Entry const& entry : sorted) {
    if (args.words_only)
      printf("%s\n", entry.line.c_str());
    else
      printf("%-*s  %#.5g\n", int(width), entry.line.c_str(), entry.score);
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

  std::map<std::string, Entry> entries;
  if (!read_input_file("freqsort", args.dict, [&](std::istream& input) {
        return load(input, args, &entries);
      }))
    return 1;

  if (entries.empty()) {
    fputs("no results\n", stderr);
    return 0;
  }
  print(args, entries);
  if (fflush(stdout) != 0) {
    fputs("freqsort: can't write output\n", stderr);
    return 1;
  }
  return 0;
}
