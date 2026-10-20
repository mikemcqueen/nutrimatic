#ifndef NUTRIMATIC_GUI_FREQ_SORT_H
#define NUTRIMATIC_GUI_FREQ_SORT_H

#include <vector>

#include "app-state.h"
#include "freqsort-impl.h"
#include "input-source.h"

// A freqsort call on one input, taking its letters and used letters from the
// LetterToolsParams in place of those in options. Its option widget is an
// "m:" field setting options' min_letters. Its second row has a "cv" toggle
// button, starting on, setting options' mode to cv (-v cv) or back to
// freqsort's default, then "m" and "x" fields, starting at 1.00 and 0.00
// and greyed out unless the mode is cv, setting options' min_cv and max_cv,
// a max of 0 meaning none, then an "X" button, greyed out like them,
// resetting the fields to those defaults (see cv_range_input()). Its fields
// set their options when apply_fields() is called. Its source can be its D:
// dictionary as well as a seed or a column.
struct FreqSort {
  static constexpr bool has_dictionary_in_src = true;

  FreqSort();

  bool run(std::vector<SharedLines> const& inputs,
           LetterToolsParams const& params, Lines* output) const;
  bool render_options(bool* entered);
  bool render_display_options(bool* entered);
  bool apply_fields();

  FreqsortOptions options;
  char min_cv[32];
  char max_cv[32];
  int min_letters;
};

#endif
