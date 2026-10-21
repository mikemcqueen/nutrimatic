#ifndef NUTRIMATIC_LOG_H
#define NUTRIMATIC_LOG_H

#include <stdarg.h>

#include <string_view>

// The arguments a "%.*s" conversion takes to print the string_view `sv`.
#define SV_ARG(sv) int((sv).size()), (sv).data()

// How a log line is colored and labeled: WARNING in yellow and ALERT in red,
// both labeled "WARNING: "; INFO in yellow and SUCCESS in green, unlabeled;
// PLAIN uncolored and unlabeled.
enum class LogLevel { WARNING, ALERT, INFO, SUCCESS, PLAIN };

// Prints PREFIX, then `level`'s label and the formatted text, to stderr, the
// label and text in `level`'s color when stderr is a terminal. The format takes
// no trailing newline.
void log_line_v(
    char const* prefix, LogLevel level, char const* format, va_list args);

// Prints "PROGRAM: WARNING: TEXT" to stderr, with the warning half in yellow
// when stderr is a terminal. The format takes no trailing newline.
void warn(std::string_view program, char const* format, ...)
    __attribute__((format(printf, 2, 3)));

// As warn(), but in red, for a warning a run should not miss.
void alert(std::string_view program, char const* format, ...)
    __attribute__((format(printf, 2, 3)));

// Prints "PROGRAM: TEXT" to stderr, uncolored, for errors and progress. The
// format takes no trailing newline.
void report(std::string_view program, char const* format, ...)
    __attribute__((format(printf, 2, 3)));

// Prints "PROGRAM: TEXT" to stderr, in green when stderr is a terminal, for
// the resolutions a run should be able to see it made. The format takes no
// trailing newline.
void success(std::string_view program, char const* format, ...)
    __attribute__((format(printf, 2, 3)));

#endif
