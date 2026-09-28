#ifndef NUTRIMATIC_GUI_FREQ_SORT_H
#define NUTRIMATIC_GUI_FREQ_SORT_H

#include <vector>

#include "app-state.h"
#include "freqsort-impl.h"
#include "input-source.h"

// A freqsort call on one input, taking its letters and used letters from the
// GlobalSettings in place of those in options.
struct FreqSort {
  FreqsortOptions options;

  bool run(std::vector<SharedLines> const& inputs,
           GlobalSettings const& settings, Lines* output) const;
};

#endif
