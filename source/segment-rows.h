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
  size_t segments_start = 0;          // offset of the first segment, past any
                                      // --show-bonus marker column
  std::vector<std::string> segments;  // comma-separated, solo suffix removed
};

// Reader state over one input.
struct SegmentRowReader {
  std::istream* input = NULL;
  char const* name = NULL;     // the file name in diagnostics; "-" for stdin
  char const* program = NULL;  // the diagnostic prefix

  // Optional row-shape checks, each independent of the other; only
  // rerank-anagrams knows a target, so only rerank-anagrams sets them. A
  // nonempty bag is always checked, since a row that does not spell the
  // target cannot be rescored against it. A zero segment count accepts any
  // count.
  std::string required_letters{}; // the bag every row must spell, or empty
  int required_segments = 0;      // the exact segment count, or 0 for any
  // Take the empty checks above from the first row, so that every later row
  // must match it. A count mismatch then suggests -g0.
  bool infer_letters = false;
  bool infer_segments = false;

  uint64_t line_number = 0;    // lines consumed, counting skipped blank ones
  bool failed = false;         // a row was malformed, or the stream went bad
};

// Reads the next nonempty row into *out, skipping blank lines. Returns false
// at end of input and on error, having diagnosed the error already; `failed`
// distinguishes the two.
bool segment_rows_next(SegmentRowReader* reader, SegmentRow* out);

#endif
