#ifndef NUTRIMATIC_GUI_BAG_STATS_H
#define NUTRIMATIC_GUI_BAG_STATS_H

#include <vector>

#include "app-state.h"
#include "bag-stats-run.h"
#include "input-source.h"

// A bag-stats --ss pf call on all threads, its input as PAIRS and its D:
// dictionary as DICT, taking its letters and used letters from the
// LetterToolsParams and keeping the classified no.pairs (--no-nopairs). It
// prints, for each DICT word whose word bag the pair bags fit, best first as
// --completions lists them, the word and its pairs, or the word and SOLO when
// it uses all the letters. Its option widgets are an "ex" toggle button
// setting exact (--exact), a "pb" toggle button, enabled only with exact and
// word_step, setting pair_step (--ss pb,wb,pf, which lists each PAIRS row in
// place of each DICT word), then a ":" field, enabled with pair_step, setting
// min_pair_bag_length (--min-pb-len), a "wb" toggle button, enabled only with
// exact, setting word_step (--ss wb,pf instead of --ss pf), then a ":" field
// setting min_word_bag_length (--min-wb-len); the fields set their lengths
// when apply_fields() is called. It starts exact. Choosing it sets its Column's source to the seed50m3 seed
// and its D: dropdown to the column before it.
struct BagStats {
  static constexpr bool reads_pairs = true;
  static constexpr bool has_columns_in_dictionaries = true;
  static constexpr bool uses_dictionary = true;
  static constexpr bool cancellable = true;
  static constexpr char const* default_seed = "seed50m3";

  bool run(std::vector<SharedLines> const& inputs,
           LetterToolsParams const& params, Lines* output) const;
  bool render_options(bool* entered);
  bool apply_fields();

  bool exact = true;
  bool word_step = false;
  bool pair_step = false;
  int min_word_bag_length = DEFAULT_MIN_WB_LENGTH;
  int min_pair_bag_length = DEFAULT_MIN_PB_LENGTH;
  int min_word_bag_length_field = DEFAULT_MIN_WB_LENGTH;
  int min_pair_bag_length_field = DEFAULT_MIN_PB_LENGTH;
};

#endif
