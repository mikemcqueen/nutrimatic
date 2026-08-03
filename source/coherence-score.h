#ifndef NUTRIMATIC_COHERENCE_SCORE_H
#define NUTRIMATIC_COHERENCE_SCORE_H

#include <stdint.h>

#include <vector>

struct PairObservation {
  int64_t left_count;
  int64_t right_count;
  int64_t observed_pair_count;
  bool pair_present;
};

struct PairScore {
  double expected_count;
  double evidence;
};

struct CoherenceSummary {
  bool defined;
  double mean;
  double minimum;
};

PairScore score_pair(PairObservation const& observation,
                     int64_t corpus_total);

CoherenceSummary summarize_coherence(std::vector<double> const& edges);

#endif
