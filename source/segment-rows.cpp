#include "segment-rows.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>

static bool segment_char(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
}

static bool reject_row(SegmentRowReader* reader) {
  reader->failed = true;
  return false;
}

static bool parse_row(SegmentRowReader* reader, SegmentRow* out) {
  std::string const& line = out->line;
  char* score_end;
  double const score = strtod(line.c_str(), &score_end);
  if (score_end == line.c_str() || *score_end != ' ' ||
      score_end[1] == '\0' || !isfinite(score) || score < 0.0) {
    fprintf(stderr,
        "%s: %s:%" PRIu64 ": expected \"score segment[,segment ...]\"\n",
        reader->program, reader->name, reader->line_number);
    return reject_row(reader);
  }

  out->segments_start = size_t(score_end - line.c_str()) + 1;
  char const first = line[out->segments_start];
  if ((first >= 'A' && first <= 'Z') || first == '-') {
    fprintf(stderr,
        "%s: %s:%" PRIu64 ": annotated input is not supported\n",
        reader->program, reader->name, reader->line_number);
    return reject_row(reader);
  }

  bool const check_shape = reader->required_segments != 0;
  std::string row_letters;
  out->segments.clear();
  size_t start = out->segments_start;
  while (true) {
    size_t const end = line.find(',', start);
    size_t const length =
        end == std::string::npos ? line.size() - start : end - start;
    if (length == 0) {
      fprintf(stderr,
          "%s: %s:%" PRIu64 ": empty segment\n",
          reader->program, reader->name, reader->line_number);
      return reject_row(reader);
    }
    out->segments.push_back(line.substr(start, length));
    std::string& segment = out->segments.back();

    size_t const partner = segment.rfind(" (");
    if (partner != std::string::npos && segment.back() == ')' &&
        partner + 3 < segment.size() &&
        std::all_of(segment.begin() + partner + 2, segment.end() - 1,
                    segment_char))
      segment.erase(partner);

    bool after_space = true;
    for (size_t i = 0; i < segment.size(); ++i) {
      char const ch = segment[i];
      if (ch == ' ') {
        if (after_space || i + 1 == segment.size()) {
          fprintf(stderr,
              "%s: %s:%" PRIu64 ": malformed spacing in segment \"%s\"\n",
              reader->program, reader->name, reader->line_number,
              segment.c_str());
          return reject_row(reader);
        }
        after_space = true;
      } else if (segment_char(ch)) {
        if (check_shape) row_letters.push_back(ch);
        after_space = false;
      } else {
        fprintf(stderr,
            "%s: %s:%" PRIu64 ": bad character '%c' in segment\n",
            reader->program, reader->name, reader->line_number, ch);
        return reject_row(reader);
      }
    }

    if (end == std::string::npos) break;
    start = end + 1;
  }

  if (!check_shape) return true;
  if (out->segments.size() != size_t(reader->required_segments)) {
    fprintf(stderr,
        "%s: %s:%" PRIu64 ": expected %d segments, found %zu\n",
        reader->program, reader->name, reader->line_number,
        reader->required_segments, out->segments.size());
    return reject_row(reader);
  }
  std::string bag = reader->required_letters;
  std::sort(bag.begin(), bag.end());
  std::sort(row_letters.begin(), row_letters.end());
  if (row_letters != bag) {
    fprintf(stderr,
        "%s: %s:%" PRIu64 ": row does not spell target letters \"%s\"\n",
        reader->program, reader->name, reader->line_number,
        reader->required_letters.c_str());
    return reject_row(reader);
  }
  return true;
}

bool segment_rows_next(SegmentRowReader* reader, SegmentRow* out) {
  while (std::getline(*reader->input, out->line)) {
    ++reader->line_number;
    if (!out->line.empty() && out->line.back() == '\r') out->line.pop_back();
    if (out->line.empty()) continue;
    return parse_row(reader, out);
  }

  if (reader->input->bad()) {
    fprintf(stderr, "%s: can't read \"%s\"\n", reader->program, reader->name);
    reader->failed = true;
  }
  return false;
}
