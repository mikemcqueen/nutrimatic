#include "dfs-score.h"

#include "dfs-solo-words.h"

#include <assert.h>
#include <math.h>

#include <algorithm>
#include <functional>

namespace {

double make_segment_boundary_log_score(
    double segment_penalty, int64_t corpus_total) {
  assert(isfinite(segment_penalty));
  assert(segment_penalty >= 1.0);
  assert(corpus_total > 0);
  return -log(segment_penalty) - log(double(corpus_total));
}

double make_multi_word_log_bonus(double word_bonus) {
  assert(isfinite(word_bonus));
  return word_bonus * log(DFS_WORD_BONUS_BASE);
}

double make_pair_log_bonus(double pair_bonus) {
  assert(isfinite(pair_bonus));
  return pair_bonus * log(DFS_PAIR_BONUS_BASE);
}

}  // namespace

DfsBestBonusPolicy::DfsBestBonusPolicy(
    DfsBestBonusMode mode, double fixed_exponent, size_t exact_segments):
    mode_(mode),
    fixed_exponent_(fixed_exponent),
    exact_segments_(exact_segments) { }

DfsBestBonusPolicy DfsBestBonusPolicy::fixed(double exponent) {
  assert(isfinite(exponent));
  return DfsBestBonusPolicy(DFS_BEST_BONUS_FIXED, exponent, 0);
}

DfsBestBonusPolicy DfsBestBonusPolicy::descending(size_t exact_segments) {
  assert(exact_segments > 0);
  return DfsBestBonusPolicy(
      DFS_BEST_BONUS_DESCENDING, 0.0, exact_segments);
}

double DfsBestBonusPolicy::local_upper_exponent(
    double non_best_upper) const {
  assert(isfinite(non_best_upper));
  if (!is_descending()) return fixed_exponent_;
  assert(exact_segments_ > 0);
  return std::max(double(exact_segments_), non_best_upper);
}

long double DfsBestBonusPolicy::cumulative_exponent(
    size_t best_segments) const {
  if (!is_descending())
    return static_cast<long double>(best_segments) * fixed_exponent_;
  assert(exact_segments_ > 0);
  assert(best_segments <= exact_segments_);
  long double const n = static_cast<long double>(exact_segments_);
  long double const k = static_cast<long double>(best_segments);
  long double const exponent = k * n - k * (k - 1.0L) / 2.0L;
  assert(isfinite(exponent));
  return exponent;
}

DfsScoreModel::DfsScoreModel(
    double segment_penalty, int64_t corpus_total, double word_bonus,
    double pair_bonus, DfsBestBonusPolicy best_bonus):
    segment_boundary_log_score_(
        make_segment_boundary_log_score(segment_penalty, corpus_total)),
    multi_word_log_bonus_(make_multi_word_log_bonus(word_bonus)),
    pair_log_bonus_(make_pair_log_bonus(pair_bonus)),
    seed_pair_log_bonus_(make_pair_log_bonus(DFS_SEED_PAIR_BONUS)),
    yes_pair_log_bonus_(make_pair_log_bonus(DFS_YES_PAIR_BONUS)),
    best_pair_exponent_(best_bonus.local_upper_exponent(
        std::max(std::max(pair_bonus, DFS_SEED_PAIR_BONUS),
                 DFS_YES_PAIR_BONUS))),
    best_pair_log_bonus_(make_pair_log_bonus(best_pair_exponent_)),
    best_bonus_policy_(best_bonus) { }

bool DfsBaseRemap::fit(std::vector<double> log_counts) {
  if (log_counts.empty()) return false;

  double total = 0.0;
  for (size_t i = 0; i < log_counts.size(); ++i) total += log_counts[i];
  mean_ = total / double(log_counts.size());
  double squares = 0.0;
  for (size_t i = 0; i < log_counts.size(); ++i) {
    double const difference = log_counts[i] - mean_;
    squares += difference * difference;
  }
  deviation_ = sqrt(squares / double(log_counts.size()));

  std::sort(log_counts.begin(), log_counts.end(), std::greater<double>());
  return map_.fit(log_counts);
}

double DfsBaseRemap::log_score(double log_count) const {
  if (!map_.valid() || deviation_ == 0.0) return log_count;
  return mean_ + map_.deviation(log_count) * deviation_;
}

double DfsScoreModel::base_log_score(int64_t count) const {
  assert(count > 0);
  double const log_count = log(double(count));
  return base_remap_ == NULL ? log_count : base_remap_->log_score(log_count);
}

double DfsScoreModel::segment_log_score(
    int64_t count, bool multi_word, bool known_pair) const {
  assert(count > 0);
  return base_log_score(count) +
      (multi_word ? multi_word_log_bonus_ : 0.0) +
      (known_pair ? pair_log_bonus_ : 0.0);
}

double DfsScoreModel::pair_log_bonus(DfsPairBonusKind kind) const {
  switch (kind) {
    case DFS_PAIR_BONUS_NONE:
      return 0.0;
    case DFS_PAIR_BONUS_LEGACY:
      return pair_log_bonus_;
    case DFS_PAIR_BONUS_SEED:
      return seed_pair_log_bonus_;
    case DFS_PAIR_BONUS_YES:
      return yes_pair_log_bonus_;
    case DFS_PAIR_BONUS_BEST:
      return best_pair_log_bonus_;
  }
  assert(false);
  return 0.0;
}

double DfsScoreModel::member_pair_log_bonus(uint16_t score_flags) const {
  return pair_log_bonus(dfs_member_pair_bonus_kind(score_flags));
}

long double DfsScoreModel::exact_best_log_bonus(
    size_t best_segments) const {
  return best_bonus_policy_.cumulative_exponent(best_segments) *
      logl(static_cast<long double>(DFS_PAIR_BONUS_BASE));
}

long double DfsScoreModel::local_best_upper_log_bonus() const {
  return std::max(
      static_cast<long double>(best_pair_log_bonus_),
      static_cast<long double>(best_pair_exponent_) *
          logl(static_cast<long double>(DFS_PAIR_BONUS_BASE)));
}

double DfsScoreModel::first_segment_log_score(
    int64_t count, bool multi_word, bool known_pair) const {
  return segment_log_score(count, multi_word, known_pair);
}

double DfsScoreModel::append_segment_log_score(
    double accumulated, int64_t count, bool multi_word,
    bool known_pair) const {
  return append_log_score(
      accumulated, segment_log_score(count, multi_word, known_pair));
}

double DfsScoreModel::append_log_score(
    double accumulated, double segment_log_score) const {
  return accumulated + segment_boundary_log_score_ + segment_log_score;
}

double DfsScoreModel::solo_local_upper_log_bonus(
    uint16_t score_flags) const {
  if ((score_flags & DFS_MEMBER_SOLO_PAIR_EDGE) != 0)
    return multi_word_log_bonus_ +
        pair_log_bonus(dfs_member_solo_pair_bonus_kind(score_flags));
  if ((score_flags & DFS_MEMBER_SOLO_WORD_EDGE) != 0)
    return multi_word_log_bonus_;
  return 0.0;
}

double DfsScoreModel::member_upper_log_score(
    int64_t count, bool multi_word, uint16_t score_flags) const {
  return segment_log_score(
      count, multi_word,
      /*known_pair=*/false) + member_pair_log_bonus(score_flags) +
      solo_local_upper_log_bonus(score_flags);
}

double DfsScoreModel::displayed_score(double log_score) const {
  return exp(log_score);
}
