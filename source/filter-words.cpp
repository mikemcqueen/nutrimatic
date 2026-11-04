// filter-words.cpp - Print the words of FILE, one per line, cleaned as
// load_dictionary() cleans them, whose length is within -m and -x and that
// have no workflow verdict yet: not in $WFROOT/.wf/dict/done/yes.words or
// no.words, and in no classified YES pair, global or any sentence's.

#include "classified.h"
#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "log.h"
#include "optparse.h"
#include "row-input.h"
#include "seed-view.h"
#include "workflow-paths.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace {

constexpr char PROGRAM[] = "filter-words";

struct Args {
  int min_len = SEED_DEFAULT_MIN_WORD_LEN;
  int max_len = 0;
  std::string path = "-";
};

void usage(std::string_view program, FILE* out) {
  fprintf(out, "usage: %.*s [-m N] [-x N] [FILE|-]\n", SV_ARG(program));
  if (out != stdout) return;

  fprintf(stdout,
          "  print each word of FILE, one per line, lowercased with\n"
          "  characters outside a-z/0-9 dropped and lines with '-' skipped,\n"
          "  whose length is within -m and -x and that is in none of:\n"
          "    $WFROOT/%s\n"
          "    $WFROOT/%s\n"
          "    a pair of $WFROOT/%s/[sN/]yes/yes.pairs\n"
          "\noptions:\n",
          WORKFLOW_YES_WORDS_PATH, WORKFLOW_NO_WORDS_PATH,
          WORKFLOW_CLASSIFIED_PATH);
  dfs_help_description_column(20);
  dfs_help_option("-m, --min N", "keep words of at least N letters "
                  "(default %d)", SEED_DEFAULT_MIN_WORD_LEN);
  dfs_help_option("-x, --max N", "keep words of at most N letters; 0 for "
                  "no limit (default: 0)");
  dfs_help_option("-h, --help", "show this help");
}

bool parse_args(char* argv[], Args* out, bool* help) {
  static struct optparse_long const long_options[] = {
    { "min", 'm', OPTPARSE_REQUIRED },
    { "max", 'x', OPTPARSE_REQUIRED },
    { "help", 'h', OPTPARSE_NONE },
    { NULL, 0, OPTPARSE_NONE },
  };
  struct optparse options;
  optparse_init(&options, argv);
  int option;
  while ((option = optparse_long(&options, long_options, NULL)) != -1) {
    switch (option) {
      case 'm':
        if (!parse_count(options.optarg, "-m", &out->min_len)) return false;
        break;
      case 'x':
        if (!parse_count(options.optarg, "-x", &out->max_len)) return false;
        break;
      case 'h':
        *help = true;
        return true;
      default:
        fprintf(stderr, "error: %s\n", options.errmsg);
        return false;
    }
  }
  if (char const* const path = optparse_arg(&options)) out->path = path;
  if (char const* const extra = optparse_arg(&options)) {
    report(PROGRAM, "unexpected argument \"%s\"", extra);
    return false;
  }
  if (out->max_len != 0 && out->min_len > out->max_len) {
    report(PROGRAM, "-m %d is more than -x %d", out->min_len, out->max_len);
    return false;
  }
  return true;
}

bool load_verdict_words(std::filesystem::path const& root,
                        std::string_view relative, DfsDictionary* words) {
  std::filesystem::path const path = root / relative;
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) {
    report(PROGRAM, "\"%s\" is missing; run wf gen dict", path.c_str());
    return false;
  }
  return load_dictionary(path, words);
}

bool load_yes_pair_words(std::filesystem::path const& root, int sentence,
                         DfsDictionary* words) {
  std::string path;
  if (!classified_pair_file(PROGRAM, root, sentence, "yes", &path,
          sentence != CLASSIFIED_NO_SENTENCE))
    return false;
  return path.empty() ||
      read_rows(PROGRAM, path, "classified pair list", false,
          [&](DfsPairRow const& row, std::string const&) {
            words->insert(row.left);
            words->insert(row.right);
            return true;
          });
}

bool load_all_yes_pair_words(std::filesystem::path const& root,
                             DfsDictionary* words) {
  if (!load_yes_pair_words(root, CLASSIFIED_NO_SENTENCE, words)) return false;
  for (int sentence = 1; sentence <= WORKFLOW_SENTENCES; ++sentence) {
    if (!load_yes_pair_words(root, sentence, words)) return false;
  }
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  (void)argc;
  Args args;
  bool help = false;
  if (!parse_args(argv, &args, &help)) {
    usage(argv[0], stderr);
    return 2;
  }
  if (help) {
    usage(argv[0], stdout);
    return 0;
  }

  std::filesystem::path const root = require_workflow_root(PROGRAM);
  if (root.empty()) return 1;
  DfsDictionary excluded;
  if (!load_verdict_words(root, WORKFLOW_YES_WORDS_PATH, &excluded) ||
      !load_verdict_words(root, WORKFLOW_NO_WORDS_PATH, &excluded) ||
      !load_all_yes_pair_words(root, &excluded))
    return 1;

  std::string word;
  if (!read_lines(PROGRAM, args.path, [&](std::string const& line, size_t) {
        if (!clean_dictionary_line(line, &word)) return true;
        int const length = int(word.size());
        if (length < args.min_len ||
            (args.max_len != 0 && length > args.max_len) ||
            excluded.contains(word))
          return true;
        fwrite(word.data(), 1, word.size(), stdout);
        fputc('\n', stdout);
        return true;
      }))
    return 1;
  if (fflush(stdout) != 0) {
    report(PROGRAM, "can't write output");
    return 1;
  }
  return 0;
}
