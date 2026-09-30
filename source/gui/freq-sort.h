#ifndef NUTRIMATIC_GUI_FREQ_SORT_H
#define NUTRIMATIC_GUI_FREQ_SORT_H

#include <vector>

#include "app-state.h"
#include "freqsort-impl.h"
#include "input-source.h"

// A freqsort call on one input, taking its letters and used letters from the
// GlobalSettings in place of those in options. Its option widget is a "cv"
// toggle button, starting on, setting options' mode to cv (-v cv) or back to
// freqsort's default. Beside it a "min:" field, starting at 1.0 and greyed
// out unless the mode is cv, sets options' min_cv, and an "m:" field sets its
// min_letters. It reads dictionaries as well as seeds and columns.
struct FreqSort {
  static constexpr bool reads_dictionaries = true;

  FreqSort();

  bool run(std::vector<SharedLines> const& inputs,
           GlobalSettings const& settings, Lines* output) const;
  bool render_options();

  FreqsortOptions options;
  char min_cv[32];
};

#endif
