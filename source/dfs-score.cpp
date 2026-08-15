#include "dfs-score.h"

#include "dfs-solo-words.h"

#include <assert.h>
#include <math.h>

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

DfsScoreModel::DfsScoreModel(
    double segment_penalty, int64_t corpus_total, double word_bonus,
    double pair_bonus):
    segment_boundary_log_score_(
        make_segment_boundary_log_score(segment_penalty, corpus_total)),
    multi_word_log_bonus_(make_multi_word_log_bonus(word_bonus)),
    pair_log_bonus_(make_pair_log_bonus(pair_bonus)) { }

double DfsScoreModel::segment_log_score(
    int64_t count, bool multi_word, bool known_pair) const {
  assert(count > 0);
  return log(double(count)) +
      (multi_word ? multi_word_log_bonus_ : 0.0) +
      (known_pair ? pair_log_bonus_ : 0.0);
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
    return multi_word_log_bonus_ + pair_log_bonus_;
  if ((score_flags & DFS_MEMBER_SOLO_WORD_EDGE) != 0)
    return multi_word_log_bonus_;
  return 0.0;
}

double DfsScoreModel::member_upper_log_score(
    int64_t count, bool multi_word, uint16_t score_flags) const {
  return segment_log_score(
      count, multi_word,
      (score_flags & DFS_MEMBER_KNOWN_PAIR) != 0) +
      solo_local_upper_log_bonus(score_flags);
}

double DfsScoreModel::displayed_score(double log_score) const {
  return exp(log_score);
}
