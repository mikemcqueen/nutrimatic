#ifndef NUTRIMATIC_DFS_CLI_ARGS_H
#define NUTRIMATIC_DFS_CLI_ARGS_H

#include <stddef.h>

#include <string>

#include "dfs-class-list.h"

// Shared argument-parsing helpers for the dfs-anagrams-family CLIs
// (dfs-anagrams, query-index): letter-bag cleanup/subtraction, integer
// option parsing, and dictionary loading.

inline constexpr int DFS_DEFAULT_MIN_WORD_LEN = 4;
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

// Parses a non-negative base-10 integer from `in`. Prints an error naming
// `what` and returns false on any parse failure or out-of-range value.
bool parse_count(char const* in, char const* what, int* out);

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
