#ifndef NUTRIMATIC_DFS_SCORE_KEY_LAYOUT_H
#define NUTRIMATIC_DFS_SCORE_KEY_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

#include <array>

#include "dfs-class-list.h"

struct ScoreKeyLayout {
  std::array<uint64_t, DFS_SYMBOL_COUNT> multipliers{};
  uint64_t exact_mask = 0;
  uint64_t projected_state_count = 0;
  uint64_t effective_state_count = 0;
  uint64_t root_key = 0;
  size_t exact_letters = 0;
  size_t wild_letters = 0;
  size_t wild_span = 1;

  static bool choose(
      std::array<uint32_t, DFS_SYMBOL_COUNT> const& bag,
      size_t letter_count, size_t cache_budget, int exact_letters,
      ScoreKeyLayout* result);
};

#endif
