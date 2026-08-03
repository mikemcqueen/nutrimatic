#include "coherence-measure.h"

int main(int argc, char* argv[]) {
  CoherenceResourceLimits const limits = {
    COHERENCE_MEMORY_LIMIT,
    COHERENCE_ORDER_RELAXATION_LIMIT,
  };
  return run_measure_coherence(argc, argv, limits);
}
