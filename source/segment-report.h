#ifndef NUTRIMATIC_SEGMENT_REPORT_H
#define NUTRIMATIC_SEGMENT_REPORT_H

#include <stdio.h>

#include <map>
#include <string>

// The --segments report: one row per index entry used by the retained results,
// keyed by the entry text. log_score is the best score of any result using the
// entry, or, once segment_report_weighted() has been applied, that score times
// count. Both forms print and parse identically, so a weighted report is just
// another report.
struct SegmentStat {
  double log_score;
  size_t count;
};

typedef std::map<std::string, SegmentStat> SegmentReport;

// Records one occurrence of text in a result scoring log_score.
void segment_report_add(
    SegmentReport* report, std::string const& text, double log_score);

// Returns the report with each score multiplied by its count.
SegmentReport segment_report_weighted(SegmentReport const& report);

// Reads rows written by segment_report_print(), merging into whatever report
// already holds: scores combine by maximum, counts by sum. name is used only in
// error messages.
bool segment_report_read(FILE* fp, char const* name, SegmentReport* report);

// Writes score, count and text by descending score, breaking ties on text.
// Counts are right aligned to the width of the largest of them.
void segment_report_print(FILE* fp, SegmentReport const& report);

#endif
