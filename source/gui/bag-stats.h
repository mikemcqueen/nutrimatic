#ifndef NUTRIMATIC_GUI_BAG_STATS_H
#define NUTRIMATIC_GUI_BAG_STATS_H

#include <vector>

#include "app-state.h"
#include "bag-stats-run.h"
#include "input-source.h"

// A bag-stats call on all threads, its input as PAIRS and its wb: words as
// DICT, taking its letters and used letters from the LetterToolsParams and
// keeping the classified no.pairs (--no-nopairs), counting a last p's pairs
// in the shared "big_dict" dictionary. It runs --ss with search, at most one
// pb step and one wb step, ending in p or pf, failing for pf after a step
// without exact, and prints, best first, without steps, each
// PAIRS row that fits the letters with its bag's pairs, with wb and no pb,
// each DICT word whose word bag leaves pairs with them, or SOLO when it uses
// all the letters, as --all lists them, and with pb, each PAIRS row of each
// pb pick that leaves pairs with its TOTAL_PAIRS, or SOLO with only WB_SOLO.
// Its option widgets are an "ex" toggle button setting exact (--exact), a "pb:"
// field setting min_pair_bag_length (--min-pb-len) and a "wb:" field setting
// min_word_bag_length (--min-wb-len). Its second row is an "ss:" field,
// starting at "wb,p", setting search. An ss: of one or two pb steps alone runs
// as bag-stats --unique (without --leftovers), printing each PAIRS row the pb
// picks use with how many combinations it's in, most first, the first
// unique_top of them (-n); more than two is diagnosed. Its fields set their
// options when apply_fields() is called; an "ss:" that isn't one leaves search
// as it is. It starts exact. Choosing it sets its Column's source to the
// seed50m3 seed and its wb: dropdown to the column before it.
struct BagStats {
  static constexpr bool reads_pairs = true;
  static constexpr bool has_word_bag_source = true;
  static constexpr bool uses_big_dictionary = true;
  static constexpr bool cancellable = true;
  static constexpr char const* default_seed = "seed50m3";
  static constexpr int unique_top = 100000;

  bool run(std::vector<SharedLines> const& inputs,
           LetterToolsParams const& params, Lines* output) const;
  bool render_options(bool* entered);
  bool render_display_options(bool* entered);
  bool apply_fields();
  // Sets search from search_field, as picks when it's pb steps alone; true
  // when that changes it. One that isn't one is diagnosed and leaves search as
  // it is.
  bool apply_search();

  bool exact = true;
  Search search{.words = 1};
  char search_field[32] = "wb,p";
  int min_word_bag_length = DEFAULT_MIN_WB_LENGTH;
  int min_pair_bag_length = DEFAULT_MIN_PB_LENGTH;
  int min_word_bag_length_field = DEFAULT_MIN_WB_LENGTH;
  int min_pair_bag_length_field = DEFAULT_MIN_PB_LENGTH;
};

#endif
