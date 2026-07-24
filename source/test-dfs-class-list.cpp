#include "dfs-class-list.h"
#include "index.h"

#include <stdio.h>
#include <stdlib.h>

#include <string>

static void check(bool ok, char const* message) {
  if (!ok) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
  }
}

int main() {
  FILE* fp = tmpfile();
  check(fp != NULL, "could not create temporary index");
  {
    IndexWriter writer(fp);
    writer.next("ab ", 0, 5);
    writer.next("ab cd ", 0, 7);
    writer.next("ba ", 0, 3);
    writer.next("cd ab ", 0, 11);
    writer.next("dcba ", 0, 9);
    writer.next(NULL, 0, 0);
  }
  fflush(fp);
  rewind(fp);

  {
    IndexReader reader(fp);
    DfsClassList list(&reader, "abcd", 2);
    check(list.entry_count() == 6, "wrong entry count");
    check(list.classes().size() == 3, "wrong class count");

    int const c = dfs_symbol_index('c');
    size_t const begin = list.candidate_begin(c);
    size_t const end = list.candidate_end(c);
    check(end - begin == 2, "wrong rarest-letter bucket");

    DfsAnagramClass const& cls = list.classes()[begin];
    check(cls.key == "abcd", "phrases were not grouped with their anagram");
    check(cls.members.size() == 3, "wrong member count");
    check(cls.members[0].text == "cd ab" && cls.members[0].count == 11 &&
          cls.members[0].word_count == 2,
          "members are not ordered by descending count");
    check(cls.members[1].text == "dcba" && cls.members[1].count == 9,
          "word member is missing");
    check(cls.members[2].text == "ab cd" && cls.members[2].count == 7,
          "phrase member is missing");
  }

  fclose(fp);
  return 0;
}
