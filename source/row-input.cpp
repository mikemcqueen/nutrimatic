#include "row-input.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <fstream>
#include <iostream>
#include <string>

bool read_input_file(
    char const* program, char const* path,
    std::function<bool(std::istream&)> const& read) {
  if (strcmp(path, "-") == 0) return read(std::cin);
  errno = 0;
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    fprintf(stderr, "%s: can't open \"%s\": %s\n",
        program, path, strerror(errno));
    return false;
  }
  return read(input);
}

bool read_rows(
    char const* program, char const* path, char const* what,
    bool allow_single_words,
    std::function<bool(DfsPairRow const&, std::string const&)> const& visit) {
  return read_input_file(program, path, [&](std::istream& input) {
    std::string line;
    size_t number = 0;
    while (std::getline(input, line)) {
      ++number;
      DfsPairRow row;
      if (!parse_pair_row(
              line, what, path, number, allow_single_words, &row) ||
          !visit(row, line))
        return false;
    }
    if (!input.eof()) {
      fprintf(stderr, "%s: can't read \"%s\"\n", program, path);
      return false;
    }
    return true;
  });
}

bool print_kept_rows(
    char const* program, char const* path, char const* what,
    bool allow_single_words,
    std::function<bool(DfsPairRow const&)> const& keep) {
  return read_rows(program, path, what, allow_single_words,
      [&](DfsPairRow const& row, std::string const& line) {
        if (keep(row) && !(std::cout << line << '\n')) {
          fprintf(stderr, "%s: can't write output\n", program);
          return false;
        }
        return true;
      });
}
