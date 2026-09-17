#include "tail-map.h"

#include <math.h>

#include <algorithm>
#include <functional>
#include <utility>

double normal_quantile(double p) {
  static double const a[6] = {
    -3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
    1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00 };
  static double const b[5] = {
    -5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
    6.680131188771972e+01, -1.328068155288572e+01 };
  static double const c[6] = {
    -7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
    -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00 };
  static double const d[4] = {
    7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
    3.754408661907416e+00 };
  static double const tail = 0.02425;
  if (p <= 0.0) return -INFINITY;
  if (p >= 1.0) return INFINITY;
  if (p < tail || p > 1.0 - tail) {
    bool const upper = p > 0.5;
    double const q = sqrt(-2.0 * log(upper ? 1.0 - p : p));
    double const z =
        (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
        ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    return upper ? -z : z;
  }
  double const q = p - 0.5;
  double const r = q * q;
  return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) *
      q / (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
}

bool TailMap::fit(std::vector<double> const& descending) {
  valid_ = false;
  size_t const finite = descending.size();
  if (finite < 3) return false;

  size_t tail = std::max<size_t>(2, (finite + 99) / 100);
  if (tail >= finite) return false;
  double const edge = descending[tail - 1];
  while (tail < finite && descending[tail] == edge) ++tail;
  if (tail >= finite) return false;

  double const threshold = descending[tail];
  double excess = 0.0;
  for (size_t i = 0; i < tail; ++i) excess += descending[i] - threshold;
  excess /= double(tail);
  if (!(excess > 0.0) || !isfinite(excess)) return false;

  descending_ = descending;
  threshold_ = threshold;
  rate_ = 1.0 / excess;
  probability_ = double(tail) / double(finite);
  finite_ = finite;
  valid_ = true;
  return true;
}

double TailMap::deviation(double value) const {
  if (!valid_ || !isfinite(value)) return NAN;

  if (value > threshold_) {
    double survival = probability_ * exp(-rate_ * (value - threshold_));
    if (survival < 1e-300) survival = 1e-300;
    return -normal_quantile(survival);
  }

  std::pair<std::vector<double>::const_iterator,
            std::vector<double>::const_iterator> const equal =
      std::equal_range(
          descending_.begin(), descending_.end(), value,
          std::greater<double>());
  double const i = double(equal.first - descending_.begin());
  double const j = double(equal.second - descending_.begin());
  double const rank = double(finite_) - 0.5 * (i + j - 1.0);
  return normal_quantile((rank - 0.375) / (double(finite_) + 0.25));
}
