#include "segment-report.h"

#include <stdio.h>

int main(int argc, char* argv[]) {
  if (argc > 1 && argv[1][0] == '-') {
    fprintf(stderr,
        "usage: %s [segments-file ...]\n"
        "  reads \"dfs-anagrams --segments\" output, multiplies each score"
        " by its count,\n"
        "  and reprints by descending product; reads standard input with no"
        " file\n",
        argv[0]);
    return 2;
  }

  SegmentReport report;
  if (argc < 2) {
    if (!segment_report_read(stdin, "-", &report)) return 1;
  } else {
    for (int i = 1; i < argc; ++i) {
      FILE* const fp = fopen(argv[i], "r");
      if (fp == NULL) {
        fprintf(stderr, "error: can't open \"%s\"\n", argv[i]);
        return 1;
      }
      bool const read = segment_report_read(fp, argv[i], &report);
      fclose(fp);
      if (!read) return 1;
    }
  }

  segment_report_print(stdout, segment_report_weighted(report));
  return 0;
}
