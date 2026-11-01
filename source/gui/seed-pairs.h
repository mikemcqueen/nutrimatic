#ifndef NUTRIMATIC_GUI_SEED_PAIRS_H
#define NUTRIMATIC_GUI_SEED_PAIRS_H

#include <vector>

#include "app-state.h"
#include "input-source.h"
#include "review-kind.h"
#include "seed-view.h"

// A seed-pairs call on one input, a column before its own, looking pairs up
// in the LetterToolsParams' p1 index and keeping those whose words are both
// in its dictionary. Its option widget is a "pm:" field setting min_yes. Its
// second row has "m" and "x" fields setting min_len and max_len. Its fields
// set their options when apply_fields() is called. Each kept pair is written
// in the order with the larger phase 1 YES, followed by a space and that YES,
// in input order. Its Column's shown pairs can be reviewed, and saved.
struct SeedPairsFilter {
  static constexpr bool reads_pairs = true;
  static constexpr bool reads_columns_only = true;
  static constexpr bool uses_dictionary = true;
  static constexpr bool uses_p1 = true;
  static constexpr bool saveable = true;

  ReviewKind review_kind(InputSource const&) const { return ReviewKind::pairs; }

  bool run(std::vector<SharedLines> const& inputs,
           LetterToolsParams const& params, Lines* output) const;
  bool render_options(bool* entered);
  bool render_display_options(bool* entered);
  bool apply_fields();

  float min_yes = .85f;  // --pm
  char min_yes_text[32] = "0.85";
  int min_len = SeedFilter{}.min_len;  // -m
  int min_len_field = SeedFilter{}.min_len;
  int max_len = SeedFilter{}.max_len;  // -x
  int max_len_field = SeedFilter{}.max_len;
};

#endif
