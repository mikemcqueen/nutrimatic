#include "dfs-score.h"

#include <assert.h>
#include <math.h>

namespace {

double make_restart_log_rate(double restart, int64_t corpus_total) {
  assert(restart > 0.0);
  assert(corpus_total > 0);
  return log(restart) - log(double(corpus_total));
}

double make_multi_word_log_bonus(double restart, double word_bonus) {
  assert(restart > 0.0);
  assert(isfinite(word_bonus));
  return -word_bonus * log(restart);
}

}  // namespace

DfsScoreModel::DfsScoreModel(
    double restart, int64_t corpus_total, double word_bonus):
    restart_log_rate_(make_restart_log_rate(restart, corpus_total)),
    multi_word_log_bonus_(
        make_multi_word_log_bonus(restart, word_bonus)) { }

double DfsScoreModel::segment_log_score(
    int64_t count, bool multi_word) const {
  assert(count > 0);
  return log(double(count)) +
      (multi_word ? multi_word_log_bonus_ : 0.0);
}

double DfsScoreModel::first_segment_log_score(
    int64_t count, bool multi_word) const {
  return segment_log_score(count, multi_word);
}

double DfsScoreModel::append_segment_log_score(
    double accumulated, int64_t count, bool multi_word) const {
  return append_log_score(
      accumulated, segment_log_score(count, multi_word));
}

double DfsScoreModel::append_log_score(
    double accumulated, double segment_log_score) const {
  return accumulated + restart_log_rate_ + segment_log_score;
}

double DfsScoreModel::displayed_score(double log_score) const {
  return exp(log_score);
}
