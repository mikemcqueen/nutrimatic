#ifndef NUTRIMATIC_DFS_SCORE_H
#define NUTRIMATIC_DFS_SCORE_H

#include <stdint.h>

// The production phase-2 penalty for starting another corpus segment.
inline constexpr double DFS_DEFAULT_SEGMENT_PENALTY = 1e6;

// Shared log-space scoring for the dfs-anagrams family. A segment is one
// selected index entry; appending another entry pays one segment-boundary
// penalty. Word count within an entry does not affect its score: measured
// against the index, multi-word entries are not rarer than single-word entries
// of the same letter count, so there is nothing to compensate. See
// findings/association-is-not-interestingness.md. Preferring answers that use
// multi-word entries is a selection question, not a scoring one.
class DfsScoreModel {
 public:
  DfsScoreModel(double segment_penalty, int64_t corpus_total);

  double segment_log_score(int64_t count) const;
  double first_segment_log_score(int64_t count) const;
  double append_segment_log_score(
      double accumulated, int64_t count) const;
  double append_log_score(
      double accumulated, double segment_log_score) const;

  double segment_boundary_log_score() const {
    return segment_boundary_log_score_;
  }
  double displayed_score(double log_score) const;

 private:
  double segment_boundary_log_score_;
};

#endif
