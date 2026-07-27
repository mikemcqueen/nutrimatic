#include "dfs-cli-args.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <fstream>
#include <string>

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

bool parse_count(char const* in, char const* what, int* out) {
  char* end;
  long const value = strtol(in, &end, 10);
  if (*in == '\0' || *end != '\0' || value < 0 || value > INT_MAX) {
    fprintf(stderr, "error: %s needs a count, not \"%s\"\n", what, in);
    return false;
  }
  *out = int(value);
  return true;
}

bool parse_double(char const* in, char const* what, double* out) {
  char* end;
  double const value = strtod(in, &end);
  if (*in == '\0' || *end != '\0' || !isfinite(value)) {
    fprintf(stderr, "error: %s needs a number, not \"%s\"\n", what, in);
    return false;
  }
  *out = value;
  return true;
}

bool load_dictionary(char const* path, DfsDictionary* dictionary) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    fprintf(stderr, "error: can't open dictionary \"%s\"\n", path);
    return false;
  }

  std::string line;
  while (std::getline(input, line)) {
    if (line.find('-') != std::string::npos) continue;

    std::string word;
    word.reserve(line.size());
    for (size_t i = 0; i < line.size(); ++i) {
      unsigned char const ch = (unsigned char) line[i];
      if (ch >= 'A' && ch <= 'Z')
        word.push_back(char(ch - 'A' + 'a'));
      else if ((ch >= 'a' && ch <= 'z') ||
               (ch >= '0' && ch <= '9'))
        word.push_back(char(ch));
    }
    if (!word.empty()) dictionary->insert(word);
  }

  if (!input.eof()) {
    fprintf(stderr, "error: can't read dictionary \"%s\"\n", path);
    return false;
  }
  return true;
}

double multi_word_bonus(double word_bonus) {
  return pow(RESTART, -word_bonus);
}
