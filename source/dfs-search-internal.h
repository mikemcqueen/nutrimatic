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

uint32_t float_to_bits(float value);
float bits_to_float(uint32_t bits);
float round_float_score_bound_up(double value);
void bound_wait_backoff(unsigned int* spins);

uint32_t packed_rank(uint32_t requirement);
uint32_t packed_count(uint32_t requirement);
uint32_t hot_letter_length(uint32_t packed);
uint32_t hot_requirement_count(uint32_t packed);
uint32_t hot_repeated_count(uint32_t packed);
uint32_t projected_total_length(uint32_t packed);
uint32_t projected_wild_length(uint32_t packed);

double round_score_bound_up(long double value, uint64_t* nextafter_calls);

#endif
