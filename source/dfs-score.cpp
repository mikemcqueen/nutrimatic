#include "dfs-score.h"

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

}  // namespace

DfsScoreModel::DfsScoreModel(
    double segment_penalty, int64_t corpus_total):
    segment_boundary_log_score_(
        make_segment_boundary_log_score(segment_penalty, corpus_total)) { }

double DfsScoreModel::segment_log_score(int64_t count) const {
  assert(count > 0);
  return log(double(count));
}

double DfsScoreModel::first_segment_log_score(int64_t count) const {
  return segment_log_score(count);
}

double DfsScoreModel::append_segment_log_score(
    double accumulated, int64_t count) const {
  return append_log_score(accumulated, segment_log_score(count));
}

double DfsScoreModel::append_log_score(
    double accumulated, double segment_log_score) const {
  return accumulated + segment_boundary_log_score_ + segment_log_score;
}

double DfsScoreModel::displayed_score(double log_score) const {
  return exp(log_score);
}
