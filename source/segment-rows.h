#ifndef NUTRIMATIC_SEGMENT_ROWS_H
#define NUTRIMATIC_SEGMENT_ROWS_H

#include <stdint.h>

#include <istream>
#include <string>
#include <vector>

// One nonempty result row. `line` is reused across calls, so a caller that
// needs the text past the current row copies it.
struct SegmentRow {
  std::string line;                   // the row as read, without a trailing CR
  size_t segments_start = 0;          // offset in `line` of the first segment
  std::vector<std::string> segments;  // the comma-separated segments, in order
};

// Reader state over one input.
struct SegmentRowReader {
  std::istream* input = NULL;
  char const* name = NULL;     // the file name in diagnostics; "-" for stdin
  char const* program = NULL;  // the diagnostic prefix
  uint64_t line_number = 0;    // lines consumed, counting skipped blank ones
  bool failed = false;         // a row was malformed, or the stream went bad
};

// Reads the next nonempty row into *out, skipping blank lines. Returns false
// at end of input and on error, having diagnosed the error already; `failed`
// distinguishes the two.
bool segment_rows_next(SegmentRowReader* reader, SegmentRow* out);

#endif
