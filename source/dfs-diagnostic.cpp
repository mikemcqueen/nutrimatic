#include "dfs-diagnostic.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>

#include <chrono>

namespace {

typedef std::chrono::steady_clock DiagnosticClock;
DiagnosticClock::time_point diagnostic_start = DiagnosticClock::now();

FILE* g_diagnostic_stream = NULL;

// Long enough for every diagnostic this program emits; a line that does not fit
// is truncated rather than heap-allocated, which keeps this callable from
// abort() paths, including ones reached because an allocation just failed.
size_t const DIAGNOSTIC_LINE_MAX = 256;

void diagnostic_v(FILE* stream, char const* format, va_list args) {
  if (stream == NULL) return;

  uint64_t const elapsed_seconds =
      uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
          DiagnosticClock::now() - diagnostic_start).count());
  char line[DIAGNOSTIC_LINE_MAX];
  int const prefix_size = snprintf(
      line, sizeof(line), "[%02llu:%02llu:%02llu] ",
      (unsigned long long) (elapsed_seconds / 3600),
      (unsigned long long) ((elapsed_seconds / 60) % 60),
      (unsigned long long) (elapsed_seconds % 60));
  if (prefix_size < 0 || size_t(prefix_size) >= sizeof(line)) return;

  vsnprintf(
      line + prefix_size, sizeof(line) - size_t(prefix_size), format, args);
  fputs(line, stream);
  fflush(stream);
}

}  // namespace

void dfs_reset_diagnostic_clock() {
  diagnostic_start = DiagnosticClock::now();
}

FILE* dfs_set_diagnostic_stream(FILE* stream) {
  FILE* const previous = g_diagnostic_stream;
  g_diagnostic_stream = stream;
  return previous;
}

FILE* dfs_diagnostic_stream() {
  return g_diagnostic_stream;
}

void dfs_check_failed(char const* file, int line, char const* expr) {
  fflush(stdout);
  dfs_diagnostic_to_stream(
      stderr, "%s:%d: invariant failed: %s\n", file, line, expr);
  abort();
}

void dfs_diagnostic(char const* format, ...) {
  va_list args;
  va_start(args, format);
  diagnostic_v(g_diagnostic_stream, format, args);
  va_end(args);
}

void dfs_diagnostic_to_stream(FILE* stream, char const* format, ...) {
  va_list args;
  va_start(args, format);
  diagnostic_v(stream, format, args);
  va_end(args);
}
