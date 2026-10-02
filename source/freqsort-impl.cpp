#include "freqsort-impl.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <iterator>

#include "dfs-cli-args.h"
#include "letter-bag.h"

namespace {

double g_statistic(LetterCounts const& counts, int total) {
  double g = 0;
  for (int i = 0; i < 26; ++i) {
    int const observed = counts[i];
    if (observed == 0) continue;
    double const expected = total * ENGLISH_PERCENT[i] / 100;
    g += observed * log(observed / expected);
  }
  return 2 * g;
}

int count_letters(std::string const& word) {
  int count = 0;
  for (char c : word)
    if (c >= 'a' && c <= 'z') ++count;
  return count;
}

}  // namespace

bool freqsort_parse_value(char const* in, FreqsortOptions* out) {
  out->mode = FREQSORT_SCORE_MULTIPLY;
  if (strcmp(in, "log") == 0) {
    out->mode = FREQSORT_SCORE_LOG;
    return true;
  }
  if (strcmp(in, "glog") == 0 || strcmp(in, "g") == 0) {
    out->mode = FREQSORT_SCORE_GLOG;
    return true;
  }
  if (strcmp(in, "cv") == 0) {
    out->mode = FREQSORT_SCORE_CV;
    return true;
  }
  char const* number = in;
  if (*number == '+') {
    out->mode = FREQSORT_SCORE_ADD;
    ++number;
  }
  if (!parse_double(number, "-v value", &out->value)) return false;
  if (out->mode == FREQSORT_SCORE_MULTIPLY && out->value == 0) out->value = 1;
  return true;
}

bool Freqsort::load(FreqsortOptions const& options) {
  options_ = options;
  counts_.fill(0);
  total_ = 0;
  entries_.clear();

  std::string remaining;
  if (!subtract_letters(lowercase_letters(options.letters),
          lowercase_letters(options.used_letters), &remaining))
    return false;
  for (char ch : remaining) ++counts_[ch - 'a'];
  total_ = int(remaining.size());
  return true;
}

bool Freqsort::score(std::string const& word, double* out) const {
  LetterCounts left = counts_;
  double sum = 0;
  double mean = 1;
  double log_sum = 0;
  int count = 0;
  for (char c : word) {
    if ((unsigned char) c >= 0x80) return false;
    if (c < 'a' || c > 'z') continue;
    int& available = left[c - 'a'];
    if (available == 0) return false;
    double const factor = (100.0 * available / total_) /
        ENGLISH_PERCENT[c - 'a'];
    --available;

    if (options_.mode == FREQSORT_SCORE_LOG) {
      log_sum += log(factor);
    } else if (options_.mode == FREQSORT_SCORE_ADD) {
      sum += factor * mean + options_.value;
    } else {
      sum += factor;
    }
    ++count;
    if (options_.mode == FREQSORT_SCORE_ADD) mean = sum / count;
  }

  if (options_.mode == FREQSORT_SCORE_LOG) {
    *out = log_sum;
  } else if (options_.mode == FREQSORT_SCORE_CV) {
    *out = cv_ratio(left);
  } else if (options_.mode == FREQSORT_SCORE_GLOG) {
    *out = g_statistic(counts_, total_) - g_statistic(left, total_ - count);
  } else {
    if (options_.mode == FREQSORT_SCORE_MULTIPLY)
      sum *= pow(options_.value, count);
    *out = count == 0 ? sum : sum / count;
  }
  return true;
}

void Freqsort::add(std::string line) {
  if (line.empty()) return;
  bool const proper = isupper((unsigned char) line[0]);
  if ((options_.match == FREQSORT_MATCH_PROPER && !proper) ||
      (options_.match == FREQSORT_MATCH_NON_PROPER && proper))
    return;

  for (char& c : line) c = char(tolower((unsigned char) c));
  int const letters = count_letters(line);
  if (letters < options_.min_letters) return;

  double value;
  if (!score(line, &value)) return;
  if (options_.mode == FREQSORT_SCORE_CV && value < options_.min_cv) return;
  char rounded[32];
  snprintf(rounded, sizeof rounded, "%.4e", value);
  entries_[line] = { line, strtod(rounded, NULL), letters };
}

std::vector<std::string> Freqsort::lines() const {
  std::vector<Entry> sorted;
  sorted.reserve(entries_.size());
  size_t width = 0;
  for (auto const& [line, entry] : entries_) {
    sorted.push_back(entry);
    width = std::max(width, line.size());
  }
  std::stable_sort(sorted.begin(), sorted.end(),
      [this](Entry const& a, Entry const& b) {
        if (options_.by_length && a.letters != b.letters)
          return a.letters < b.letters;
        return a.score < b.score;
      });

  std::vector<std::string> out;
  out.reserve(sorted.size());
  for (Entry const& entry : sorted) {
    if (options_.words_only) {
      out.push_back(entry.line);
      continue;
    }
    char const* const format =
        options_.mode == FREQSORT_SCORE_CV ? "%-*s  %.2f" : "%-*s  %#.5g";
    int const size = snprintf(NULL, 0, format, int(width),
        entry.line.c_str(), entry.score);
    std::string text(size_t(size) + 1, '\0');
    snprintf(text.data(), text.size(), format, int(width),
        entry.line.c_str(), entry.score);
    text.pop_back();
    out.push_back(std::move(text));
  }
  return out;
}
