#include "coherence-score.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <limits>
#include <stdexcept>
#include <vector>

static void check(bool condition, char const* message) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
  }
}

static void check_close(double actual, double expected,
                        char const* message) {
  if (fabs(actual - expected) > 1e-12) {
    fprintf(stderr, "FAIL: %s: expected %.17g, got %.17g\n",
            message, expected, actual);
    exit(1);
  }
}

template<typename Function>
static void check_throws(Function function, char const* message) {
  try {
    function();
  } catch (std::invalid_argument const&) {
    return;
  }
  fprintf(stderr, "FAIL: %s\n", message);
  exit(1);
}

int main() {
  PairObservation above = {1000, 2000, 40, true};
  PairScore const above_score = score_pair(above, 100000);
  check(above_score.evidence > 0.0,
        "above-expectation evidence is positive");
  check_close(above_score.expected_count, 20.0, "expected count");

  PairObservation below = {1000, 2000, 2, true};
  check(score_pair(below, 100000).evidence < 0.0,
        "below-expectation evidence is negative");

  PairObservation rare_absent = {10, 10, 0, false};
  PairScore const rare_absent_score =
      score_pair(rare_absent, 1000000000);
  check(rare_absent_score.evidence < 0.0 &&
            fabs(rare_absent_score.evidence) < 0.01,
        "rare absent pair is nearly neutral");

  PairObservation common_absent = {10000, 10000, 0, false};
  PairScore const common_absent_score =
      score_pair(common_absent, 100000);
  check(common_absent_score.evidence < rare_absent_score.evidence,
        "frequent absent pair is stronger negative evidence");

  PairObservation repeated_rare_pair = {10, 10, 10, true};
  check(score_pair(repeated_rare_pair, 1000000000).evidence > 0.0,
        "repeated rare pair is positive evidence");

  check_throws([&]() { score_pair(above, 0); },
               "zero corpus total is rejected");
  check_throws([&]() {
    PairObservation invalid = {1000, 2000, 0, true};
    score_pair(invalid, 100000);
  }, "present zero-count pair is rejected");
  check_throws([&]() {
    PairObservation invalid = {10, 10, 11, true};
    score_pair(invalid, 100000);
  }, "pair count above a marginal is rejected");

  CoherenceSummary const undefined = summarize_coherence({});
  check(!undefined.defined && std::isnan(undefined.mean) &&
            std::isnan(undefined.minimum),
        "one-word reduction is undefined");
  std::vector<double> edges;
  edges.push_back(1.0);
  edges.push_back(-0.5);
  CoherenceSummary const summary = summarize_coherence(edges);
  check(summary.defined, "multi-word reduction is defined");
  check_close(summary.mean, 0.25, "coherence mean");
  check_close(summary.minimum, -0.5, "coherence minimum");
  return 0;
}
