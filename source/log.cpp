#include "log.h"

#include <stdio.h>
#include <unistd.h>

#include <string_view>

namespace {

struct LogStyle {
  char const* color;
  char const* label;
};

LogStyle log_style(LogLevel level) {
  switch (level) {
    case LogLevel::WARNING: return { "\033[33m", "WARNING: " };
    case LogLevel::ALERT: return { "\033[31m", "WARNING: " };
    case LogLevel::INFO: return { "\033[33m", "" };
    case LogLevel::SUCCESS: return { "\033[32m", "" };
    case LogLevel::PLAIN: return { "", "" };
  }
  return { "", "" };
}

void program_log_v(
    std::string_view program, LogLevel level, char const* format,
    va_list args) {
  flockfile(stderr);
  fprintf(stderr, "%.*s: ", int(program.size()), program.data());
  log_line_v("", level, format, args);
  funlockfile(stderr);
}

}  // namespace

void log_line_v(
    char const* prefix, LogLevel level, char const* format, va_list args) {
  LogStyle const style = log_style(level);
  bool const tty = isatty(fileno(stderr)) && *style.color;
  flockfile(stderr);
  fprintf(stderr, "%s%s%s", prefix, tty ? style.color : "", style.label);
  vfprintf(stderr, format, args);
  fprintf(stderr, "%s\n", tty ? "\033[0m" : "");
  funlockfile(stderr);
}

void report(std::string_view program, char const* format, ...) {
  va_list args;
  va_start(args, format);
  program_log_v(program, LogLevel::PLAIN, format, args);
  va_end(args);
}

void warn(std::string_view program, char const* format, ...) {
  va_list args;
  va_start(args, format);
  program_log_v(program, LogLevel::WARNING, format, args);
  va_end(args);
}

void alert(std::string_view program, char const* format, ...) {
  va_list args;
  va_start(args, format);
  program_log_v(program, LogLevel::ALERT, format, args);
  va_end(args);
}

void success(std::string_view program, char const* format, ...) {
  va_list args;
  va_start(args, format);
  program_log_v(program, LogLevel::SUCCESS, format, args);
  va_end(args);
}
