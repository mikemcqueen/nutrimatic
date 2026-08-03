#include "segment-report.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <utility>
#include <vector>

void segment_report_add(
    SegmentReport* report, std::string const& text, double log_score) {
  SegmentReport::iterator found = report->find(text);
  if (found == report->end()) {
    SegmentStat stat;
    stat.log_score = log_score;
    stat.count = 1;
    report->insert(std::make_pair(text, stat));
  } else {
    found->second.log_score = std::max(found->second.log_score, log_score);
    ++found->second.count;
  }
}

SegmentReport segment_report_weighted(SegmentReport const& report) {
  SegmentReport weighted;
  for (SegmentReport::const_iterator entry = report.begin();
       entry != report.end(); ++entry) {
    SegmentStat stat = entry->second;
    stat.log_score += log(double(stat.count));
    weighted.insert(weighted.end(), std::make_pair(entry->first, stat));
  }
  return weighted;
}

static bool read_line(FILE* fp, std::string* line) {
  line->clear();
  char buffer[512];
  while (fgets(buffer, sizeof(buffer), fp) != NULL) {
    line->append(buffer);
    if (line->back() == '\n') break;
  }
  if (line->empty()) return false;
  while (!line->empty() &&
         (line->back() == '\n' || line->back() == '\r'))
    line->pop_back();
  return true;
}

bool segment_report_read(
    FILE* fp, char const* name, SegmentReport* report) {
  std::string line;
  size_t line_number = 0;
  while (read_line(fp, &line)) {
    ++line_number;
    if (line.empty()) continue;

    char const* const start = line.c_str();
    char* score_end;
    errno = 0;
    double const score = strtod(start, &score_end);
    char* count_end;
    unsigned long long const count = strtoull(score_end, &count_end, 10);
    if (score_end == start || count_end == score_end || score < 0.0 ||
        errno != 0 || *count_end != ' ' || count_end[1] == '\0') {
      fprintf(stderr, "error: %s:%zu: expected \"score count text\"\n",
              name, line_number);
      return false;
    }

    std::string const text(count_end + 1);
    SegmentStat stat;
    stat.log_score = log(score);
    stat.count = size_t(count);
    std::pair<SegmentReport::iterator, bool> const inserted =
        report->insert(std::make_pair(text, stat));
    if (!inserted.second) {
      inserted.first->second.log_score =
          std::max(inserted.first->second.log_score, stat.log_score);
      inserted.first->second.count += stat.count;
    }
  }

  if (ferror(fp)) {
    fprintf(stderr, "error: %s: %s\n", name, strerror(errno));
    return false;
  }
  return true;
}

void segment_report_print(FILE* fp, SegmentReport const& report) {
  std::vector<SegmentReport::const_iterator> ordered;
  ordered.reserve(report.size());
  size_t widest = 0;
  for (SegmentReport::const_iterator entry = report.begin();
       entry != report.end(); ++entry) {
    ordered.push_back(entry);
    widest = std::max(widest, entry->second.count);
  }
  std::sort(ordered.begin(), ordered.end(),
            [](SegmentReport::const_iterator a,
               SegmentReport::const_iterator b) {
    if (a->second.log_score != b->second.log_score)
      return a->second.log_score > b->second.log_score;
    return a->first < b->first;
  });

  int const count_width = snprintf(NULL, 0, "%zu", widest);
  for (size_t i = 0; i < ordered.size(); ++i)
    fprintf(fp, "%#.4g %*zu %s\n", exp(ordered[i]->second.log_score),
            count_width, ordered[i]->second.count,
            ordered[i]->first.c_str());
}
