#ifndef NUTRIMATIC_OPTION_VALUE_H
#define NUTRIMATIC_OPTION_VALUE_H

#include <string.h>

// Matches one option that takes a value in a hand-rolled argv loop, accepting
// the spellings the optparse-based tools accept: "-n N", "-nN", "--name N"
// and "--name=N". Either name may be NULL when the option has no short or no
// long spelling.
//
// Returns false when `argv[*index]` is some other option, leaving `*index`
// and `*value` alone. On a match `*value` is the value and `*index` advances
// over a separate value argument; a value missing from the end of the command
// line still matches, with `*value` set to NULL, so the caller diagnoses it
// the same way it diagnoses an empty or unparsable one.
inline bool match_option_value(
    int argc, char* const argv[], int* index,
    char const* short_name, char const* long_name, char const** value) {
  char const* const option = argv[*index];
  if (short_name != NULL) {
    size_t const length = strlen(short_name);
    if (strncmp(option, short_name, length) == 0) {
      if (option[length] != '\0') {
        *value = option + length;
        return true;
      }
      *value = ++*index == argc ? NULL : argv[*index];
      return true;
    }
  }
  if (long_name != NULL) {
    size_t const length = strlen(long_name);
    if (strncmp(option, long_name, length) == 0) {
      if (option[length] == '=') {
        *value = option + length + 1;
        return true;
      }
      if (option[length] == '\0') {
        *value = ++*index == argc ? NULL : argv[*index];
        return true;
      }
    }
  }
  return false;
}

#endif
