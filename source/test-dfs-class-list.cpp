#include "dfs-class-list.h"
#include "index.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <set>
#include <string>

static void check(bool ok, char const* message) {
  if (!ok) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
  }
}

static std::string member_text(DfsMemberView const& view) {
  return std::string(view.text, view.text_length);
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

    check(list.class_key(begin) == "abcd",
          "phrases were not grouped with their anagram");
    check(list.class_length(begin) == 4, "wrong class length");
    check(list.member_count(begin) == 3, "wrong member count");
    DfsMemberView const best = list.member(begin, 0);
    check(member_text(best) == "cd ab" && best.count == 11 &&
          best.word_count == 2,
          "members are not ordered by descending count");
    check(member_text(list.member(begin, 1)) == "dcba" &&
          list.member(begin, 1).count == 9, "word member is missing");
    check(member_text(list.member(begin, 2)) == "ab cd" &&
          list.member(begin, 2).count == 7, "phrase member is missing");

    // Every class's member range has to land where its record says, which is
    // what the counting-sort scatter and the compaction pass settle.
    std::set<std::string> keys;
    size_t total = 0;
    for (size_t ci = 0; ci < list.classes().size(); ++ci) {
      keys.insert(list.class_key(ci));
      check(list.member_count(ci) > 0, "class has no members");
      total += list.member_count(ci);
      for (size_t mi = 0; mi < list.member_count(ci); ++mi) {
        DfsMemberView const view = list.member(ci, mi);
        std::string letters;
        for (size_t i = 0; i < view.text_length; ++i)
          if (view.text[i] != ' ') letters.push_back(view.text[i]);
        check(letters.size() == list.class_length(ci),
              "member does not belong to its class");
      }
    }
    check(total == list.entry_count(), "member ranges do not tile the arena");
    check(keys.size() == 3 && keys.count("ab") == 1 &&
          keys.count("abcd") == 1 && keys.count("cd") == 1,
          "wrong class keys");
  }

  fclose(fp);
  return 0;
}
