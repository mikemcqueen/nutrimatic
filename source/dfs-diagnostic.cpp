#include "dfs-diagnostic.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>

#include <chrono>

#include "log.h"

namespace {

typedef std::chrono::steady_clock DiagnosticClock;
DiagnosticClock::time_point diagnostic_start = DiagnosticClock::now();

FILE* g_diagnostic_stream = NULL;

// Long enough for every diagnostic this program emits; a line that does not fit
// is truncated rather than heap-allocated, which keeps this callable from
// abort() paths, including ones reached because an allocation just failed.
size_t const DIAGNOSTIC_LINE_MAX = 256;

// Writes the "[HH:MM:SS] " elapsed-time prefix into `line`, returning its
// length, or -1 if it does not fit.
int elapsed_prefix(char* line, size_t size) {
  uint64_t const elapsed_seconds =
      uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
          DiagnosticClock::now() - diagnostic_start).count());
  int const prefix_size = snprintf(
      line, size, "[%02llu:%02llu:%02llu] ",
      (unsigned long long) (elapsed_seconds / 3600),
      (unsigned long long) ((elapsed_seconds / 60) % 60),
      (unsigned long long) (elapsed_seconds % 60));
  return prefix_size < 0 || size_t(prefix_size) >= size ? -1 : prefix_size;
}

void diagnostic_v(FILE* stream, char const* format, va_list args) {
  if (stream == NULL) return;

  char line[DIAGNOSTIC_LINE_MAX];
  int const prefix_size = elapsed_prefix(line, sizeof(line));
  if (prefix_size < 0) return;

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

void dfs_diagnostic_log(LogLevel level, char const* format, ...) {
  char prefix[DIAGNOSTIC_LINE_MAX];
  if (elapsed_prefix(prefix, sizeof(prefix)) < 0) return;
  va_list args;
  va_start(args, format);
  log_line_v(prefix, level, format, args);
  va_end(args);
}
