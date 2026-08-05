#ifndef NUTRIMATIC_DFS_CLI_ARGS_H
#define NUTRIMATIC_DFS_CLI_ARGS_H

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "dfs-class-list.h"
#include "dfs-score.h"
#include "optparse.h"

// Shared argument-parsing helpers for the dfs-anagrams-family CLIs
// (dfs-anagrams, query-index): letter-bag cleanup/subtraction, integer
// option parsing, dictionary loading, and the options both CLIs accept.

inline constexpr int DFS_DEFAULT_MIN_WORD_LEN = 4;
// --pairs is exactly this cap on the words in one extracted entry.
inline constexpr int DFS_PAIRS_LIMIT = 2;

// Long-option codes for shared options with no short form. They sit well above
// the range each CLI uses for its own codes, so adding a private option cannot
// silently collide with a shared one.
inline constexpr int DFS_OPT_DICT = 300;
inline constexpr int DFS_OPT_PAIRS = 301;

// The rows both CLIs contribute to their optparse_long table. A macro rather
// than a shared array because optparse terminates on a NULL row, so each CLI
// still needs one table of its own; this splices the common rows into it.
#define DFS_COMMON_LONG_OPTIONS \
  { "used-letters", 'u', OPTPARSE_REQUIRED }, \
  { "dict", DFS_OPT_DICT, OPTPARSE_REQUIRED }, \
  { "min-word-length", 'm', OPTPARSE_REQUIRED }, \
  { "max-extract-words", 'x', OPTPARSE_REQUIRED }, \
  { "pairs", DFS_OPT_PAIRS, OPTPARSE_NONE }, \
  { "top", 'n', OPTPARSE_REQUIRED }, \
  { "search-threads", 'S', OPTPARSE_REQUIRED }, \
  { "segment-penalty", 'P', OPTPARSE_REQUIRED }

// What the shared options parsed into. `top` has no shared default because the
// two CLIs disagree on it; each sets its own before the option loop.
struct DfsCommonArgs {
  std::string used_letters;
  char const* dictionary_file = NULL;
  int min_word_len = DFS_DEFAULT_MIN_WORD_LEN;
  int max_extract_words = 0;
  int top = 0;
  int search_threads = 1;
  double segment_penalty = DFS_DEFAULT_SEGMENT_PENALTY;
  bool min_word_len_given = false;
  bool max_extract_words_given = false;
  bool pairs_given = false;
};

enum DfsOptionResult {
  DFS_OPTION_OTHER,    // not a shared option; the CLI's own switch decides
  DFS_OPTION_HANDLED,
  DFS_OPTION_ERROR,    // diagnosed already
};

// Which shared option was just parsed, for callers that treat some specially.
// `score_incompatible` is false only for options that still mean something
// when no phase-1 extraction runs.
struct DfsCommonOption {
  char const* name;
  bool score_incompatible;
};

// Parses one shared option. `which` may be NULL and is written only when the
// result is DFS_OPTION_HANDLED.
DfsOptionResult dfs_parse_common_option(
    int option, struct optparse* options, DfsCommonArgs* out,
    DfsCommonOption* which);

// Reconciles --pairs with an explicit --max-extract-words. Call once after the
// option loop; prints an error and returns false when the two disagree.
bool dfs_finalize_common_args(DfsCommonArgs* args);
inline constexpr size_t DFS_DEFAULT_SCORE_CACHE_MIB = 64;
inline constexpr unsigned int DFS_DEFAULT_MAX_PREPROCESS_THREADS = 20;
inline constexpr size_t DFS_MIB = size_t(1024) * size_t(1024);

// Copies in-only lowercase a-z/0-9 characters from `in` into `out`, skipping
// spaces. Prints an error naming `what` and returns false on any other
// character.
bool clean_letters(char const* in, char const* what, std::string* out);

// Removes the multiset `used` from the multiset `bag`, writing the remainder
// (sorted by character) to `out`. Prints an error and returns false if `used`
// contains a letter not available in `bag`, or if nothing is left.
bool subtract_letters(std::string const& bag, std::string const& used,
                      std::string* out);

// Rejects a bag wider than DFS_MAX_BAG_LETTERS. Call this on the
// post-subtraction bag, not on the raw argument: the packed-record field widths
// depend on what phase 1 extracts, and subtraction only shrinks.
bool check_bag_length(std::string const& bag);

// Parses a non-negative base-10 integer from `in`. Prints an error naming
// `what` and returns false on any parse failure or out-of-range value.
bool parse_count(char const* in, char const* what, int* out);

// Parses a non-negative base-10 64-bit integer from `in`.
bool parse_count64(char const* in, char const* what, int64_t* out);

// Parses a non-negative MiB count into bytes, rejecting overflow.
bool parse_mib(char const* in, char const* what, size_t* out);

// Parses a finite floating-point number from `in`. Prints an error naming
// `what` and returns false on any parse failure or non-finite value.
bool parse_double(char const* in, char const* what, double* out);

// Parses a finite segment penalty at least 1. Values below 1 would make
// appended segments score-improving and invalidate phase-2 pruning.
bool parse_segment_penalty(char const* in, double* out);

// Applies dfs-anagrams' short-input default adjustment and validates that the
// resulting minimum can fit in the remaining bag.
bool finalize_min_word_length(
    std::string const& letters, bool explicitly_given, int* min_word_len);

// Resolves 0 (automatic) to one thread below 26 letters and otherwise to the
// available hardware concurrency capped at the shared production maximum.
size_t resolve_preprocess_threads(int requested, size_t letter_count);

// Loads a newline-delimited word list, lowercased, skipping lines containing
// '-' and stripping characters outside a-z/0-9. Prints an error and returns
// false if the file can't be opened or read.
bool load_dictionary(char const* path, DfsDictionary* dictionary);

#endif
