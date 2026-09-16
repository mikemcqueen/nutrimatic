#include "segment-rows.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

bool segment_rows_next(SegmentRowReader* reader, SegmentRow* out) {
  while (std::getline(*reader->input, out->line)) {
    ++reader->line_number;
    if (!out->line.empty() && out->line.back() == '\r') out->line.pop_back();
    if (out->line.empty()) continue;

    char* score_end;
    (void) strtod(out->line.c_str(), &score_end);
    if (score_end == out->line.c_str() || *score_end != ' ' ||
        score_end[1] == '\0') {
      fprintf(stderr,
          "%s: %s:%" PRIu64
          ": expected \"score segment[,segment ...]\"\n",
          reader->program, reader->name, reader->line_number);
      reader->failed = true;
      return false;
    }

    out->segments_start = size_t(score_end - out->line.c_str()) + 1;
    out->segments.clear();
    size_t start = out->segments_start;
    while (true) {
      size_t const end = out->line.find(',', start);
      size_t const length =
          end == std::string::npos ? out->line.size() - start : end - start;
      if (length == 0) {
        fprintf(stderr,
            "%s: %s:%" PRIu64 ": empty segment\n",
            reader->program, reader->name, reader->line_number);
        reader->failed = true;
        return false;
      }
      out->segments.push_back(out->line.substr(start, length));

      if (end == std::string::npos) break;
      start = end + 1;
    }
    return true;
  }

  if (reader->input->bad()) {
    fprintf(stderr, "%s: can't read \"%s\"\n", reader->program, reader->name);
    reader->failed = true;
  }
  return false;
}
