#ifndef NUTRIMATIC_GUI_PAIR_FILTER_H
#define NUTRIMATIC_GUI_PAIR_FILTER_H

#include <string>
#include <vector>

#include "app-state.h"
#include "input-source.h"
#include "pfilter-impl.h"

// A pfilter call on one input, taking its dictionary from app_state() and its
// sentence from the GlobalSettings. Its option widget is a "u:" field with a
// clear button; entering letters there, or clearing it, sets used, which the Column adds to its used letters.
struct PairFilter {
  bool run(std::vector<SharedLines> const& inputs,
           GlobalSettings const& settings, Lines* output) const;
  bool render_options();
  std::string used_letters() const { return used; }

  bool yes = false;  // -y
  std::string used;
  char used_text[64] = {};
};

#endif
