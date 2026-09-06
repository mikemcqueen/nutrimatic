#ifndef NUTRIMATIC_LOG_H
#define NUTRIMATIC_LOG_H

// Prints "PROGRAM: WARNING: TEXT" to stderr, with the warning half in yellow
// when stderr is a terminal. The format takes no trailing newline.
void warn(char const* program, char const* format, ...)
    __attribute__((format(printf, 2, 3)));

// Prints "PROGRAM: TEXT" to stderr, in green when stderr is a terminal, for
// the resolutions a run should be able to see it made. The format takes no
// trailing newline.
void success(char const* program, char const* format, ...)
    __attribute__((format(printf, 2, 3)));

#endif
