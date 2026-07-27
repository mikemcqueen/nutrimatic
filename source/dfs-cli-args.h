#ifndef NUTRIMATIC_DFS_CLI_ARGS_H
#define NUTRIMATIC_DFS_CLI_ARGS_H

#include <string>

#include "dfs-class-list.h"

// Shared argument-parsing helpers for the dfs-anagrams-family CLIs
// (dfs-anagrams, query-index): letter-bag cleanup/subtraction, integer
// option parsing, and dictionary loading.

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

// Parses a finite floating-point number from `in`. Prints an error naming
// `what` and returns false on any parse failure or non-finite value.
bool parse_double(char const* in, char const* what, double* out);

// Loads a newline-delimited word list, lowercased, skipping lines containing
// '-' and stripping characters outside a-z/0-9. Prints an error and returns
// false if the file can't be opened or read.
bool load_dictionary(char const* path, DfsDictionary* dictionary);

#endif
