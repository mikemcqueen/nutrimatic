#ifndef NUTRIMATIC_LETTER_BAG_H
#define NUTRIMATIC_LETTER_BAG_H

#include <limits.h>
#include <stddef.h>

#include <array>
#include <string>

// How many of each letter a through z a bag holds, 'a' + j at j.
using LetterCounts = std::array<int, 26>;

// The percentage of English text that is 'a' + j, at j.
extern double const ENGLISH_PERCENT[26];

// A multiset of letters that words and segments are checked against.
struct LetterBag {
  int counts[UCHAR_MAX + 1] = { 0 };
  size_t size = 0;

  LetterBag() = default;
  explicit LetterBag(std::string const& letters);
};

// Removes the multiset `used` from the multiset `bag`, writing the remainder
// (sorted by character) to `out`. Prints an error and returns false if `bag`
// is empty, if `used` contains a letter not available in `bag`, or if nothing
// is left.
bool subtract_letters(std::string const& bag, std::string const& used,
                      std::string* out);

// The a-z/0-9 characters of `in`, lowercased; every other character is
// dropped. Every LETTERS bag and used-letters argument goes through this.
std::string bag_letters(std::string const& in);

// bag_letters() without the digits, for tools that count only a-z.
std::string az_letters(std::string const& in);

// Filters `letters` and `used_letters` through bag_letters() and fills `out`
// with what is left of the first once the second is removed. Errors are
// diagnosed already.
bool make_letter_bag(
    char const* letters, std::string const& used_letters, LetterBag* out);

// Whether the letters of `text`, spaces skipped, fit within `bag`.
bool fits_letter_bag(LetterBag const& bag, std::string const& text);

// The consonant/vowel ratio of `counts`, the number of each letter a through
// z; y is a consonant. 0 when there are no consonants, and infinity when there
// are consonants but no vowels.
double cv_ratio(LetterCounts const& counts);

// cv_ratio() of the a-z letters left in `bag` once those of `text` are
// removed. `text` must fit within `bag`.
double remaining_cv_ratio(LetterBag const& bag, std::string const& text);

#endif
