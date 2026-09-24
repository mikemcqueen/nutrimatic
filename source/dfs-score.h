#ifndef NUTRIMATIC_DFS_SCORE_H
#define NUTRIMATIC_DFS_SCORE_H

#include "tail-map.h"

#include <stddef.h>
#include <stdint.h>

#include <vector>

// The production phase-2 penalty for starting another corpus segment.
inline constexpr double DFS_DEFAULT_SEGMENT_PENALTY = 1e6;
// Keep phrase bonuses independent of the selected segment penalty. Sharing the
// magnitude is deliberate: at --word-bonus 1 a multi-word segment earns back
// exactly the default boundary penalty it costs, so extending an answer with a
// multi-word entry is free while extending it with a single word is not.
inline constexpr double DFS_WORD_BONUS_BASE = 1e6;
inline constexpr double DFS_PAIR_BONUS_BASE = 1e6;
inline constexpr double DFS_DEFAULT_PAIR_BONUS = 1.0;
inline constexpr double DFS_DEFAULT_WORD_BONUS = 0.0;
inline constexpr double DFS_SEED_PAIR_BONUS = 1.0;
inline constexpr double DFS_YES_PAIR_BONUS = 1.05;
inline constexpr double DFS_BEST_PAIR_BONUS = 4.0;

enum DfsPairBonusKind : uint8_t;

enum DfsBestBonusMode : uint8_t {
  DFS_BEST_BONUS_FIXED,
  DFS_BEST_BONUS_DESCENDING,
};

// The fixed policy preserves the score used when a result's final segment
// count is unknown. The descending policy is result-level: local member scores
// use one safe upper exponent, while completed results use B(N, k).
class DfsBestBonusPolicy {
 public:
  static DfsBestBonusPolicy fixed(double exponent);
  static DfsBestBonusPolicy descending(
      size_t exact_segments, double scale = 1.0);

  bool is_descending() const {
    return mode_ == DFS_BEST_BONUS_DESCENDING;
  }
  size_t exact_segments() const { return exact_segments_; }
  double local_upper_exponent(double non_best_upper) const;
  long double cumulative_exponent(size_t best_segments) const;

 private:
  DfsBestBonusPolicy(
      DfsBestBonusMode mode, double exponent, size_t exact_segments);

  DfsBestBonusMode mode_;
  // The fixed exponent, or the scale on the descending schedule.
  double exponent_;
  size_t exact_segments_;
};

// A monotone recalibration of the base count term onto the scale the batch of
// available segments would carry if its upper tail were normal. It replaces
// log(count) with the log count carrying the same mapped deviation, which
// leaves the boundary penalty and every bonus commensurate with it. See
// findings/ptm-base-score.md.
class DfsBaseRemap {
 public:
  // `log_counts` holds log(count) for every member of the batch, in any
  // order. Returns false when the batch cannot be fitted.
  bool fit(std::vector<double> log_counts);

  bool valid() const { return map_.valid(); }
  double rate() const { return map_.rate(); }
  double mean() const { return mean_; }
  double deviation() const { return deviation_; }
  size_t size() const { return map_.size(); }
  double log_score(double log_count) const;

 private:
  TailMap map_;
  double mean_ = 0.0;
  double deviation_ = 0.0;
};

// Shared log-space scoring for the dfs-anagrams family. A segment is one
// selected index entry; spaces within an entry control its optional phrase
// bonus, pair-source membership controls a second optional bonus, and
// appending another entry pays one segment-boundary penalty.
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
                double word_bonus, double pair_bonus = 0.0,
                DfsBestBonusPolicy best_bonus =
                    DfsBestBonusPolicy::fixed(DFS_BEST_PAIR_BONUS),
                double seed_bonus = DFS_SEED_PAIR_BONUS,
                double yes_bonus = DFS_YES_PAIR_BONUS);

  double segment_log_score(
      int64_t count, bool multi_word, bool known_pair = false) const;
  // The base term a count contributes before any bonus: log(count), or its
  // recalibration when a remap is installed.
  double base_log_score(int64_t count) const;
  // Borrowed, and installed only before any member ordering that reads it.
  void set_base_remap(DfsBaseRemap const* remap) { base_remap_ = remap; }
  DfsBaseRemap const* base_remap() const { return base_remap_; }
  double first_segment_log_score(
      int64_t count, bool multi_word, bool known_pair = false) const;
  double append_segment_log_score(
      double accumulated, int64_t count, bool multi_word,
      bool known_pair = false) const;
  double append_log_score(
      double accumulated, double segment_log_score) const;
  double solo_local_upper_log_bonus(uint16_t score_flags) const;
  double member_upper_log_score(
      int64_t count, bool multi_word, uint16_t score_flags) const;

  double segment_boundary_log_score() const {
    return segment_boundary_log_score_;
  }
  double multi_word_log_bonus() const { return multi_word_log_bonus_; }
  double pair_log_bonus() const { return pair_log_bonus_; }
  double pair_log_bonus(DfsPairBonusKind kind) const;
  double member_pair_log_bonus(uint16_t score_flags) const;
  bool descending_best_bonus() const {
    return best_bonus_policy_.is_descending();
  }
  size_t exact_segments() const {
    return best_bonus_policy_.exact_segments();
  }
  long double local_best_upper_log_bonus() const;
  long double exact_best_log_bonus(size_t best_segments) const;
  double displayed_score(double log_score) const;

 private:
  double segment_boundary_log_score_;
  double multi_word_log_bonus_;
  double pair_log_bonus_;
  double seed_pair_log_bonus_;
  double yes_pair_log_bonus_;
  double best_pair_exponent_;
  double best_pair_log_bonus_;
  DfsBestBonusPolicy best_bonus_policy_;
  DfsBaseRemap const* base_remap_ = NULL;
};

#endif
