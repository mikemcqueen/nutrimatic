#include "letter-bag.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

double const ENGLISH_PERCENT[26] = {
  8.167, 1.492, 2.782, 4.253, 12.702, 2.228, 2.015, 6.094, 6.966,
  0.153, 0.772, 4.025, 2.406, 6.749, 7.507, 1.929, 0.095, 5.987,
  6.327, 9.056, 2.758, 0.978, 2.360, 0.150, 1.974, 0.074,
};

bool clean_letters(char const* in, char const* what, std::string* out) {
  for (; *in != '\0'; ++in) {
    if (*in == ' ') continue;
    if ((*in < 'a' || *in > 'z') && (*in < '0' || *in > '9')) {
      fprintf(stderr, "error: bad character '%c' in %s\n", *in, what);
      return false;
    }
    out->push_back(*in);
  }
  return true;
}

bool subtract_letters(std::string const& bag, std::string const& used,
                      std::string* out) {
  int have[UCHAR_MAX + 1] = { 0 };
  for (size_t i = 0; i < bag.size(); ++i)
    ++have[(unsigned char) bag[i]];

  for (size_t i = 0; i < used.size(); ++i) {
    unsigned char const ch = (unsigned char) used[i];
    if (have[ch] == 0) {
      fprintf(stderr, "error: no '%c' left in \"%s\" to use\n",
              ch, bag.c_str());
      return false;
    }
    --have[ch];
  }

  out->clear();
  for (int ch = 0; ch <= UCHAR_MAX; ++ch)
    out->append(size_t(have[ch]), char(ch));

  if (out->empty()) {
    fputs("error: no letters left after removing used letters\n", stderr);
    return false;
  }
  return true;
}

std::string lowercase_letters(std::string const& in) {
  std::string out;
  for (char c : in) {
    int const ch = tolower((unsigned char) c);
    if (ch >= 'a' && ch <= 'z') out.push_back(char(ch));
  }
  return out;
}

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

double cv_ratio(LetterCounts const& counts) {
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

double remaining_cv_ratio(LetterBag const& bag, std::string const& text) {
  LetterCounts counts;
  for (int i = 0; i < 26; ++i) counts[i] = bag.counts['a' + i];
  for (char ch : text)
    if (ch >= 'a' && ch <= 'z') --counts[ch - 'a'];
  return cv_ratio(counts);
}
