#include "index.h"
#include "search.h"

#include <stdio.h>
#include <string.h>

void PrintAll(SearchDriver* d, FILE* progress, int progress_factor) {
  long long const interval = 100000LL * (progress_factor > 0 ? progress_factor : 1);
  long long count = 0;
  for (;;) {
    if (!(++count % interval)) {
      fprintf(progress, "# %lld\n", count);
      fflush(progress);
      // Results are block-buffered when stdout is a pipe.  If progress went
      // somewhere else then flushing it didn't move them, so nudge them out
      // on the same schedule rather than paying for a flush on every line.
      if (progress != stdout) fflush(stdout);
    }
    if (d->step()) {
      if (d->text == NULL) break;
      int len = strlen(d->text);
      while (len > 0 && d->text[len - 1] == ' ') --len;
      // # - don't suppress trailing zeros (maintains proper padding)
      printf("%#.4g %.*s\n", d->score, len, d->text);
    }
  }
}
