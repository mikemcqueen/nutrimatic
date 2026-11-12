#include "row-input.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include "log.h"

bool read_input_file(
    std::string_view program, std::filesystem::path const& path,
    std::function<bool(std::istream&)> const& read) {
  if (path == "-") return read(std::cin);
  errno = 0;
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    report(program, "can't open \"%s\": %s", path.c_str(), strerror(errno));
    return false;
  }
  return read(input);
}

bool read_lines(
    std::string_view program, std::filesystem::path const& path,
    std::function<bool(std::string const&, size_t)> const& visit) {
  return read_input_file(program, path, [&](std::istream& input) {
    std::string line;
    size_t number = 0;
    while (std::getline(input, line))
      if (!visit(line, ++number)) return false;
    if (!input.eof()) {
      report(program, "can't read \"%s\"", path.c_str());
      return false;
    }
    return true;
  });
}

namespace {

RowFormat line_format(std::string_view line) {
  size_t const start = std::min(line.find_first_not_of(' '), line.size());
  size_t const space = line.find(' ', start);
  std::string_view const lead = line.substr(start, space - start);
  if (space != std::string_view::npos && !lead.empty() &&
      std::all_of(lead.begin(), lead.end(),
                  [](char ch) { return ch >= '0' && ch <= '9'; }))
    return line.find(' ', space + 1) == std::string_view::npos
        ? RowFormat::ranked_words : RowFormat::ranked;
  return line.find(',') == std::string_view::npos
      ? RowFormat::words : RowFormat::pairs;
}

char const* format_name(RowFormat format) {
  switch (format) {
    case RowFormat::pairs: return "word,word";
    case RowFormat::pairs_or_words: return "word or word,word";
    case RowFormat::ranked: return "COUNT word word";
    case RowFormat::words: return "word";
    case RowFormat::ranked_words: return "COUNT word";
    case RowFormat::detect: break;
  }
  return "a row";
}

}  // namespace

bool read_rows(
    std::string_view program, std::filesystem::path const& path,
    std::string_view what, RowFormat format,
    std::function<bool(uint64_t, DfsPairRow const&, std::string const&)> const&
        visit) {
  bool const strict = format == RowFormat::detect ||
      format == RowFormat::words || format == RowFormat::ranked_words;
  RowFormat file_format = format;
  return read_lines(program, path,
      [&](std::string const& line, size_t number) {
        if (strict) {
          RowFormat const found = line_format(line);
          if (file_format == RowFormat::detect) file_format = found;
          if (found != file_format) {
            fprintf(stderr,
                "error: %.*s \"%s\" line %zu: expected %s, the format %s\n",
                SV_ARG(what), path.c_str(), number, format_name(file_format),
                format == RowFormat::detect
                    ? "of line 1" : "required here");
            return false;
          }
        }
        DfsPairRow row;
        uint64_t count = 0;
        bool parsed = false;
        switch (file_format) {
          case RowFormat::ranked:
          case RowFormat::ranked_words:
            parsed = parse_ranked_row(line, what, path.native(), number,
                file_format == RowFormat::ranked_words, &row, &count);
            break;
          case RowFormat::pairs:
          case RowFormat::pairs_or_words:
          case RowFormat::words:
          case RowFormat::detect:
            parsed = parse_pair_row(line, what, path.native(), number,
                file_format != RowFormat::pairs, &row);
            break;
        }
        return parsed && visit(count, row, line);
      });
}

bool print_kept_rows(
    std::string_view program, std::filesystem::path const& path,
    std::string_view what, RowFormat format,
    std::function<bool(DfsPairRow const&)> const& keep) {
  return read_rows(program, path, what, format,
      [&](uint64_t, DfsPairRow const& row, std::string const& line) {
        if (keep(row) && !(std::cout << line << '\n')) {
          report(program, "can't write output");
          return false;
        }
        return true;
      });
}

void append_top_segments_row(std::string* out, std::string_view segments,
                             std::string_view separator) {
  *out += "0 ";
  for (size_t at; (at = segments.find(separator)) != std::string_view::npos;) {
    *out += segments.substr(0, at);
    *out += ',';
    segments.remove_prefix(at + separator.size());
  }
  *out += segments;
  *out += '\n';
}
