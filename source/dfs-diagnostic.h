#ifndef NUTRIMATIC_DFS_DIAGNOSTIC_H
#define NUTRIMATIC_DFS_DIAGNOSTIC_H

#include <stdio.h>

void dfs_reset_diagnostic_clock();

// Returns the previous stream, so callers that want to restore it can do so
// with no separate save step. NULL disables dfs_diagnostic() entirely.
FILE* dfs_set_diagnostic_stream(FILE* stream);

// The stream dfs_diagnostic() currently writes to (possibly NULL). For the
// rare caller that composes a line across multiple raw fprintf/fputc calls
// instead of a single dfs_diagnostic() call.
FILE* dfs_diagnostic_stream();

// Writes a timestamped line to the current diagnostic stream and flushes it.
// A NULL stream is a no-op, so callers can gate on their own "should I log?"
// condition without separately checking whether diagnostics are enabled.
//
// The line is assembled in a fixed stack buffer and truncated if it does not
// fit, so this allocates nothing and is safe to call on an out-of-memory path.
void dfs_diagnostic(char const* format, ...)
    __attribute__((format(printf, 1, 2)));

// As dfs_diagnostic(), but writes to an explicit stream instead of the global
// one. Fatal paths use this with stderr: it reports unconditionally even when
// diagnostics are disabled, and unlike dfs_set_diagnostic_stream() it mutates
// no shared state, which matters because worker threads can abort concurrently.
void dfs_diagnostic_to_stream(FILE* stream, char const* format, ...)
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
