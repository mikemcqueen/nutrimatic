#ifndef NUTRIMATIC_GUI_FIND_PAIRS_H
#define NUTRIMATIC_GUI_FIND_PAIRS_H

#include <vector>

#include "app-state.h"
#include "input-source.h"
#include "pairs-impl.h"

// A pairs call on one input, taking its letters and used letters from the
// GlobalSettings in place of those in options. Its option widgets are an "m:"
// field setting min_word_length, an "x" checkbox setting exact, and a "solo"
// checkbox, enabled only with exact, setting allow_solo. It starts with a
// minimum of 3, exact, and solo words allowed. It reads dictionaries as well
// as seeds and columns.
struct FindPairs {
  static constexpr bool reads_dictionaries = true;

  FindPairs();

  bool run(std::vector<SharedLines> const& inputs,
           GlobalSettings const& settings, Lines* output) const;
  bool render_options();

  PairsOptions options;
};

#endif
