#ifndef NUTRIMATIC_GUI_PAIR_FILTER_H
#define NUTRIMATIC_GUI_PAIR_FILTER_H

#include <string>
#include <vector>

#include "app-state.h"
#include "input-source.h"
#include "pfilter-impl.h"

// A pfilter call on one input, taking its dictionary and sentence from the
// LetterToolsParams. Its option widgets are a "cv" toggle
// button, setting cv, an "x:" field setting max_letters, and a "u:" field
// with a clear button; entering letters there, or clearing it, sets used, which the Column adds to its used letters.
// With cv, which requires letters, each kept line is followed by a space and
// the consonant/vowel ratio of the letters its pair leaves (see
// remaining_cv_ratio()), lowest ratio first, as pfilter --cv prints them.
// Without cv, the kept lines are sorted. Its Column's shown pairs can be
// reviewed.
struct PairFilter {
  static constexpr bool reads_pairs = true;
  static constexpr bool reads_classified = true;
  static constexpr bool reviewable = true;

  bool run(std::vector<SharedLines> const& inputs,
           LetterToolsParams const& params, Lines* output) const;
  bool render_options();
  std::string used_letters() const { return used; }

  bool yes = false;  // -y
  bool cv = false;   // --cv
  int max_letters = PFILTER_DEFAULT_MAX_LETTERS;  // -x
  std::string used;
  char used_text[64] = {};
};

#endif
