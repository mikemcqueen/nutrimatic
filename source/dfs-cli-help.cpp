#include "dfs-cli-help.h"

#include "dfs-cli-args.h"
#include "workflow-paths.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static size_t const HELP_DESCRIPTION_COLUMN = 38;
static size_t const HELP_LINE_WIDTH = 100;

static void help_spaces(size_t count) {
  while (count-- > 0) fputc(' ', stdout);
}

void dfs_help_option(char const* option, char const* format, ...) {
  char description[4096];
  va_list args;
  va_start(args, format);
  vsnprintf(description, sizeof description, format, args);
  va_end(args);

  fputs("  ", stdout);
  fputs(option, stdout);
  size_t column = 2 + strlen(option);
  if (column >= HELP_DESCRIPTION_COLUMN) {
    fputc('\n', stdout);
    help_spaces(HELP_DESCRIPTION_COLUMN);
  } else {
    help_spaces(HELP_DESCRIPTION_COLUMN - column);
  }
  column = HELP_DESCRIPTION_COLUMN;

  char const* p = description;
  while (*p != '\0') {
    while (*p == ' ') ++p;
    char const* end = p;
    while (*end != '\0' && *end != ' ') ++end;
    size_t const word_length = size_t(end - p);
    if (column > HELP_DESCRIPTION_COLUMN &&
        column + 1 + word_length > HELP_LINE_WIDTH) {
      fputc('\n', stdout);
      help_spaces(HELP_DESCRIPTION_COLUMN);
      column = HELP_DESCRIPTION_COLUMN;
    } else if (column > HELP_DESCRIPTION_COLUMN) {
      fputc(' ', stdout);
      ++column;
    }
    fwrite(p, 1, word_length, stdout);
    column += word_length;
    p = end;
  }
  fputs("\n\n", stdout);
}

void dfs_help_index(bool require_for_near) {
  dfs_help_option("-i, --idx INDEX",
      "read the completed Nutrimatic index from INDEX; workflow mode "
      "defaults to DIR/%s; required otherwise%s",
      WORKFLOW_INDEX_PATH, require_for_near ? " and with --near" : "");
}

void dfs_help_used_letters() {
  dfs_help_option("-u, --used-letters LETTERS",
      "subtract letters already used from the input letters before "
      "searching");
}

void dfs_help_dictionary() {
  dfs_help_option("--dict PATH",
      "filter entries to words in the dictionary; workflow mode defaults "
      "to DIR/%s", WORKFLOW_DICT_PATH);
}

void dfs_help_min_word_length() {
  dfs_help_option("-m, --min-word-length N",
      "minimum word length (default: %d; 0 for no minimum)",
      DFS_DEFAULT_MIN_WORD_LEN);
}

void dfs_help_top(int default_top) {
  dfs_help_option("-n, --top N",
      "maximum number of results (default: %d; 0 returns all results)",
      default_top);
}

void dfs_help_max_extract_words(int default_max) {
  if (default_max == 0) {
    dfs_help_option("-x, --max-words N",
        "explore at most N words inside one index entry (default: 0, no "
        "limit)");
  } else {
    dfs_help_option("-x, --max-words N",
        "explore at most N words inside one index entry (default: %d; 0 "
        "means no limit)", default_max);
  }
}

void dfs_help_seed_pairs() {
  dfs_help_option("--seed-pairs FILE",
      "load a repeatable fixed pair-bonus tier of %.2f",
      DFS_SEED_PAIR_BONUS);
}

void dfs_help_yes_pairs() {
  dfs_help_option("--yes-pairs FILE",
      "load a repeatable fixed pair-bonus tier of %.2f",
      DFS_YES_PAIR_BONUS);
}

void dfs_help_best_pairs() {
  dfs_help_option("--best-pairs FILE",
      "mark BEST entries; may be given once");
}

void dfs_help_wf() {
  dfs_help_option("--wf",
      "use the nonempty WFROOT environment variable as the workflow root");
}

void dfs_help_target() {
  dfs_help_option("-t, --target TARGET",
      "select a prefix of sN/[ou]-letters/mN/gN; its sentence seed is "
      "auto-loaded, and a complete target also loads its optional %s unless "
      "--best-pairs replaces it", WORKFLOW_TARGET_BEST_PAIRS_NAME);
}

void dfs_help_segment_penalty() {
  dfs_help_option("-P, --segment-penalty P",
      "divide the score by P for each selected index entry after the first; "
      "P must be at least 1 (default: %.0f); k entries score as product(count) "
      "/ (corpus-total * P)^(k-1)", DFS_DEFAULT_SEGMENT_PENALTY);
}

void dfs_help_word_bonus() {
  dfs_help_option("--word-bonus N",
      "multiply each multi-word index entry by %.0f^N (default: %.1f); at N=1 "
      "a multi-word entry earns back the default -P it costs, so adding one "
      "as a further entry is free",
      DFS_WORD_BONUS_BASE, DFS_DEFAULT_WORD_BONUS);
}

void dfs_help_pair_bonus() {
  dfs_help_option("--pair-bonus N",
      "multiply each index entry found in --pairs by %.0f^N (default: %.1f)",
      DFS_PAIR_BONUS_BASE, DFS_DEFAULT_PAIR_BONUS);
}

void dfs_help_no_score(bool has_segments_mode) {
  if (has_segments_mode) {
    dfs_help_option("--no-score",
        "omit the leading score from each result; the output cannot be read "
        "by filter-segments or rerank-anagrams; cannot be combined with "
        "--segments");
  } else {
    dfs_help_option("--no-score",
        "omit the leading count or score from each result");
  }
}
