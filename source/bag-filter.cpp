// bag-filter.cpp - Print words and pairs that fit within a letter bag.

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <fstream>
#include <iostream>
#include <string>

#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "optparse.h"

namespace {

struct Counts {
  int values[UCHAR_MAX + 1] = { 0 };
};

struct Args {
  Counts bag;
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
  std::string bag;
  std::string remove;
  std::string remaining;
  if (!clean_letters(letters, "letters", &bag) ||
      !clean_letters(used_letters.c_str(), "used letters", &remove) ||
      !subtract_letters(bag, remove, &remaining))
    return false;
  for (size_t i = 0; i < remaining.size(); ++i)
    ++out->bag.values[(unsigned char) remaining[i]];

  out->files = argv + options.optind;
  return *out->files != NULL;
}

bool fits(Args const& args, DfsPairRow const& row) {
  Counts left = args.bag;
  for (std::string const* word : { &row.left, &row.right }) {
    for (size_t i = 0; i < word->size(); ++i)
      if (--left.values[(unsigned char) (*word)[i]] < 0) return false;
  }
  if (!args.exact) return true;
  for (int value : left.values)
    if (value != 0) return false;
  return true;
}

bool filter(std::istream& input, char const* source, Args const& args) {
  std::string line;
  size_t number = 0;
  while (std::getline(input, line)) {
    ++number;
    DfsPairRow row;
    if (!parse_pair_row(line, "input", source, number, true, &row))
      return false;
    if (fits(args, row) && !(std::cout << line << '\n')) {
      fputs("bag-filter: can't write output\n", stderr);
      return false;
    }
  }
  if (!input.eof()) {
    fprintf(stderr, "bag-filter: can't read \"%s\"\n", source);
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

  for (char** file = args.files; *file != NULL; ++file) {
    if (strcmp(*file, "-") == 0) {
      if (!filter(std::cin, *file, args)) return 1;
      continue;
    }
    std::ifstream input(*file, std::ios::binary);
    if (!input.is_open()) {
      fprintf(stderr, "bag-filter: can't open \"%s\"\n", *file);
      return 1;
    }
    if (!filter(input, *file, args)) return 1;
  }
  if (!(std::cout << std::flush)) {
    fputs("bag-filter: can't write output\n", stderr);
    return 1;
  }
  return 0;
}
