#ifndef NUTRIMATIC_LETTER_BAG_H
#define NUTRIMATIC_LETTER_BAG_H

#include <limits.h>
#include <stddef.h>

#include <string>

// A multiset of letters that words and segments are checked against.
struct LetterBag {
  int counts[UCHAR_MAX + 1] = { 0 };
  size_t size = 0;

  LetterBag() = default;
  explicit LetterBag(std::string const& letters);
};

// Cleans `letters` and `used_letters` and fills `out` with what is left of
// the first once the second is removed. Errors are diagnosed already.
bool make_letter_bag(
    char const* letters, std::string const& used_letters, LetterBag* out);

// Whether the letters of `text`, spaces skipped, fit within `bag`.
bool fits_letter_bag(LetterBag const& bag, std::string const& text);

#endif
