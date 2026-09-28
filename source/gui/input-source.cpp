#include "input-source.h"

#include "row-input.h"

bool read_file_lines(std::string const& path, Lines* lines) {
  lines->clear();
  return read_lines("pgui", path.c_str(), [&](std::string const& line, size_t) {
    lines->push_back(line);
    return true;
  });
}
