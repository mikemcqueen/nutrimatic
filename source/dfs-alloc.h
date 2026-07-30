#ifndef NUTRIMATIC_DFS_ALLOC_H
#define NUTRIMATIC_DFS_ALLOC_H

#include <stdint.h>
#include <stdlib.h>

// Cache-aligned allocation and checked unsigned arithmetic, shared by phase 1's
// arenas and phase 2's flat arrays.

inline constexpr size_t DFS_CACHE_ALIGNMENT = 64;

inline bool dfs_round_up_alignment(size_t bytes, size_t* rounded) {
  if (bytes > SIZE_MAX - (DFS_CACHE_ALIGNMENT - 1)) return false;
  *rounded = (bytes + DFS_CACHE_ALIGNMENT - 1) & ~(DFS_CACHE_ALIGNMENT - 1);
  return true;
}

// Zero bytes returns NULL, which callers must not treat as failure.
inline void* dfs_allocate_aligned(size_t bytes) {
  if (bytes == 0) return NULL;
  size_t rounded;
  if (!dfs_round_up_alignment(bytes, &rounded)) return NULL;
  return aligned_alloc(DFS_CACHE_ALIGNMENT, rounded);
}

inline void* dfs_allocate_aligned_exact(size_t bytes) {
  if (bytes == 0) return NULL;
  void* result = NULL;
  return posix_memalign(&result, DFS_CACHE_ALIGNMENT, bytes) == 0
      ? result
      : NULL;
}

struct DfsAlignedFree {
  void operator()(void* pointer) const { free(pointer); }
};

inline bool dfs_checked_multiply_u64(uint64_t a, uint64_t b, uint64_t* out) {
  if (a != 0 && b > UINT64_MAX / a) return false;
  *out = a * b;
  return true;
}

inline bool dfs_checked_add_u64(uint64_t a, uint64_t b, uint64_t* out) {
  if (b > UINT64_MAX - a) return false;
  *out = a + b;
  return true;
}

#endif
