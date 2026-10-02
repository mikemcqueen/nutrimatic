// lc.cpp - Compare the letters left in a text, once others are used, against
// standard English letter frequencies.

#include <stdio.h>

#include <iostream>
#include <string>
#include <vector>

#include "dfs-cli-help.h"
#include "lc-impl.h"
#include "letter-bag.h"
#include "optparse.h"
#include "row-input.h"

namespace {

void usage(FILE* out) {
  fputs("usage: lc [-r|--cv] [-f FILE] TEXT [USED...]\n"
        "       lc [-r|--cv] -f FILE [USED...]\n", out);
  if (out != stdout) return;

  fputs(
      "  count the letters of TEXT, remove the letters of each USED, and\n"
      "  report how the letters that remain compare to standard English\n"
      "  frequencies\n"
      "\n"
      "  The table lists each remaining letter, sorted by factor:\n"
      "    char      the letter\n"
      "    cnt       times it occurs in the remaining letters\n"
      "    expected  its frequency in English text, as a percent\n"
      "    actual    its frequency here, as a percent; parenthesized when\n"
      "              below expected\n"
      "    factor    actual / expected; +++ above 2, --- below 0.5\n"
      "\n"
      "  The C/V ratio line compares consonants per vowel (y is a consonant)\n"
      "  against an English average of 1.59, marking the actual value +++\n"
      "  above 1.8 and --- below 1.4.\n"
      "\narguments:\n", stdout);
  dfs_help_option("TEXT", "the letters to count; case and non-letters are "
                  "ignored");
  dfs_help_option("USED", "text whose letters are removed from TEXT; must "
                  "be spellable from TEXT");
  fputs("\noptions:\n", stdout);
  dfs_help_option("-r", "print only the remaining letters");
  dfs_help_option("--cv", "print only the C/V ratio of the remaining "
                  "letters");
  dfs_help_option("-f FILE", "read TEXT from the first line of FILE; '-' "
                  "reads standard input");
  dfs_help_option("-h, --help", "show this help");
}

bool read_text(char const* path, std::string* out) {
  return read_input_file("lc", path, [&](std::istream& input) {
    if (std::getline(input, *out) || input.eof()) return true;
    fprintf(stderr, "lc: can't read \"%s\"\n", path);
    return false;
  });
}

bool parse_args(char* argv[], LetterCountOptions* out, bool* help) {
  enum { OPT_CV = 256 };
  static struct optparse_long const long_options[] = {
    { "help", 'h', OPTPARSE_NONE },
    { NULL, 'r', OPTPARSE_NONE },
    { "cv", OPT_CV, OPTPARSE_NONE },
    { NULL, 'f', OPTPARSE_REQUIRED },
    { NULL, 0, OPTPARSE_NONE },
  };

  struct optparse options;
  optparse_init(&options, argv);

  char const* file = NULL;
  int option;
  while ((option = optparse_long(&options, long_options, NULL)) != -1) {
    switch (option) {
      case 'h':
        *help = true;
        return true;
      case 'r':
      case OPT_CV: {
        LetterCountMode const mode =
            option == 'r' ? LETTER_COUNT_REMAINING : LETTER_COUNT_CV;
        if (out->mode != LETTER_COUNT_TABLE && out->mode != mode) {
          fputs("error: -r and --cv are incompatible\n", stderr);
          return false;
        }
        out->mode = mode;
        break;
      }
      case 'f':
        file = options.optarg;
        break;
      default:
        fprintf(stderr, "error: %s\n", options.errmsg);
        return false;
    }
  }

  if (file != NULL) {
    if (!read_text(file, &out->text)) return false;
  } else {
    char const* const text = optparse_arg(&options);
    if (text == NULL) return false;
    out->text = text;
  }
  for (char* arg; (arg = optparse_arg(&options)) != NULL;)
    out->used_letters += lowercase_letters(arg);
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  (void) argc;
  LetterCountOptions options;
  bool help = false;
  std::vector<std::string> lines;
  if (!parse_args(argv, &options, &help) ||
      (!help && !letter_count_lines(options, &lines))) {
    usage(stderr);
    return 2;
  }
  if (help) {
    usage(stdout);
    return 0;
  }
  for (std::string const& line : lines) printf("%s\n", line.c_str());
  if (fflush(stdout) != 0) {
    fputs("lc: can't write output\n", stderr);
    return 1;
  }
  return 0;
}
