#ifndef NUTRIMATIC_DFS_SCORE_H
#define NUTRIMATIC_DFS_SCORE_H

#include <stdint.h>

// The production phase-2 penalty for starting another corpus segment.
inline constexpr double DFS_DEFAULT_SEGMENT_PENALTY = 1e6;
// Keep phrase bonuses independent of the selected segment penalty. Sharing the
// magnitude is deliberate: at --word-bonus 1 a multi-word segment earns back
// exactly the default boundary penalty it costs, so extending an answer with a
// pair is free while extending it with a single word is not.
inline constexpr double DFS_WORD_BONUS_BASE = 1e6;

// Shared log-space scoring for the dfs-anagrams family. A segment is one
// selected index entry; spaces within an entry affect only its optional phrase
// bonus, while appending another entry pays one segment-boundary penalty.
//
// The multi-word bonus is a statement of what the tool is for, not a
// correction for anything measured. Multi-word entries are not rarer than
// single-word entries of the same letter count -- see
// findings/association-is-not-interestingness.md, which closes the question of
// whether any index statistic *derives* a preference for phrases. It does not,
// and none is wanted: pairs are the objective, so the preference is asserted
// here as a tunable term and calibrated by inspecting results.
class DfsScoreModel {
 public:
  DfsScoreModel(double segment_penalty, int64_t corpus_total,
                double word_bonus);

  double segment_log_score(int64_t count, bool multi_word) const;
  double first_segment_log_score(int64_t count, bool multi_word) const;
  double append_segment_log_score(
      double accumulated, int64_t count, bool multi_word) const;
  double append_log_score(
      double accumulated, double segment_log_score) const;

  double segment_boundary_log_score() const {
    return segment_boundary_log_score_;
  }
  double multi_word_log_bonus() const { return multi_word_log_bonus_; }
  double displayed_score(double log_score) const;

 private:
  double segment_boundary_log_score_;
  double multi_word_log_bonus_;
};

#endif
