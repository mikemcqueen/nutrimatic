#include "lc-impl.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>

#include <algorithm>
#include <array>

#include "letter-bag.h"

namespace {

double const CV_AVERAGE = 1.59;
double const CV_HIGH = 1.8;
double const CV_LOW = 1.4;

int const COLUMNS = 5;
char const* const HEADERS[COLUMNS] = {
  "char", "cnt", "expected", "actual", "factor",
};
int const WIDTHS[COLUMNS] = { 4, 4, 8, 8, 9 };
int const FREQUENCY_FIRST_COLUMN = 2;

__attribute__((format(printf, 1, 2)))
std::string format(char const* format, ...) {
  va_list args;
  va_start(args, format);
  va_list copy;
  va_copy(copy, args);
  int const size = vsnprintf(NULL, 0, format, copy);
  va_end(copy);
  std::string out(size_t(size) + 1, '\0');
  vsnprintf(out.data(), out.size(), format, args);
  va_end(args);
  out.pop_back();
  return out;
}

std::string row(std::array<std::string, COLUMNS> const& fields) {
  std::string out;
  for (int i = 0; i < COLUMNS; ++i) {
    if (i > 0) out += ' ';
    out += fields[i];
    if (i + 1 < COLUMNS && int(fields[i].size()) < WIDTHS[i])
      out.append(size_t(WIDTHS[i]) - fields[i].size(), ' ');
  }
  return out;
}

std::string repeat(char const* text, int count) {
  std::string out;
  for (int i = 0; i < count; ++i) out += text;
  return out;
}

std::string group_header(char const* label) {
  int indent = 0;
  for (int i = 0; i < FREQUENCY_FIRST_COLUMN; ++i) indent += WIDTHS[i] + 1;
  int span = int(std::string(HEADERS[COLUMNS - 1]).size());
  for (int i = FREQUENCY_FIRST_COLUMN; i < COLUMNS - 1; ++i)
    span += WIDTHS[i] + 1;
  std::string const text = format(" %s ", label);
  int const dashes = std::max(0, span - int(text.size()) - 2);
  int const left = dashes / 2;
  return std::string(size_t(indent), ' ') + "┌" + repeat("─", left) + text +
      repeat("─", dashes - left) + "┐";
}

std::string percent(double value, bool below) {
  return below ? format("(%.2f)", value) : format("%.2f", value);
}

std::string cv_line(double cv) {
  std::string actual;
  if (isinf(cv)) {
    actual = "inf +++";
  } else {
    actual = percent(cv, cv < CV_AVERAGE);
    if (cv > CV_HIGH)
      actual += " +++";
    else if (cv < CV_LOW)
      actual += " ---";
  }
  return format("C/V ratio: %.2f %s", CV_AVERAGE, actual.c_str());
}

void table(LetterCounts const& counts, int total,
           std::vector<std::string>* out) {
  struct Letter {
    char ch;
    int count;
    double actual;
    double factor;
  };
  std::vector<Letter> letters;
  for (int i = 0; i < 26; ++i) {
    if (counts[i] == 0) continue;
    double const actual = 100.0 * counts[i] / total;
    letters.push_back(
        { char('a' + i), counts[i], actual, actual / ENGLISH_PERCENT[i] });
  }
  std::stable_sort(letters.begin(), letters.end(),
      [](Letter const& a, Letter const& b) { return a.factor < b.factor; });

  out->push_back(group_header("frequency"));
  out->push_back(row({ HEADERS[0], HEADERS[1], HEADERS[2], HEADERS[3],
                       HEADERS[4] }));
  for (Letter const& letter : letters) {
    std::string factor = format("%.2f", letter.factor);
    if (letter.factor > 2)
      factor += " +++";
    else if (letter.factor < 0.5)
      factor += " ---";
    out->push_back(row({
        format("'%c'", letter.ch),
        format("%d", letter.count),
        format("%.2f", ENGLISH_PERCENT[letter.ch - 'a']),
        percent(letter.actual, letter.factor < 1),
        factor,
    }));
  }
}

}  // namespace

bool letter_count_lines(
    LetterCountOptions const& options, std::vector<std::string>* out) {
  out->clear();
  std::string const text = lowercase_letters(options.text);
  std::string const used = lowercase_letters(options.used_letters);
  std::string remaining;
  if (!subtract_letters(text, used, &remaining)) return false;

  LetterCounts counts = {};
  for (char ch : remaining) ++counts[ch - 'a'];
  double const cv = cv_ratio(counts);

  if (options.mode == LETTER_COUNT_REMAINING) {
    out->push_back(remaining);
    return true;
  }
  if (options.mode == LETTER_COUNT_CV) {
    out->push_back(isinf(cv) ? "inf" : format("%.2f", cv));
    return true;
  }

  out->push_back(format("total: %zu", text.size()));
  if (!used.empty()) out->push_back("ignoring: " + used);
  out->push_back(
      format("remain: %zu, %s", remaining.size(), remaining.c_str()));
  std::string removed;
  for (char ch = 'a'; ch <= 'z'; ++ch)
    if (counts[ch - 'a'] == 0 && text.find(ch) != std::string::npos)
      removed += ch;
  if (!removed.empty())
    out->push_back(format("removed: %zu, %s", removed.size(), removed.c_str()));
  out->push_back(cv_line(cv));
  table(counts, int(remaining.size()), out);
  return true;
}
