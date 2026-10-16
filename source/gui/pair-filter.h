#ifndef NUTRIMATIC_GUI_PAIR_FILTER_H
#define NUTRIMATIC_GUI_PAIR_FILTER_H

#include <string>
#include <vector>

#include "app-state.h"
#include "input-source.h"
#include "pfilter-impl.h"

// A pfilter call on one input, taking its dictionary and sentence from the
// LetterToolsParams. Its option widgets are an "x:" field setting
// max_letters, and a "u:" field with a clear button setting used, which the
// Column adds to its used letters; clearing it counts as Enter. Its display
// option widgets are a "cv" toggle button, setting cv, then "m:" and "x:"
// fields, starting at 0.00 and 10.0 and greyed out unless cv is set, setting
// min_cv and max_cv. Its fields set their options when apply_fields() is
// called. While cv is set, items whose ratio lies outside
// [min_cv, max_cv] are hidden (see display_hides()).
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
  bool render_options(bool* entered);
  bool render_display_options(bool* entered);
  bool apply_fields();
  unsigned display_version() const { return display_version_; }
  bool display_hides(std::string const& value) const;
  std::string used_letters() const { return used; }

  bool yes = false;  // -y
  bool cv = false;   // --cv
  int max_letters = PFILTER_DEFAULT_MAX_LETTERS;  // -x
  int max_letters_field = PFILTER_DEFAULT_MAX_LETTERS;
  std::string used;
  char used_text[64] = {};
  double min_cv = 0;
  double max_cv = 10;
  char min_cv_text[32] = "0.00";
  char max_cv_text[32] = "10.0";
  unsigned display_version_ = 0;
};

#endif
