#include "segment-rows.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <string_view>

#include "log.h"

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
    report(reader->program, "%s:%" PRIu64
           ": expected \"score segment[,segment ...]\"", reader->name,
           reader->line_number);
    return reject_row(reader);
  }

  out->segments_start = size_t(score_end - line.c_str()) + 1;
  char const first = line[out->segments_start];
  if ((first >= 'A' && first <= 'Z') || first == '-') {
    size_t const space = line.find(' ', out->segments_start);
    if (space == std::string::npos || space + 1 == line.size()) {
      report(reader->program, "%s:%" PRIu64
             ": expected \"score segment[,segment ...]\"", reader->name,
             reader->line_number);
      return reject_row(reader);
    }
    out->segments_start = space + 1;
  }

  bool const infer_letters =
      reader->infer_letters && reader->required_letters.empty();
  bool const check_letters = !reader->required_letters.empty();
  std::string row_letters;
  out->segments.clear();
  size_t start = out->segments_start;
  while (true) {
    size_t const end = line.find(',', start);
    size_t const length =
        end == std::string::npos ? line.size() - start : end - start;
    if (length == 0) {
      report(reader->program, "%s:%" PRIu64 ": empty segment", reader->name,
             reader->line_number);
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
          report(reader->program, "%s:%" PRIu64
                 ": malformed spacing in segment \"%s\"", reader->name,
                 reader->line_number, segment.c_str());
          return reject_row(reader);
        }
        after_space = true;
      } else if (segment_char(ch)) {
        if (check_letters || infer_letters) row_letters.push_back(ch);
        after_space = false;
      } else {
        report(reader->program, "%s:%" PRIu64
               ": bad character '%c' in segment", reader->name,
               reader->line_number, ch);
        return reject_row(reader);
      }
    }

    if (end == std::string::npos) break;
    start = end + 1;
  }

  if (reader->infer_segments && reader->required_segments == 0) {
    reader->required_segments = int(out->segments.size());
  } else if (reader->required_segments != 0 &&
             out->segments.size() != size_t(reader->required_segments)) {
    if (reader->infer_segments)
      report(reader->program, "%s:%" PRIu64
             ": found %zu segments, but the first row has %d;"
             " pass -g0 to accept mixed counts", reader->name,
             reader->line_number, out->segments.size(),
             reader->required_segments);
    else
      report(reader->program, "%s:%" PRIu64
             ": expected %d segments, found %zu", reader->name,
             reader->line_number, reader->required_segments,
             out->segments.size());
    return reject_row(reader);
  }
  if (infer_letters) {
    reader->required_letters = row_letters;
    return true;
  }
  if (!check_letters) return true;
  std::string bag = reader->required_letters;
  std::sort(bag.begin(), bag.end());
  std::sort(row_letters.begin(), row_letters.end());
  if (row_letters != bag) {
    report(reader->program, "%s:%" PRIu64
           ": row does not spell target letters \"%s\"", reader->name,
           reader->line_number, reader->required_letters.c_str());
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
    report(reader->program, "can't read \"%s\"", reader->name);
    reader->failed = true;
  }
  return false;
}
