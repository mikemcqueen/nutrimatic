#ifndef NUTRIMATIC_DFS_SEARCH_DATA_H
#define NUTRIMATIC_DFS_SEARCH_DATA_H

#include <stdint.h>

#include <array>
#include <memory>
#include <vector>

#include "dfs-alloc.h"
#include "dfs-class-list.h"
#include "dfs-score-bounds.h"
#include "dfs-score.h"

// One phase-1 class as the phase-2 scan reads it: the length and requirement
// counts it tests first, packed into one register-wide record.
struct FitClassMetadata {
  uint32_t letters_offset;
  uint32_t packed_length_and_count;
};

struct alignas(16) FitClass {
  FitClassMetadata metadata;
  uint64_t support_mask;
};

// What a cached score bound says about completing a remainder. UNKNOWN means
// the bound has nothing to say, not that completion is impossible.
enum Reachability {
  REACHABILITY_UNKNOWN,
  REACHABILITY_NO,
  REACHABILITY_YES,
};

// The prepared query: everything phase 2 builds for one call, owned outright.
// A runner takes one by move and destroys it when the call ends, so no prepared
// table outlives the search using it and the facade declares none of them. Only
// the phase-1 class list is borrowed, because its owner outlives every phase-2
// call.
struct DfsSearchData {
  DfsClassList const* class_list = NULL;
  ScoreBounds score_bounds;
  DfsScoreModel score_model{1.0, 1, 0.0};
  std::vector<double> best_member_log_scores;
  double segment_boundary_log_score = 0.0;
  size_t letter_count = 0;
  size_t max_depth = 0;
  // Nonzero restricts results to exactly this many selected index entries.
  // max_depth already stops the walk from going deeper, so this additionally
  // suppresses shallower solutions and prunes paths that can no longer reach
  // the target. min_word_length is what bounds the segments a remainder still
  // affords; DfsClassList clamps it to at least 1.
  size_t exact_depth = 0;
  size_t min_word_length = 1;
  std::array<uint32_t, DFS_SYMBOL_COUNT> bag{};
  uint64_t bag_mask = 0;
  uint64_t score_key = 0;
  uint64_t exact_root_key = 0;
  size_t score_wild_letters = 0;
  // Fixed for the whole run, so the exact recurrence tests one local bool
  // instead of chasing the bound table's mode on every subtract/restore.
  bool score_bounds_active = false;
  std::unique_ptr<FitClass[], DfsAlignedFree> fit_classes;
  std::unique_ptr<uint64_t[], DfsAlignedFree> class_supports;
  std::unique_ptr<uint64_t[], DfsAlignedFree> score_key_deltas;
  std::unique_ptr<uint32_t[], DfsAlignedFree> packed_letters;
  bool support_scan_vector = false;
  bool certificate_ready = false;
  bool certificate_shadow = false;
  size_t certificate_stride = 0;
  std::vector<uint32_t> certificate_group_end;
  std::vector<double> certificate_max_score;
  std::vector<double> length_tail_bounds;
  size_t requested_search_threads = 1;
  bool progress_enabled = false;
  int64_t progress_interval = 0;

  size_t certificate_table_bytes() const {
    return certificate_max_score.size() * sizeof(double) +
        certificate_group_end.size() * sizeof(uint32_t) +
        length_tail_bounds.size() * sizeof(double);
  }

  // Candidates within one forced-symbol bucket descend by length, so the
  // first one short enough to fit is a binary search.
  size_t first_length_candidate(
      size_t begin, size_t end, size_t letters_left) const;
  bool certificate_rejects(
      size_t base, size_t length, size_t letters_left,
      double representative_log_score, double floor) const;
  // With bounds off this answers UNKNOWN for every key, so the exact
  // recurrence can skip maintaining and probing score keys entirely.
  Reachability cached_reachability(
      uint64_t score_key, bool original_root) const;
};

#endif
