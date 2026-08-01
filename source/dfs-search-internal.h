#ifndef NUTRIMATIC_DFS_SEARCH_INTERNAL_H
#define NUTRIMATIC_DFS_SEARCH_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define DFS_LIKELY(value) __builtin_expect(!!(value), 1)
#define DFS_UNLIKELY(value) __builtin_expect(!!(value), 0)
#else
#define DFS_LIKELY(value) (value)
#define DFS_UNLIKELY(value) (value)
#endif

constexpr uint32_t FLOAT_BOUND_UNSEEN = UINT32_C(0x7fc00001);
constexpr uint32_t FLOAT_BOUND_COMPUTING = UINT32_C(0x7fc00002);
constexpr size_t EXACT_MEMO_LOOKAHEAD_DEFAULT = 16;

bool support_scan_avx2_enabled();

bool projected_bound_requirements(
    uint64_t state_count, size_t value_bytes, size_t* bytes);
bool projected_score_bound_arithmetic_supported();

inline uint32_t packed_rank(uint32_t requirement) { return requirement & 63U; }
inline uint32_t packed_count(uint32_t requirement) { return requirement >> 6; }
inline uint32_t hot_letter_length(uint32_t packed) { return packed & 0xffffU; }
inline uint32_t hot_requirement_count(uint32_t packed) {
  return (packed >> 16) & 0xffU;
}
inline uint32_t hot_repeated_count(uint32_t packed) { return packed >> 24; }
inline uint32_t projected_total_length(uint32_t packed) {
  return packed & 0xffffU;
}
inline uint32_t projected_wild_length(uint32_t packed) { return packed >> 16; }

// The repeated requirements sort first, so testing that prefix is the whole
// multiplicity check once a support mask has already cleared.  Both runners
// scan the same packed array, so they share this.
inline bool hot_multiplicity_fits(
    uint32_t const* requirements, uint32_t packed_length_and_count,
    uint32_t const* bag) {
  uint32_t const repeated = hot_repeated_count(packed_length_and_count);
  for (uint32_t i = 0; i < repeated; ++i) {
    uint32_t const requirement = requirements[i];
    if (bag[packed_rank(requirement)] < packed_count(requirement))
      return false;
  }
  return true;
}

double round_score_bound_up(long double value, uint64_t* nextafter_calls);

#endif
