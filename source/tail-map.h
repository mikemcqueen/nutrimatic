#ifndef NUTRIMATIC_TAIL_MAP_H
#define NUTRIMATIC_TAIL_MAP_H

#include <stddef.h>

#include <vector>

// The standard normal quantile. Acklam's rational approximation, to about
// 1e-9 relative.
double normal_quantile(double p);

// A monotone map from one batch of log-space values onto the deviations they
// would carry if the batch's upper tail were normal rather than exponential:
// the top percent through a fitted exponential tail and the rest through
// their own ranks. See findings/parametric-tail-map.md.
class TailMap {
 public:
  // `descending` holds the batch's finite values, highest first. Returns
  // false, leaving the map invalid, when there is no tail to separate from a
  // body: fewer than three values, or none below the threshold.
  bool fit(std::vector<double> const& descending);

  bool valid() const { return valid_; }
  double rate() const { return rate_; }
  double threshold() const { return threshold_; }
  size_t size() const { return finite_; }

  // The deviation `value` would carry in a normal batch. `value` need not be
  // one of the fitted values; ties share one mid-rank and one deviation.
  double deviation(double value) const;

 private:
  std::vector<double> descending_;
  double threshold_ = 0.0;
  double rate_ = 0.0;
  double probability_ = 0.0;
  size_t finite_ = 0;
  bool valid_ = false;
};

#endif
