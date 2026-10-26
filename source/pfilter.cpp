#include <stdio.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "classified.h"
#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "letter-bag.h"
#include "log.h"
#include "optparse.h"
#include "pfilter-impl.h"
#include "row-input.h"
#include "workflow-paths.h"

constexpr char PROGRAM[] = "pfilter";

namespace {

int const OPT_NO_DICT = 256;
int const OPT_CV = 257;
int const OPT_CSV = 258;
int const OPT_EXACT = 259;
int const OPT_NO_NOPAIRS = 260;
int const OPT_REQUIRED_WORDS = 261;

struct Args {
  PfilterOptions options;
  std::optional<std::filesystem::path> dictionary_file;
  std::optional<std::filesystem::path> required_words_file;
  bool no_dict = false;
  bool cv = false;
  bool csv = false;
  std::filesystem::path source;
};

void usage(FILE* out) {
  fputs("usage: pfilter [-l LETTERS [-u LETTERS]... [--exact]] [-m N] [-x N] "
        "[-s N] [--no-nopairs] [-y] [-d FILE | --no-dict] [-i INDEX] "
        "[--rw FILE] "
        "[--cv [--csv]] PAIRS-FILE|-\n", out);
  if (out != stdout) return;

  fputs("  print pairs whose two words are in the dictionary\n"
        "  and not in $WFROOT/.wf/classified/no/no.pairs\n"
        "  PAIRS-FILE contains word,word lines; '-' reads standard input\n"
        "\noptions:\n", stdout);
  dfs_help_option("-l, --letters LETTERS",
      "print only pairs that fit within LETTERS; may be given once");
  dfs_help_option("-u, --used-letters LETTERS",
      "subtract letters already used from the -l letters; may be repeated");
  dfs_help_option("--exact",
      "print only pairs that use all of the -l letters; requires -l");
  dfs_help_option("-m, --min-letters N",
      "print only pairs of at least N letters (default: %d)",
      PFILTER_DEFAULT_MIN_LETTERS);
  dfs_help_option("-x, --max-letters N",
      "print only pairs of at most N letters; 0 for no limit (default: %d)",
      PFILTER_DEFAULT_MAX_LETTERS);
  classified_help_sentence_no();
  dfs_help_option("--no-nopairs",
      "keep pairs listed in the no.pairs files");
  dfs_help_option("-y, --yes",
      "also drop pairs listed in $WFROOT/%s/yes/yes.pairs, and with -s N "
      "in $WFROOT/%s/sN/yes/yes.pairs",
      WORKFLOW_CLASSIFIED_PATH, WORKFLOW_CLASSIFIED_PATH);
  dfs_help_option("-d, --dict FILE",
      "dictionary FILE (default: $WFROOT/%s)", WORKFLOW_DICT_PATH);
  dfs_help_option("--no-dict", "skip the dictionary check");
  dfs_help_option("-i, --idx INDEX",
      "print only pairs with \"word1 word2\" or \"word2 word1\" in INDEX");
  dfs_help_option("--rw, --required-words FILE",
      "print only pairs with at least one word in FILE, one word per line");
  dfs_help_option("--cv",
      "print after each pair the consonant/vowel ratio of the -l letters "
      "left once the pair's letters are removed; y is a consonant. inf "
      "when only consonants are left. Pairs print sorted by it, lowest "
      "first. Requires -l");
  dfs_help_option("--csv",
      "with --cv, sort as --cv does but print only the pairs");
  dfs_help_option("-h, --help", "show this help");
}

bool parse_args(char* argv[], Args* out, bool* help) {
  static struct optparse_long const long_options[] = {
    { "letters", 'l', OPTPARSE_REQUIRED },
    { "used-letters", 'u', OPTPARSE_REQUIRED },
    { "exact", OPT_EXACT, OPTPARSE_NONE },
    { "min-letters", 'm', OPTPARSE_REQUIRED },
    { "max-letters", 'x', OPTPARSE_REQUIRED },
    CLASSIFIED_SENTENCE_LONG_OPTION,
    { "no-nopairs", OPT_NO_NOPAIRS, OPTPARSE_NONE },
    { "yes", 'y', OPTPARSE_NONE },
    { "dict", 'd', OPTPARSE_REQUIRED },
    { "no-dict", OPT_NO_DICT, OPTPARSE_NONE },
    { "idx", 'i', OPTPARSE_REQUIRED },
    { "rw", OPT_REQUIRED_WORDS, OPTPARSE_REQUIRED },
    { "required-words", OPT_REQUIRED_WORDS, OPTPARSE_REQUIRED },
    { "cv", OPT_CV, OPTPARSE_NONE },
    { "csv", OPT_CSV, OPTPARSE_NONE },
    { "help", 'h', OPTPARSE_NONE },
    { NULL, 0, OPTPARSE_NONE },
  };

  struct optparse options;
  optparse_init(&options, argv);

  char const* letters = NULL;
  std::string used_letters;
  bool have_used_letters = false;
  int option;
  while ((option = optparse_long(&options, long_options, NULL)) != -1) {
    switch (option) {
      case 'l':
        if (letters != NULL) {
          report(PROGRAM, "--letters may be given only once");
          return false;
        }
        letters = options.optarg;
        break;
      case 'u':
        used_letters += options.optarg;
        have_used_letters = true;
        break;
      case OPT_EXACT:
        out->options.exact = true;
        break;
      case OPT_NO_NOPAIRS:
        out->options.drop_no = false;
        break;
      case 'm':
        if (!parse_count(options.optarg, "--min-letters",
                &out->options.min_letters))
          return false;
        break;
      case 'x':
        if (!parse_count(options.optarg, "--max-letters",
                &out->options.max_letters))
          return false;
        break;
      case 's':
        if (!parse_classified_sentence(options.optarg, &out->options.sentence))
          return false;
        break;
      case 'y':
        out->options.drop_yes = true;
        break;
      case 'd':
        out->dictionary_file = options.optarg;
        break;
      case OPT_NO_DICT:
        out->no_dict = true;
        break;
      case 'i':
        out->options.index_file = options.optarg;
        break;
      case OPT_REQUIRED_WORDS:
        out->required_words_file = options.optarg;
        break;
      case OPT_CV:
        out->cv = true;
        break;
      case OPT_CSV:
        out->csv = true;
        break;
      case 'h':
        *help = true;
        return true;
      default:
        fprintf(stderr, "error: %s\n", options.errmsg);
        return false;
    }
  }

  if (letters != NULL) {
    LetterBag bag;
    if (!make_letter_bag(letters, used_letters, &bag)) return false;
    out->options.bag = bag;
  } else if (have_used_letters) {
    report(PROGRAM, "--used-letters requires --letters");
    return false;
  }

  if (!out->options.drop_no && !out->options.drop_yes &&
      out->options.sentence != CLASSIFIED_NO_SENTENCE) {
    report(PROGRAM, "-s goes with -y when given --no-nopairs");
    return false;
  }

  if (out->options.exact && letters == NULL) {
    report(PROGRAM, "--exact requires --letters");
    return false;
  }

  if (out->cv && letters == NULL) {
    report(PROGRAM, "--cv requires --letters");
    return false;
  }

  if (out->csv && !out->cv) {
    report(PROGRAM, "--csv requires --cv");
    return false;
  }

  if (out->no_dict && out->dictionary_file) {
    report(PROGRAM, "--dict and --no-dict can't be combined");
    return false;
  }

  char const* const source = optparse_arg(&options);
  if (source == NULL || optparse_arg(&options) != NULL) return false;
  out->source = source;
  return true;
}

// Prints the lines of `source` that `filter` keeps, each padded and followed
// by the remaining_cv_ratio() of its pair against `bag`, lowest ratio first.
// With `csv`, prints only the lines, unpadded, in the same order.
bool print_kept_rows_with_cv(std::filesystem::path const& source,
                             Pfilter const& filter, LetterBag const& bag,
                             bool csv) {
  std::vector<std::pair<std::string, double>> kept;
  size_t width = 0;
  if (!read_rows(PROGRAM, source, "pair list", false,
          [&](DfsPairRow const& row, std::string const& line) {
            if (!filter.keep(row)) return true;
            kept.emplace_back(line,
                remaining_cv_ratio(bag, row.left + row.right));
            width = std::max(width, line.size());
            return true;
          }))
    return false;
  std::stable_sort(kept.begin(), kept.end(),
      [](auto const& a, auto const& b) { return a.second < b.second; });
  for (auto const& [line, cv] : kept) {
    if (csv)
      printf("%s\n", line.c_str());
    else
      printf("%-*s  %.2f\n", int(width), line.c_str(), cv);
  }
  if (fflush(stdout) != 0) {
    report(PROGRAM, "can't write output");
    return false;
  }
  return true;
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

  if (args.options.sentence == CLASSIFIED_NO_SENTENCE &&
      (args.options.drop_no || args.options.drop_yes))
    alert(PROGRAM, "--sentence not supplied");

  DfsDictionary dictionary;
  if (!args.no_dict) {
    if (!default_workflow_path(PROGRAM, &args.dictionary_file,
                               WORKFLOW_DICT_PATH))
      return 1;
    if (!load_dictionary(*args.dictionary_file, &dictionary)) return 1;
    args.options.dictionary = std::cref(dictionary);
  }

  DfsDictionary required_words;
  if (args.required_words_file) {
    if (!load_dictionary(*args.required_words_file, &required_words))
      return 1;
    if (required_words.empty()) {
      report(PROGRAM, "--required-words \"%s\" has no words",
             args.required_words_file->c_str());
      return 1;
    }
    args.options.required_words = std::cref(required_words);
  }

  ClassifiedPairCache cache;
  Pfilter filter;
  if (!filter.load(PROGRAM, args.options, cache)) return 1;
  if (args.cv)
    return print_kept_rows_with_cv(args.source, filter, *args.options.bag,
               args.csv) ? 0 : 1;
  if (!print_kept_rows(PROGRAM, args.source, "pair list", false,
          [&](DfsPairRow const& row) { return filter.keep(row); }))
    return 1;
  if (!(std::cout << std::flush)) {
    report(PROGRAM, "can't write output");
    return 1;
  }
  return 0;
}
