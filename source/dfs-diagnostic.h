#ifndef NUTRIMATIC_DFS_DIAGNOSTIC_H
#define NUTRIMATIC_DFS_DIAGNOSTIC_H

#include <stdio.h>

void dfs_reset_diagnostic_clock();

// Writes a timestamped line to stream and flushes it. A NULL stream is a
// no-op, so callers can gate on their own "should I log?" condition without
// separately checking whether a progress/verbose stream was supplied.
void dfs_diagnostic(FILE* stream, char const* format, ...)
    __attribute__((format(printf, 2, 3)));

// Invariant check that survives NDEBUG. The release build sets b_ndebug=true,
// so plain assert() is compiled out there; use DFS_CHECK for the few
// invariants whose violation is memory-unsafe or silently corrupts a result,
// where continuing is worse than dying. Prefer assert() everywhere else.
//
// The failure path is out of line and noreturn, so the compiler sinks it into
// a cold section and the hot path keeps one not-taken branch.
[[noreturn]] void dfs_check_failed(
    char const* file, int line, char const* expr);

#define DFS_CHECK(expr) \
  ((expr) ? (void) 0 : dfs_check_failed(__FILE__, __LINE__, #expr))

#endif
