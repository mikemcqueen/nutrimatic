#ifndef NUTRIMATIC_GUI_BAG_STATS_H
#define NUTRIMATIC_GUI_BAG_STATS_H

#include <vector>

#include "app-state.h"
#include "bag-stats-run.h"
#include "input-source.h"

// A bag-stats call on all threads, its input as PAIRS and its D: dictionary
// as DICT, taking its letters and used letters from the LetterToolsParams and
// keeping the classified no.pairs (--no-nopairs), counting a last p's pairs
// in the shared "big_dict" dictionary. It runs --ss with search, at most one
// pb step and one wb step, ending in p or pf, failing for pf after a step
// without exact, and prints, best first, without steps, each
// PAIRS row that fits the letters with its bag's pairs, with wb and no pb,
// each DICT word whose word bag leaves pairs with them, or SOLO when it uses
// all the letters, as --all lists them, and with pb, each PAIRS row of each
// pb pick that leaves pairs with its TOTAL_PAIRS, or SOLO with only WB_SOLO.
// Its option widgets are an "ex" toggle button setting exact (--exact), a
// "pb:" field setting min_pair_bag_length (--min-pb-len) and a "wb:" field
// setting min_word_bag_length (--min-wb-len). Its second row is an "ss:"
// field, starting at "wb,p", setting search. Its fields set their options
// when apply_fields() is called; an "ss:" that isn't one leaves search as it
// is. It starts exact. Choosing it sets its Column's source to the seed50m3
// seed and its D: dropdown to the column before it.
struct BagStats {
  static constexpr bool reads_pairs = true;
  static constexpr bool has_columns_in_dictionaries = true;
  static constexpr bool uses_dictionary = true;
  static constexpr bool uses_big_dictionary = true;
  static constexpr bool cancellable = true;
  static constexpr char const* default_seed = "seed50m3";

  bool run(std::vector<SharedLines> const& inputs,
           LetterToolsParams const& params, Lines* output) const;
  bool render_options(bool* entered);
  bool render_display_options(bool* entered);
  bool apply_fields();

  bool exact = true;
  Search search{.words = 1};
  char search_field[32] = "wb,p";
  int min_word_bag_length = DEFAULT_MIN_WB_LENGTH;
  int min_pair_bag_length = DEFAULT_MIN_PB_LENGTH;
  int min_word_bag_length_field = DEFAULT_MIN_WB_LENGTH;
  int min_pair_bag_length_field = DEFAULT_MIN_PB_LENGTH;
};

#endif
