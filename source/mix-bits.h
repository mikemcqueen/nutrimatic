#ifndef NUTRIMATIC_MIX_BITS_H
#define NUTRIMATIC_MIX_BITS_H

#include <stdint.h>

// Mixes `value`'s bits for a hash table slot: splitmix64's finalizer.
inline uint64_t mix_bits(uint64_t value) {
  value ^= value >> 30;
  value *= UINT64_C(0xbf58476d1ce4e5b9);
  value ^= value >> 27;
  value *= UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31);
}

#endif
