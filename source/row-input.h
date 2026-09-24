#ifndef NUTRIMATIC_ROW_INPUT_H
#define NUTRIMATIC_ROW_INPUT_H

#include <functional>
#include <istream>
#include <string>

#include "dfs-cli-args.h"

// Calls `read` on `path` opened for reading, or on standard input when `path`
// is '-', and returns what it returns. A file that can't be opened is
// diagnosed, prefixed by `program`.
bool read_input_file(
    char const* program, char const* path,
    std::function<bool(std::istream&)> const& read);

// Calls `visit` on each line of `path`, '-' for standard input, and its
// 1-based number. Stops and returns false when `visit` does. Errors are
// diagnosed already, prefixed by `program`.
bool read_lines(
    char const* program, char const* path,
    std::function<bool(std::string const&, size_t)> const& visit);

// As read_lines(), but calls `visit` on each line's row, parsed as
// parse_pair_row() parses it with `what` naming rows in its diagnostics, and
// the line.
bool read_rows(
    char const* program, char const* path, char const* what,
    bool allow_single_words,
    std::function<bool(DfsPairRow const&, std::string const&)> const& visit);

// Prints each line of `path`, '-' for standard input, whose row `keep`
// accepts. Rows are parsed as parse_pair_row() parses them, and `what` names
// them in its diagnostics. Errors are diagnosed already, prefixed by
// `program`; the caller flushes standard output.
bool print_kept_rows(
    char const* program, char const* path, char const* what,
    bool allow_single_words,
    std::function<bool(DfsPairRow const&)> const& keep);

#endif
