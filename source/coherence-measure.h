#ifndef NUTRIMATIC_COHERENCE_MEASURE_H
#define NUTRIMATIC_COHERENCE_MEASURE_H

#include <stddef.h>
#include <stdint.h>

inline constexpr size_t COHERENCE_MEMORY_LIMIT =
    size_t(3072) * 1024 * 1024;
inline constexpr uint64_t COHERENCE_ORDER_RELAXATION_LIMIT =
    UINT64_C(250000000);
inline constexpr size_t COHERENCE_MAX_WORDS = 63;
inline constexpr size_t COHERENCE_MAX_LINE_BYTES = 4096;

struct CoherenceResourceLimits {
  size_t memory_bytes;
  uint64_t order_relaxations;
};

int run_measure_coherence(int argc, char* argv[],
                          CoherenceResourceLimits const& limits);

#endif
