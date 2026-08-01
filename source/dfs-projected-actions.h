#ifndef NUTRIMATIC_DFS_PROJECTED_ACTIONS_H
#define NUTRIMATIC_DFS_PROJECTED_ACTIONS_H

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <vector>

#include "dfs-class-list.h"

struct BoundStateView;
struct DfsSearchData;
struct ScoreKeyLayout;

struct alignas(16) ProjectedAction {
  uint64_t score_key_delta;
  double partial_score;
  double rounding_error_base;
  uint32_t repeated_offset;
  uint32_t packed_lengths;
  uint32_t repeated_count;
};

class ProjectedActions {
 public:
  static bool build(
      DfsSearchData const& data, ScoreKeyLayout const& layout,
      uint16_t const* wild_lengths, ProjectedActions* result);
  size_t bucket_begin(size_t bucket) const;
  size_t bucket_end(size_t bucket) const;
  ProjectedAction const& action(size_t index) const;
  uint64_t exact_support(size_t index) const;
  uint32_t const* repeated_begin(ProjectedAction const& action) const;
  bool fits(size_t index, BoundStateView state) const;
  size_t first_length_candidate(
      size_t begin, size_t end, size_t letters_left) const;
  size_t size() const;

 private:
  std::vector<ProjectedAction> actions_;
  std::vector<uint64_t> exact_supports_;
  std::vector<uint32_t> repeated_requirements_;
  std::array<size_t, DFS_SYMBOL_COUNT + 2> bucket_starts_{};
};

#endif
