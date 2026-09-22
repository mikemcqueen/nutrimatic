#ifndef NUTRIMATIC_DFS_CLI_ARGS_H
#define NUTRIMATIC_DFS_CLI_ARGS_H

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "dfs-class-list.h"
#include "dfs-score.h"
#include "optparse.h"

// Shared argument-parsing helpers for the dfs-anagrams-family CLIs
// (dfs-anagrams, query-index): letter-bag cleanup/subtraction, integer
// option parsing, dictionary loading, and the options both CLIs accept.

inline constexpr int DFS_DEFAULT_MIN_WORD_LEN = 4;

// Long-option codes for shared options with no short form. They sit well above
// the range each CLI uses for its own codes, so adding a private option cannot
// silently collide with a shared one.
inline constexpr int DFS_OPT_DICT = 300;
inline constexpr int DFS_OPT_PAIRS = 301;
inline constexpr int DFS_OPT_WORD_BONUS = 302;
inline constexpr int DFS_OPT_PAIR_BONUS = 303;
inline constexpr int DFS_OPT_SOLO_WORDS = 304;
inline constexpr int DFS_OPT_HIDE_SOLO_WORDS = 305;
inline constexpr int DFS_OPT_SEED_PAIRS = 306;
inline constexpr int DFS_OPT_YES_PAIRS = 307;
inline constexpr int DFS_OPT_BEST_PAIRS = 308;
inline constexpr int DFS_OPT_WF = 309;
inline constexpr int DFS_OPT_WFROOT = 310;
inline constexpr int DFS_OPT_MORE_BEST_PAIRS = 311;
inline constexpr int DFS_OPT_NO_SCORE = 312;

// The rows both CLIs contribute to their optparse_long table. A macro rather
// than a shared array because optparse terminates on a NULL row, so each CLI
// still needs one table of its own; this splices the common rows into it.
#define DFS_COMMON_LONG_OPTIONS \
  { "used-letters", 'u', OPTPARSE_REQUIRED }, \
  { "dict", DFS_OPT_DICT, OPTPARSE_REQUIRED }, \
  { "min-word-length", 'm', OPTPARSE_REQUIRED }, \
  { "max-words", 'x', OPTPARSE_REQUIRED }, \
  { "pairs", DFS_OPT_PAIRS, OPTPARSE_REQUIRED }, \
  { "top", 'n', OPTPARSE_REQUIRED }, \
  { "search-threads", 'S', OPTPARSE_REQUIRED }, \
  { "segment-penalty", 'P', OPTPARSE_REQUIRED }, \
  { "word-bonus", DFS_OPT_WORD_BONUS, OPTPARSE_REQUIRED }, \
  { "pair-bonus", DFS_OPT_PAIR_BONUS, OPTPARSE_REQUIRED }, \
  { "solo-words", DFS_OPT_SOLO_WORDS, OPTPARSE_REQUIRED }, \
  { "hide-solo-words", DFS_OPT_HIDE_SOLO_WORDS, OPTPARSE_NONE }, \
  { "seed-pairs", DFS_OPT_SEED_PAIRS, OPTPARSE_REQUIRED }, \
  { "yes-pairs", DFS_OPT_YES_PAIRS, OPTPARSE_REQUIRED }, \
  { "best-pairs", DFS_OPT_BEST_PAIRS, OPTPARSE_REQUIRED }, \
  { "more-best-pairs", DFS_OPT_MORE_BEST_PAIRS, OPTPARSE_REQUIRED }, \
  { "wf", DFS_OPT_WF, OPTPARSE_NONE }, \
  { "wfroot", DFS_OPT_WFROOT, OPTPARSE_REQUIRED }, \
  { "target", 't', OPTPARSE_REQUIRED }

// What the shared options parsed into. `top` has no shared default because the
// two CLIs disagree on it; each sets its own before the option loop.
struct DfsCommonArgs {
  std::string used_letters;
  char const* dictionary_file = NULL;
  char const* pair_file = NULL;
  int min_word_len = DFS_DEFAULT_MIN_WORD_LEN;
  int max_extract_words = 0;
  int top = 0;
  int search_threads = 1;
  double segment_penalty = DFS_DEFAULT_SEGMENT_PENALTY;
  double word_bonus = DFS_DEFAULT_WORD_BONUS;
  double pair_bonus = DFS_DEFAULT_PAIR_BONUS;
  std::vector<std::string> seed_pair_files;
  std::vector<std::string> yes_pair_files;
  std::vector<std::string> best_pair_files;
  std::string one_best_pair;
  bool best_pairs_given = false;
  bool workflow = false;
  std::string workflow_root;
  std::string target;
  bool target_complete = false;
  std::string workflow_index_file;
  std::string workflow_dictionary_file;
  std::vector<std::string> solo_words;
  bool hide_solo_words = false;
  bool show_score = true;
  bool min_word_len_given = false;
  bool max_extract_words_given = false;
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

// Resolves the common arguments after option parsing: raw scalar bonuses are
// validated and the effective legacy pair bonus is normalized, then the
// workflow defaults and weighted pair sources are resolved, then the
// workflow's reject files are appended to `workflow_reject_files`.
// A NULL `workflow_reject_files` skips that last step for a caller that excludes
// nothing. The order is fixed here because each step reads what the one
// before it resolved. `allow_targetless_workflow` lets query-index use the
// workflow without a target or seed, and skips its classified YES source
// when no target is selected.
bool finalize_dfs_common_args(
    DfsCommonArgs* args, char const* program, char const** index_file,
    std::vector<std::string>* workflow_reject_files,
    bool allow_targetless_workflow = false);

struct DfsWorkflowTargetSettings {
  std::string letters;
  int min_word_len;
  int num_segments;
};

// Reads and derives the generator inputs encoded by a complete workflow
// target. The sentence bag comes from ROOT/.wf/best/sN/letters; o- selects
// the target label's letters and u- subtracts them. Call after
// finalize_dfs_common_args().
bool load_dfs_workflow_target_settings(
    DfsCommonArgs const& args, char const* program,
    DfsWorkflowTargetSettings* out);

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

// Reports the cleaned, post-subtraction letter bag to the diagnostic stream.
void dfs_diagnostic_letter_bag(std::string const& letters);

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

// Appends a comma-separated direct CLI value. Fields are intentionally strict:
// nonempty lowercase a-z/0-9 only, unique across repeated occurrences, with a
// shared maximum of 16 external partners.
bool parse_solo_words(
    char const* in, std::vector<std::string>* solo_words);

// Applies dfs-anagrams' short-input default adjustment and validates that the
// resulting minimum can fit in the remaining bag.
bool finalize_min_word_length(
    std::string const& letters, bool explicitly_given, int* min_word_len);

// Resolves 0 (automatic) to one thread below 26 letters and otherwise to the
// available hardware concurrency capped at the shared production maximum.
size_t resolve_preprocess_threads(int requested, size_t letter_count);

// Resolves 0 (automatic) to the available hardware concurrency, using one
// thread when the implementation cannot report it.
size_t resolve_search_threads(int requested);

// Loads a newline-delimited word list, lowercased, skipping lines containing
// '-' and stripping characters outside a-z/0-9. Prints an error and returns
// false if the file can't be opened or read.
bool load_dictionary(char const* path, DfsDictionary* dictionary);

// One normalized pair-file row in written order. right is empty for a
// standalone word; line_number is 1-based.
struct DfsPairRow {
  std::string left;
  std::string right;
  size_t line_number;

  std::string entry(bool reverse = false) const {
    if (right.empty()) return left;
    return reverse ? right + " " + left : left + " " + right;
  }
};

// Parse one PAIRS line using the same validation and normalization as the
// shared file loader. Prints a source/line diagnostic on failure.
bool parse_pair_row(
    std::string const& line, char const* what, char const* source,
    size_t line_number, bool allow_single_words, DfsPairRow* out);

// A pair set owns normalized whole-entry keys. The general loader below owns
// every two-word pair as both "left right" and "right left"; the extraction
// loader may keep a short-word pair in only its written orientation. When
// single words are allowed, each is owned as one key without a space.
//
// Loads a newline-delimited list of "word,word" pairs, applying
// load_dictionary()'s cleanup to each field, and inserts both word orders.
// When allow_single_words, a line without a comma is loaded as one key.
// Lines containing '-' are skipped, or are an error when reject_hyphens: a
// skipped line costs a bonus list nothing, but silently drops an entry a
// caller meant to enforce. Unless quiet, reports the number of pairs read and
// unique ordered keys loaded. When diagnostic_source is non-NULL, appends it
// to that summary. Prints an error and returns false if the file can't be
// opened or read, or if any surviving line does not hold an allowed number of
// nonempty fields. `what` names the list in every diagnostic. Non-NULL rows
// receives every row in written order, for a caller that must repeat the
// file's own word order rather than a key's.
bool load_pair_file(
    char const* path, char const* what, DfsPairSet* pairs, bool quiet,
    bool reject_hyphens, bool allow_single_words = false,
    char const* diagnostic_source = NULL,
    std::vector<DfsPairRow>* rows = NULL);

// As load_pair_file(), but accepts standalone entries only to discard them.
// Adds their number to `ignored_single_words`; pairs are loaded normally.
bool load_pair_file_ignoring_single_words(
    char const* path, char const* what, DfsPairSet* pairs,
    size_t* ignored_single_words);

// Loads an ordinary extraction bonus list after -m has been finalized.
// Standalone words and pairs must contain at least min_word_len normalized
// non-space characters in total. Pairs containing a word shorter than the
// minimum are stored only in written order, and their first words are added to
// exception_prefixes so phase 1 can reach the exact exceptional entry. Other
// pairs retain the general loader's symmetric representation. Neither output
// is modified unless the entire input parses and validates successfully.
bool load_extraction_pair_file(
    char const* path, char const* what, int min_word_len,
    DfsPairSet* pairs, DfsPairSet* exception_prefixes, bool quiet,
    bool reject_hyphens, std::vector<DfsPairRow>* rows = NULL);

// Loads the repeatable fixed-tier pair inputs. Duplicate and reversed keys
// retain the strongest source. `score_mode` uses the symmetric score loader;
// extraction mode preserves the short-word directional rules and accumulates
// their exception prefixes. In extraction mode, non-NULL rows receives every
// YES file's rows and non-NULL best_rows every BEST file's rows, as
// load_extraction_pair_file() reports them. A nonempty `args.one_best_pair`
// is loaded after the BEST files as one further BEST entry, with the same
// semantics a BEST file holding that one line would have. Seed rows are
// currently turned off due to excessive occurrences of non-dictionary words.
bool load_weighted_pair_files(
    DfsCommonArgs const& args, bool score_mode, int min_word_len,
    DfsPairBonusMap* pairs, DfsPairSet* exception_prefixes,
    std::vector<DfsPairRow>* rows = NULL,
    std::vector<DfsPairRow>* best_rows = NULL);

// Loads and combines exclusion lists in the same format from regular files and
// at most one workflow root, which resolves to
// DIR/.wf/classified/no/no.pairs. A directory without a .wf subdirectory is an
// error, and so is a '-' line: asking for exclusions is explicit, so neither a
// missing workflow nor an unreadable line quietly narrows the exclusion set.
// With allow_single_words, a regular file's single-word line is kept as a
// bare-word key; a workflow directory's no.pairs stays pairs-only either way.
bool load_reject_files(
    std::vector<std::string> const& paths, bool allow_single_words,
    DfsPairSet* pairs);

#endif
