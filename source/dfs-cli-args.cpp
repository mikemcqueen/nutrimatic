#include "dfs-cli-args.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <thread>

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

bool parse_mib(char const* in, char const* what, size_t* out) {
  if (*in == '\0' || *in == '-') {
    fprintf(stderr, "error: %s needs a count, not \"%s\"\n", what, in);
    return false;
  }
  errno = 0;
  char* end;
  unsigned long long const value = strtoull(in, &end, 10);
  if (*end != '\0' || errno == ERANGE ||
      value > static_cast<unsigned long long>(SIZE_MAX / DFS_MIB)) {
    fprintf(stderr, "error: %s needs a count, not \"%s\"\n", what, in);
    return false;
  }
  *out = size_t(value) * DFS_MIB;
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

bool finalize_min_word_length(
    std::string const& letters, bool explicitly_given, int* min_word_len) {
  if (!explicitly_given && *min_word_len > int(letters.size()))
    *min_word_len = int(letters.size());

  if (*min_word_len > int(letters.size())) {
    fprintf(stderr,
        "error: no word of %d letters fits in the %zu left in \"%s\"\n",
        *min_word_len, letters.size(), letters.c_str());
    return false;
  }
  return true;
}

size_t resolve_preprocess_threads(int requested, size_t letter_count) {
  if (requested != 0) return size_t(requested);
  if (letter_count < 26) return 1;
  unsigned int const available = std::thread::hardware_concurrency();
  if (available <= 1) return 1;
  return size_t(std::min(
      available, DFS_DEFAULT_MAX_PREPROCESS_THREADS));
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
