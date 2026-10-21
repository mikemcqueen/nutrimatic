#ifndef NUTRIMATIC_PARSE_NUMBER_H
#define NUTRIMATIC_PARSE_NUMBER_H

#include <charconv>
#include <string_view>
#include <system_error>

// Whether all of `text` is one number of type T, as std::from_chars reads it:
// no leading space or '+', and nothing after the number. Out-of-range values
// fail. `*out` is set only on success.
template <typename T>
bool parse_number(std::string_view text, T* out) {
  char const* const end = text.data() + text.size();
  T value;
  std::from_chars_result const result =
      std::from_chars(text.data(), end, value);
  if (result.ec != std::errc() || result.ptr != end) return false;
  *out = value;
  return true;
}

#endif
