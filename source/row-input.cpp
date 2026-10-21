#include "row-input.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include "log.h"

bool read_input_file(
    std::string_view program, char const* path,
    std::function<bool(std::istream&)> const& read) {
  if (strcmp(path, "-") == 0) return read(std::cin);
  errno = 0;
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    report(program, "can't open \"%s\": %s", path, strerror(errno));
    return false;
  }
  return read(input);
}

bool read_lines(
    std::string_view program, char const* path,
    std::function<bool(std::string const&, size_t)> const& visit) {
  return read_input_file(program, path, [&](std::istream& input) {
    std::string line;
    size_t number = 0;
    while (std::getline(input, line))
      if (!visit(line, ++number)) return false;
    if (!input.eof()) {
      report(program, "can't read \"%s\"", path);
      return false;
    }
    return true;
  });
}

bool read_rows(
    std::string_view program, char const* path, std::string_view what,
    bool allow_single_words,
    std::function<bool(DfsPairRow const&, std::string const&)> const& visit) {
  return read_lines(program, path,
      [&](std::string const& line, size_t number) {
        DfsPairRow row;
        return parse_pair_row(
                   line, what, path, number, allow_single_words, &row) &&
            visit(row, line);
      });
}

bool print_kept_rows(
    std::string_view program, char const* path, std::string_view what,
    bool allow_single_words,
    std::function<bool(DfsPairRow const&)> const& keep) {
  return read_rows(program, path, what, allow_single_words,
      [&](DfsPairRow const& row, std::string const& line) {
        if (keep(row) && !(std::cout << line << '\n')) {
          report(program, "can't write output");
          return false;
        }
        return true;
      });
}
