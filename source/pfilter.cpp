#include <stdio.h>

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include "classified.h"
#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "index.h"
#include "letter-bag.h"
#include "log.h"
#include "optparse.h"
#include "row-input.h"
#include "workflow-paths.h"

namespace {

int const DEFAULT_MAX_LETTERS = 15;
int const OPT_NO_DICT = 256;

struct Args {
  bool have_bag = false;
  LetterBag bag;
  int max_letters = DEFAULT_MAX_LETTERS;
  int sentence = CLASSIFIED_NO_SENTENCE;
  bool drop_yes = false;
  char const* index_file = NULL;
  char const* dictionary_file = NULL;
  bool no_dict = false;
  char const* source = NULL;
};

void usage(FILE* out) {
  fputs("usage: pfilter [-l LETTERS [-u LETTERS]...] [-x N] [-s N] [-y] "
        "[-d FILE | --no-dict] [-i INDEX] PAIRS-FILE|-\n", out);
  if (out != stdout) return;

  fputs("  print pairs whose two words are in the dictionary\n"
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
  dfs_help_option("-d, --dict FILE",
      "dictionary FILE (default: $WFROOT/%s)", WORKFLOW_DICT_PATH);
  dfs_help_option("--no-dict", "skip the dictionary check");
  dfs_help_option("-i, --idx INDEX",
      "print only pairs with \"word1 word2\" or \"word2 word1\" in INDEX");
  dfs_help_option("-h, --help", "show this help");
}

bool parse_args(char* argv[], Args* out, bool* help) {
  static struct optparse_long const long_options[] = {
    { "letters", 'l', OPTPARSE_REQUIRED },
    { "used-letters", 'u', OPTPARSE_REQUIRED },
    { "max-letters", 'x', OPTPARSE_REQUIRED },
    CLASSIFIED_SENTENCE_LONG_OPTION,
    { "yes", 'y', OPTPARSE_NONE },
    { "dict", 'd', OPTPARSE_REQUIRED },
    { "no-dict", OPT_NO_DICT, OPTPARSE_NONE },
    { "idx", 'i', OPTPARSE_REQUIRED },
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
      case 'd':
        out->dictionary_file = options.optarg;
        break;
      case OPT_NO_DICT:
        out->no_dict = true;
        break;
      case 'i':
        out->index_file = options.optarg;
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

  if (out->no_dict && out->dictionary_file != NULL) {
    fputs("pfilter: --dict and --no-dict can't be combined\n", stderr);
    return false;
  }

  out->source = optparse_arg(&options);
  return out->source != NULL && optparse_arg(&options) == NULL;
}

bool in_index(IndexReader const& reader, DfsPairRow const& row) {
  IndexReader::EntryPosition position;
  return reader.aggregate_entry_position(row.entry(), &position) ||
      reader.aggregate_entry_position(row.entry(/*reverse=*/true), &position);
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

  if (args.sentence == CLASSIFIED_NO_SENTENCE)
    alert("pfilter", "--sentence not supplied");

  char const* const root = require_workflow_root("pfilter");
  if (root == NULL) return 1;
  DfsDictionary dictionary;
  if (!args.no_dict) {
    std::string const dict_path = args.dictionary_file != NULL
        ? std::string(args.dictionary_file)
        : (std::filesystem::path(root) / WORKFLOW_DICT_PATH).string();
    if (!load_dictionary(dict_path.c_str(), &dictionary)) return 1;
  }

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

  std::unique_ptr<IndexReader> reader;
  if (args.index_file != NULL) {
    FILE* fp = fopen(args.index_file, "rb");
    if (fp == NULL) {
      fprintf(stderr, "pfilter: can't open \"%s\"\n", args.index_file);
      return 1;
    }
    reader.reset(new IndexReader(fp));
  }

  if (!print_kept_rows("pfilter", args.source, "pair list", false,
          [&](DfsPairRow const& row) {
            return (args.max_letters == 0 ||
                       row.left.size() + row.right.size() <=
                           size_t(args.max_letters)) &&
                (args.no_dict || (dictionary.contains(row.left) &&
                    dictionary.contains(row.right))) &&
                !rejected.contains(row.entry()) &&
                (!args.have_bag ||
                    fits_letter_bag(args.bag, row.left + row.right)) &&
                (reader == nullptr || in_index(*reader, row));
          }))
    return 1;
  if (!(std::cout << std::flush)) {
    fputs("pfilter: can't write output\n", stderr);
    return 1;
  }
  return 0;
}
