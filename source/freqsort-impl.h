#ifndef NUTRIMATIC_FREQSORT_IMPL_H
#define NUTRIMATIC_FREQSORT_IMPL_H

#include <map>
#include <string>
#include <vector>

#include "letter-bag.h"

enum FreqsortMatch {
  FREQSORT_MATCH_ALL,
  FREQSORT_MATCH_PROPER,
  FREQSORT_MATCH_NON_PROPER,
};

enum FreqsortScoreMode {
  FREQSORT_SCORE_MULTIPLY,
  FREQSORT_SCORE_ADD,
  FREQSORT_SCORE_LOG,
  FREQSORT_SCORE_GLOG,
  FREQSORT_SCORE_CV,
};

// How freqsort ranks entries against letters less used_letters; see
// freqsort's usage().
struct FreqsortOptions {
  std::string letters;                                // LETTERS
  std::string used_letters;                           // IGNORE...
  bool by_length = false;                             // -i
  bool words_only = false;                            // -w
  FreqsortMatch match = FREQSORT_MATCH_ALL;           // -p, -n, -a
  FreqsortScoreMode mode = FREQSORT_SCORE_MULTIPLY;   // -v
  double value = 1.1;                                 // -v
  int min_letters = 4;                                // -m
  double min_cv = 0.0;                                // --min-cv
};

// Sets out's mode and value from a -v argument: log, glog, g, cv, NUMBER, or
// +NUMBER. Returns false, with the error diagnosed, when it can't be parsed.
bool freqsort_parse_value(char const* in, FreqsortOptions* out);

// The a-z letters of `in`, lowercased.
std::string freqsort_letters(char const* in);

// Entries ranked under FreqsortOptions.
class Freqsort {
 public:
  // Takes `options` and the letters left once its used_letters are removed.
  // Returns false, with the error diagnosed, when they aren't all available
  // or nothing is left.
  bool load(FreqsortOptions const& options);

  // Adds `line` when the options admit it and its letters are available. Only
  // valid after load() succeeds.
  void add(std::string line);

  bool empty() const { return entries_.empty(); }

  // The entries as freqsort prints them, lowest score first.
  std::vector<std::string> lines() const;

 private:
  struct Entry {
    std::string line;
    double score;
    int letters;
  };

  bool score(std::string const& word, double* out) const;

  FreqsortOptions options_;
  LetterCounts counts_ = {};
  int total_ = 0;
  std::map<std::string, Entry> entries_;
};

#endif
