#include "letter-bag.h"

#include <math.h>
#include <string.h>

#include "dfs-cli-args.h"

LetterBag::LetterBag(std::string const& letters) : size(letters.size()) {
  for (size_t i = 0; i < letters.size(); ++i)
    ++counts[(unsigned char) letters[i]];
}

bool make_letter_bag(
    char const* letters, std::string const& used_letters, LetterBag* out) {
  std::string bag;
  std::string remove;
  std::string remaining;
  if (!clean_letters(letters, "letters", &bag) ||
      !clean_letters(used_letters.c_str(), "used letters", &remove) ||
      !subtract_letters(bag, remove, &remaining))
    return false;
  *out = LetterBag(remaining);
  return true;
}

bool fits_letter_bag(LetterBag const& bag, std::string const& text) {
  int need[UCHAR_MAX + 1] = { 0 };
  for (char ch : text) {
    if (ch == ' ') continue;
    unsigned char const index = (unsigned char) ch;
    if (++need[index] > bag.counts[index]) return false;
  }
  return true;
}

double cv_ratio(int const counts[26]) {
  int vowels = 0;
  int consonants = 0;
  for (int i = 0; i < 26; ++i) {
    if (strchr("aeiou", 'a' + i) != NULL)
      vowels += counts[i];
    else
      consonants += counts[i];
  }
  if (consonants == 0) return 0;
  if (vowels == 0) return INFINITY;
  return double(consonants) / vowels;
}
