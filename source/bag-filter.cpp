// bag-filter.cpp - Print words and pairs that fit within a letter bag.

#include <stdio.h>

#include <iostream>
#include <string>

#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "letter-bag.h"
#include "optparse.h"
#include "row-input.h"

namespace {

struct Args {
  LetterBag bag;
  bool exact = false;
  char** files = NULL;
};

void usage(FILE* out) {
  fputs("usage: bag-filter [-e] [-u LETTERS] LETTERS FILE...\n", out);
  if (out != stdout) return;

  fputs("  print the words and word,word pairs in each FILE that fit within\n"
        "  LETTERS; '-' reads standard input\n"
        "\noptions:\n", stdout);
  dfs_help_option("-e, --exact", "print only entries that use every letter");
  dfs_help_used_letters();
  dfs_help_option("-h, --help", "show this help");
}

bool parse_args(char* argv[], Args* out, bool* help) {
  static struct optparse_long const long_options[] = {
    { "exact", 'e', OPTPARSE_NONE },
    { "used-letters", 'u', OPTPARSE_REQUIRED },
    { "help", 'h', OPTPARSE_NONE },
    { NULL, 0, OPTPARSE_NONE },
  };

  struct optparse options;
  optparse_init(&options, argv);

  std::string used_letters;
  int option;
  while ((option = optparse_long(&options, long_options, NULL)) != -1) {
    switch (option) {
      case 'e':
        out->exact = true;
        break;
      case 'u':
        used_letters += options.optarg;
        break;
      case 'h':
        *help = true;
        return true;
      default:
        fprintf(stderr, "error: %s\n", options.errmsg);
        return false;
    }
  }

  char const* const letters = optparse_arg(&options);
  if (letters == NULL) return false;
  if (!make_letter_bag(letters, used_letters, &out->bag)) return false;

  out->files = argv + options.optind;
  return *out->files != NULL;
}

bool fits(Args const& args, DfsPairRow const& row) {
  std::string const letters = row.left + row.right;
  return fits_letter_bag(args.bag, letters) &&
      (!args.exact || letters.size() == args.bag.size);
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

  for (char** file = args.files; *file != NULL; ++file) {
    if (!print_kept_rows("bag-filter", *file, "input", true,
            [&](DfsPairRow const& row) { return fits(args, row); }))
      return 1;
  }
  if (!(std::cout << std::flush)) {
    fputs("bag-filter: can't write output\n", stderr);
    return 1;
  }
  return 0;
}
