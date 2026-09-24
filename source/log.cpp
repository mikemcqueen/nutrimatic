#include "log.h"

#include <stdio.h>
#include <unistd.h>

#include <string>

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
  }
  return { "", "" };
}

void program_log_v(
    char const* program, LogLevel level, char const* format, va_list args) {
  std::string const prefix = std::string(program) + ": ";
  log_line_v(prefix.c_str(), level, format, args);
}

}  // namespace

void log_line_v(
    char const* prefix, LogLevel level, char const* format, va_list args) {
  LogStyle const style = log_style(level);
  bool const tty = isatty(fileno(stderr));
  fprintf(stderr, "%s%s%s", prefix, tty ? style.color : "", style.label);
  vfprintf(stderr, format, args);
  fprintf(stderr, "%s\n", tty ? "\033[0m" : "");
}

void warn(char const* program, char const* format, ...) {
  va_list args;
  va_start(args, format);
  program_log_v(program, LogLevel::WARNING, format, args);
  va_end(args);
}

void alert(char const* program, char const* format, ...) {
  va_list args;
  va_start(args, format);
  program_log_v(program, LogLevel::ALERT, format, args);
  va_end(args);
}

void success(char const* program, char const* format, ...) {
  va_list args;
  va_start(args, format);
  program_log_v(program, LogLevel::SUCCESS, format, args);
  va_end(args);
}
