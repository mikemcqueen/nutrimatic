#include "index.h"

#include <stdio.h>
#include <stdlib.h>

static void check(bool condition, char const* message) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
  }
}

int main() {
  FILE* fp = tmpfile();
  check(fp != NULL, "could not create index fixture");
  {
    IndexWriter writer(fp);
    writer.next("red tea ", 0, 3);
    writer.next("red tea cup ", 0, 2);
    writer.next("red tree ", 0, 1);
    writer.next(NULL, 0, 0);
  }
  fflush(fp);
  rewind(fp);

  {
    IndexReader reader(fp);
    int64_t count = 0;
    check(reader.aggregate_entry_count("red", &count) && count == 6,
          "aggregate red count");
    check(reader.aggregate_entry_count("red tea", &count) && count == 5,
          "aggregate red tea count");
    check(reader.exact_entry_count("red tea", &count) && count == 3,
          "exact red tea count");
    check(reader.aggregate_entry_count("red tree", &count) && count == 1,
          "aggregate red tree count");
    check(!reader.aggregate_entry_count("tea red", &count),
          "missing aggregate entry");

    IndexReader::EntryPosition red;
    IndexReader::EntryPosition tea_after_red;
    IndexReader::EntryPosition red_tea;
    check(reader.aggregate_entry_position("red", &red),
          "resolve red position");
    check(reader.continuation_entry_position(red, "tea", &tea_after_red),
          "resolve tea after red");
    check(reader.aggregate_entry_position("red tea", &red_tea),
          "resolve red tea position");
    check(tea_after_red.continuation == red_tea.continuation &&
              tea_after_red.aggregate_count == red_tea.aggregate_count,
          "continuation and root positions agree");
    check(!reader.continuation_entry_position(red, "treehouse", &red_tea),
          "missing continuation returns false");
  }

  fclose(fp);
  return 0;
}
