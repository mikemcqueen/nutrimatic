#ifndef NUTRIMATIC_TEXT_UTIL_H
#define NUTRIMATIC_TEXT_UTIL_H

#include <string_view>

// `text` without leading or trailing whitespace (the characters isspace()
// matches in the "C" locale).
inline std::string_view trim(std::string_view const text) {
  std::string_view const space = " \t\n\v\f\r";
  size_t const begin = text.find_first_not_of(space);
  if (begin == std::string_view::npos) return std::string_view();
  return text.substr(begin, text.find_last_not_of(space) + 1 - begin);
}

#endif  // NUTRIMATIC_TEXT_UTIL_H
