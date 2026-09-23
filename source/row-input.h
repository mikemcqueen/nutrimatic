#ifndef NUTRIMATIC_ROW_INPUT_H
#define NUTRIMATIC_ROW_INPUT_H

#include <functional>
#include <istream>

#include "dfs-cli-args.h"

// Calls `read` on `path` opened for reading, or on standard input when `path`
// is '-', and returns what it returns. A file that can't be opened is
// diagnosed, prefixed by `program`.
bool read_input_file(
    char const* program, char const* path,
    std::function<bool(std::istream&)> const& read);

// Prints each line of `path`, '-' for standard input, whose row `keep`
// accepts. Rows are parsed as parse_pair_row() parses them, and `what` names
// them in its diagnostics. Errors are diagnosed already, prefixed by
// `program`; the caller flushes standard output.
bool print_kept_rows(
    char const* program, char const* path, char const* what,
    bool allow_single_words,
    std::function<bool(DfsPairRow const&)> const& keep);

#endif
