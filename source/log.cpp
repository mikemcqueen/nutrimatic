#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

namespace {

void print(
    char const* program, char const* color, char const* label,
    char const* format, va_list args) {
  bool const tty = isatty(fileno(stderr));
  fprintf(stderr, "%s: %s%s", program, tty ? color : "", label);
  vfprintf(stderr, format, args);
  fprintf(stderr, "%s\n", tty ? "\033[0m" : "");
}

}  // namespace

void warn(char const* program, char const* format, ...) {
  va_list args;
  va_start(args, format);
  print(program, "\033[33m", "WARNING: ", format, args);
  va_end(args);
}

void success(char const* program, char const* format, ...) {
  va_list args;
  va_start(args, format);
  print(program, "\033[32m", "", format, args);
  va_end(args);
}
