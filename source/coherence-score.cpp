#include "coherence-score.h"

#include <math.h>

#include <algorithm>
#include <limits>
#include <stdexcept>

static void require(bool condition, char const* message) {
  if (!condition) throw std::invalid_argument(message);
}

PairScore score_pair(PairObservation const& observation,
                     int64_t corpus_total) {
  require(corpus_total > 0, "corpus total must be positive");
  require(observation.left_count > 0, "left count must be positive");
  require(observation.right_count > 0, "right count must be positive");
  require(observation.observed_pair_count >= 0,
          "observed pair count must be non-negative");
  require(observation.observed_pair_count <= observation.left_count,
          "pair count exceeds left count");
  require(observation.observed_pair_count <= observation.right_count,
          "pair count exceeds right count");
  require(!observation.pair_present || observation.observed_pair_count > 0,
          "a present pair must have a positive count");
  require(observation.pair_present || observation.observed_pair_count == 0,
          "an absent pair must have zero count");
  require(observation.left_count <= corpus_total,
          "left count exceeds corpus total");
  require(observation.right_count <= corpus_total,
          "right count exceeds corpus total");
  require(observation.left_count <=
              corpus_total - observation.right_count +
                  observation.observed_pair_count,
          "contingency counts exceed corpus total");

  PairScore result;
  result.expected_count =
      double(observation.left_count) *
      (double(observation.right_count) / double(corpus_total));
  require(std::isfinite(result.expected_count),
          "expected count must be finite");

  long double const observed[] = {
    static_cast<long double>(observation.observed_pair_count),
    static_cast<long double>(
        observation.left_count - observation.observed_pair_count),
    static_cast<long double>(
        observation.right_count - observation.observed_pair_count),
    static_cast<long double>(corpus_total - observation.left_count -
        observation.right_count + observation.observed_pair_count),
  };
  long double const left = observation.left_count;
  long double const right = observation.right_count;
  long double const total = corpus_total;
  long double const expected[] = {
    left * right / total,
    left * (total - right) / total,
    (total - left) * right / total,
    (total - left) * (total - right) / total,
  };
  long double likelihood_ratio = 0.0;
  for (size_t i = 0; i < 4; ++i) {
    if (observed[i] == 0.0L) continue;
    require(expected[i] > 0.0 && std::isfinite(expected[i]),
            "expected contingency count must be positive and finite");
    likelihood_ratio += observed[i] * std::log(observed[i] / expected[i]);
  }
  likelihood_ratio *= 2.0;
  require(std::isfinite(likelihood_ratio) && likelihood_ratio >= 0.0L,
          "likelihood ratio must be non-negative and finite");
  double const direction = observation.observed_pair_count <
          result.expected_count ? -1.0 :
      observation.observed_pair_count > result.expected_count ? 1.0 : 0.0;
  result.evidence = direction * double(std::sqrt(likelihood_ratio));
  require(std::isfinite(result.evidence),
          "pair evidence must be finite");
  return result;
}

CoherenceSummary summarize_coherence(std::vector<double> const& edges) {
  CoherenceSummary result;
  result.defined = !edges.empty();
  result.mean = std::numeric_limits<double>::quiet_NaN();
  result.minimum = std::numeric_limits<double>::quiet_NaN();
  if (edges.empty()) return result;

  double sum = 0.0;
  result.minimum = edges[0];
  for (size_t i = 0; i < edges.size(); ++i) {
    require(std::isfinite(edges[i]), "edge value must be finite");
    sum += edges[i];
    result.minimum = std::min(result.minimum, edges[i]);
  }
  result.mean = sum / double(edges.size());
  require(std::isfinite(result.mean), "coherence mean must be finite");
  return result;
}
