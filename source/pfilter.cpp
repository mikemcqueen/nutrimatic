#include <stdio.h>

#include <filesystem>
#include <iostream>
#include <string>

#include "classified.h"
#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "letter-bag.h"
#include "optparse.h"
#include "row-input.h"
#include "workflow-paths.h"

namespace {

int const DEFAULT_MAX_LETTERS = 15;

struct Args {
  bool have_bag = false;
  LetterBag bag;
  int max_letters = DEFAULT_MAX_LETTERS;
  int sentence = CLASSIFIED_NO_SENTENCE;
  bool drop_yes = false;
  char const* source = NULL;
};

void usage(FILE* out) {
  fputs("usage: pfilter [-l LETTERS [-u LETTERS]...] [-x N] [-s N] [-y] "
        "PAIRS-FILE|-\n", out);
  if (out != stdout) return;

  fputs("  print pairs whose two words are in $WFROOT/.wf/dict/words.filtered\n"
        "  and not in $WFROOT/.wf/classified/no/no.pairs\n"
        "  PAIRS-FILE contains word,word lines; '-' reads standard input\n"
        "\noptions:\n", stdout);
  dfs_help_option("-l, --letters LETTERS",
      "print only pairs that fit within LETTERS; may be given once");
  dfs_help_option("-u, --used-letters LETTERS",
      "subtract letters already used from the -l letters; may be repeated");
  dfs_help_option("-x, --max-letters N",
      "print only pairs of at most N letters; 0 for no limit (default: %d)",
      DEFAULT_MAX_LETTERS);
  classified_help_sentence_no();
  dfs_help_option("-y, --yes",
      "also drop pairs listed in $WFROOT/%s/yes/yes.pairs, and with -s N "
      "in $WFROOT/%s/sN/yes/yes.pairs",
      WORKFLOW_CLASSIFIED_PATH, WORKFLOW_CLASSIFIED_PATH);
  dfs_help_option("-h, --help", "show this help");
}

bool parse_args(char* argv[], Args* out, bool* help) {
  static struct optparse_long const long_options[] = {
    { "letters", 'l', OPTPARSE_REQUIRED },
    { "used-letters", 'u', OPTPARSE_REQUIRED },
    { "max-letters", 'x', OPTPARSE_REQUIRED },
    CLASSIFIED_SENTENCE_LONG_OPTION,
    { "yes", 'y', OPTPARSE_NONE },
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
          fputs("pfilter: --letters may be given only once\n", stderr);
          return false;
        }
        letters = options.optarg;
        break;
      case 'u':
        used_letters += options.optarg;
        have_used_letters = true;
        break;
      case 'x':
        if (!parse_count(options.optarg, "--max-letters", &out->max_letters))
          return false;
        break;
      case 's':
        if (!parse_classified_sentence(options.optarg, &out->sentence))
          return false;
        break;
      case 'y':
        out->drop_yes = true;
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
    if (!make_letter_bag(letters, used_letters, &out->bag)) return false;
    out->have_bag = true;
  } else if (have_used_letters) {
    fputs("pfilter: --used-letters requires --letters\n", stderr);
    return false;
  }

  out->source = optparse_arg(&options);
  return out->source != NULL && optparse_arg(&options) == NULL;
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

  char const* const root = require_workflow_root("pfilter");
  if (root == NULL) return 1;
  std::filesystem::path const dict_path =
      std::filesystem::path(root) / WORKFLOW_DICT_PATH;
  DfsDictionary dictionary;
  if (!load_dictionary(dict_path.c_str(), &dictionary)) return 1;

  DfsPairSet rejected;
  if (!load_global_no_pairs("pfilter", root, &rejected)) return 1;
  if (args.sentence != CLASSIFIED_NO_SENTENCE &&
      !load_sentence_no_pairs("pfilter", root, args.sentence, &rejected))
    return 1;
  if (args.drop_yes &&
      !load_classified_pairs("pfilter", root, CLASSIFIED_NO_SENTENCE, "yes",
          &rejected))
    return 1;
  if (args.drop_yes && args.sentence != CLASSIFIED_NO_SENTENCE &&
      !load_classified_pairs("pfilter", root, args.sentence, "yes",
          &rejected))
    return 1;

  if (!print_kept_rows("pfilter", args.source, "pair list", false,
          [&](DfsPairRow const& row) {
            return (args.max_letters == 0 ||
                       row.left.size() + row.right.size() <=
                           size_t(args.max_letters)) &&
                dictionary.contains(row.left) &&
                dictionary.contains(row.right) &&
                !rejected.contains(row.entry()) &&
                (!args.have_bag ||
                    fits_letter_bag(args.bag, row.left + row.right));
          }))
    return 1;
  if (!(std::cout << std::flush)) {
    fputs("pfilter: can't write output\n", stderr);
    return 1;
  }
  return 0;
}
