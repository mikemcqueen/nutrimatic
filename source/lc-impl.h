#ifndef NUTRIMATIC_LC_IMPL_H
#define NUTRIMATIC_LC_IMPL_H

#include <string>
#include <vector>

enum LetterCountMode {
  LETTER_COUNT_TABLE,
  LETTER_COUNT_REMAINING,
  LETTER_COUNT_CV,
};

// What lc reports on the letters of text less those of used_letters; see
// lc's usage(). Case and non-letters in either are ignored.
struct LetterCountOptions {
  std::string text;                           // TEXT, or -f FILE
  std::string used_letters;                   // USED...
  LetterCountMode mode = LETTER_COUNT_TABLE;  // -r, --cv
};

// Fills `out` with lc's output under `options`, one line per element.
// Returns false, with the error diagnosed, when used_letters aren't all in
// text or nothing is left.
bool letter_count_lines(
    LetterCountOptions const& options, std::vector<std::string>* out);

#endif
