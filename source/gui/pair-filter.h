#ifndef NUTRIMATIC_GUI_PAIR_FILTER_H
#define NUTRIMATIC_GUI_PAIR_FILTER_H

#include <vector>

#include "app-state.h"
#include "input-source.h"
#include "pfilter-impl.h"

// A pfilter call on one input, taking its dictionary from app_state() and its
// sentence from the GlobalSettings.
struct PairFilter {
  bool yes = false;  // -y

  bool run(std::vector<SharedLines> const& inputs,
           GlobalSettings const& settings, Lines* output) const;
};

#endif
