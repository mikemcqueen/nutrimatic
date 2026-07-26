#include "dfs-diagnostic.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>

#include <chrono>

namespace {

typedef std::chrono::steady_clock DiagnosticClock;
DiagnosticClock::time_point diagnostic_start = DiagnosticClock::now();

}  // namespace

void dfs_reset_diagnostic_clock() {
  diagnostic_start = DiagnosticClock::now();
}

void dfs_check_failed(char const* file, int line, char const* expr) {
  // Worker threads can reach this concurrently; one fprintf keeps the message
  // intact, and stderr is unbuffered so it lands before the abort.
  fflush(stdout);
  fprintf(stderr, "%s:%d: invariant failed: %s\n", file, line, expr);
  abort();
}

void dfs_diagnostic(FILE* stream, char const* format, ...) {
  uint64_t const elapsed_seconds =
      uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
          DiagnosticClock::now() - diagnostic_start).count());
  uint64_t const hours = elapsed_seconds / 3600;
  uint64_t const minutes = (elapsed_seconds / 60) % 60;
  uint64_t const seconds = elapsed_seconds % 60;
  fprintf(stream, "[%02llu:%02llu:%02llu] ",
          (unsigned long long) hours,
          (unsigned long long) minutes,
          (unsigned long long) seconds);

  va_list args;
  va_start(args, format);
  vfprintf(stream, format, args);
  va_end(args);
}
