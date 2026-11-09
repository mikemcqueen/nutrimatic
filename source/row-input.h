#ifndef NUTRIMATIC_ROW_INPUT_H
#define NUTRIMATIC_ROW_INPUT_H

#include <stdint.h>

#include <filesystem>
#include <functional>
#include <istream>
#include <string>
#include <string_view>

#include "dfs-cli-args.h"

// What each line of a row file holds: word,word; that or a single word; or
// COUNT word word.
enum class RowFormat { pairs, pairs_or_words, ranked };

// Calls `read` on `path` opened for reading, or on standard input when `path`
// is '-', and returns what it returns. A file that can't be opened is
// diagnosed, prefixed by `program`.
bool read_input_file(
    std::string_view program, std::filesystem::path const& path,
    std::function<bool(std::istream&)> const& read);

// Calls `visit` on each line of `path`, '-' for standard input, and its
// 1-based number. Stops and returns false when `visit` does. Errors are
// diagnosed already, prefixed by `program`.
bool read_lines(
    std::string_view program, std::filesystem::path const& path,
    std::function<bool(std::string const&, size_t)> const& visit);

// As read_lines(), but calls `visit` on each line's COUNT, 0 unless `format`
// is ranked, then its row, parsed by parse_ranked_row() or parse_pair_row()
// with `what` naming rows in its diagnostics, and the line.
bool read_rows(
    std::string_view program, std::filesystem::path const& path,
    std::string_view what, RowFormat format,
    std::function<bool(uint64_t, DfsPairRow const&, std::string const&)> const&
        visit);

// Prints each line of `path`, '-' for standard input, whose row `keep`
// accepts. Rows are parsed as read_rows() parses them, and `what` names them
// in its diagnostics. Errors are diagnosed already, prefixed by `program`;
// the caller flushes standard output.
bool print_kept_rows(
    std::string_view program, std::filesystem::path const& path,
    std::string_view what, RowFormat format,
    std::function<bool(DfsPairRow const&)> const& keep);

// Appends `segments` to `out` as a top-segments row: "0 ", then `segments`
// with each `separator` written as a comma, then a newline.
void append_top_segments_row(std::string* out, std::string_view segments,
                             std::string_view separator);

#endif
