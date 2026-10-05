#ifndef NUTRIMATIC_GUI_FIND_PAIRS_H
#define NUTRIMATIC_GUI_FIND_PAIRS_H

#include <vector>

#include "app-state.h"
#include "input-source.h"
#include "pairs-impl.h"

// A pairs call on one input, taking its letters and used letters from the
// LetterToolsParams in place of those in options. Its option widgets are an "m:"
// field setting min_word_length, an "ex" checkbox setting exact, and "mx:"
// fields setting min_words and max_words. It starts with a minimum length of
// 3, exact, and 1 to 2 words. It reads dictionaries as well as seeds and
// columns.
struct FindPairs {
  static constexpr bool reads_dictionaries = true;

  FindPairs();

  bool run(std::vector<SharedLines> const& inputs,
           LetterToolsParams const& params, Lines* output) const;
  bool render_options();

  PairsOptions options;
};

#endif
