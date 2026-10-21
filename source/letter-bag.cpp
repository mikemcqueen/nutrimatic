#include "letter-bag.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "log.h"

double const ENGLISH_PERCENT[26] = {
  8.167, 1.492, 2.782, 4.253, 12.702, 2.228, 2.015, 6.094, 6.966,
  0.153, 0.772, 4.025, 2.406, 6.749, 7.507, 1.929, 0.095, 5.987,
  6.327, 9.056, 2.758, 0.978, 2.360, 0.150, 1.974, 0.074,
};

bool subtract_letters(std::string const& bag, std::string const& used,
                      std::string* out) {
  if (bag.empty()) {
    fputs("error: no letters given\n", stderr);
    return false;
  }
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

std::string bag_letters(std::string const& in) {
  std::string out;
  for (char c : in) {
    int const ch = tolower((unsigned char) c);
    if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))
      out.push_back(char(ch));
  }
  return out;
}

std::string az_letters(std::string const& in) {
  std::string out;
  for (char c : in) {
    int const ch = tolower((unsigned char) c);
    if (ch >= 'a' && ch <= 'z') out.push_back(char(ch));
  }
  return out;
}

bool load_letter_bag(std::filesystem::path const& path,
                     std::string_view program, std::string* out) {
  FILE* const file = fopen(path.c_str(), "rb");
  if (file == NULL) {
    report(program, "can't open letters \"%s\": %s", path.c_str(),
           strerror(errno));
    return false;
  }
  std::string text;
  char buffer[4096];
  size_t got;
  while ((got = fread(buffer, 1, sizeof buffer, file)) > 0)
    text.append(buffer, got);
  int const error = ferror(file) ? errno : 0;
  fclose(file);
  if (error != 0) {
    report(program, "can't read letters \"%s\": %s", path.c_str(),
           strerror(error));
    return false;
  }
  *out = bag_letters(text);
  return true;
}

LetterBag::LetterBag(std::string const& letters) : size(letters.size()) {
  for (size_t i = 0; i < letters.size(); ++i)
    ++counts[(unsigned char) letters[i]];
}

bool make_letter_bag(
    char const* letters, std::string const& used_letters, LetterBag* out) {
  std::string remaining;
  if (!subtract_letters(bag_letters(letters),
                        bag_letters(used_letters), &remaining))
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
