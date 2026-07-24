#include "dfs-class-list.h"
#include "dfs-search.h"
#include "index.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

static void check(bool ok, char const* message) {
  if (!ok) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
  }
}

class CollectSolutions: public DfsSolutionSink {
 public:
  explicit CollectSolutions(DfsClassList const* classes): classes(classes) { }

  void emit(std::vector<size_t> const& indexes, double log_score) {
    std::vector<std::string> keys;
    for (size_t i = 0; i < indexes.size(); ++i)
      keys.push_back(classes->classes()[indexes[i]].key);
    std::sort(keys.begin(), keys.end());

    std::string key;
    for (size_t i = 0; i < keys.size(); ++i) {
      if (i != 0) key += ' ';
      key += keys[i];
    }
    solutions.insert(key);
    if (key == "ab ab") ab_ab_log_score = log_score;
  }

  DfsClassList const* classes;
  std::set<std::string> solutions;
  double ab_ab_log_score = 0.0;
};

static int smoke_test() {
  FILE* fp = tmpfile();
  check(fp != NULL, "could not create temporary index");
  {
    IndexWriter writer(fp);
    writer.next("a ", 0, 2);
    writer.next("aa ", 0, 3);
    writer.next("ab ", 0, 5);
    writer.next("b ", 0, 7);
    writer.next("bb ", 0, 11);
    writer.next(NULL, 0, 0);
  }
  fflush(fp);
  rewind(fp);

  {
    IndexReader reader(fp);
    DfsClassList classes(&reader, "aabb", 1);
    CollectSolutions sink(&classes);
    DfsAnagramSearch search(&classes, "aabb", 1e-6, reader.count());
    search.run(&sink);

    check(search.solutions_found() == 6, "wrong solution count");
    check(sink.solutions.size() == 6, "permutation duplicate emitted");
    double const expected =
        2.0 * log(5.0) + log(1e-6) - log(double(reader.count()));
    check(fabs(sink.ab_ab_log_score - expected) < 1e-12,
          "wrong representative score");
  }

  fclose(fp);
  return 0;
}

static void check_count(int64_t actual, int64_t expected,
                        char const* message) {
  if (actual != expected) {
    fprintf(stderr, "FAIL: %s: expected %lld, got %lld\n", message,
            (long long) expected, (long long) actual);
    exit(1);
  }
}

static int validate_14_letters() {
  char const* index_path = getenv("IDX");
  if (index_path == NULL || index_path[0] == '\0') {
    fprintf(stderr, "SKIP: export IDX to run the 14-letter validation\n");
    return 77;
  }

  FILE* fp = fopen(index_path, "rb");
  if (fp == NULL) {
    fprintf(stderr, "FAIL: could not open index \"%s\"\n", index_path);
    return 1;
  }

  IndexReader reader(fp);
  std::string const letters = "featstudiotsen";

  DfsClassList words(&reader, letters, 4, false);
  check_count(words.entry_count(), 17274, "words-only entry count");
  check_count(words.classes().size(), 2458, "words-only class count");
  DfsAnagramSearch words_search(&words, letters, 1e-6, reader.count());
  words_search.run(NULL);
  check_count(words_search.solutions_found(), 27177,
              "words-only solution count");
  check_count(words_search.nodes_visited(), 53084, "words-only node count");

  DfsClassList with_phrases(&reader, letters, 4);
  check_count(with_phrases.entry_count(), 18299,
              "phrase-inclusive entry count");
  check_count(with_phrases.classes().size(), 2760,
              "phrase-inclusive class count");
  DfsAnagramSearch phrase_search(
      &with_phrases, letters, 1e-6, reader.count());
  phrase_search.run(NULL);
  check_count(phrase_search.solutions_found(), 27401,
              "phrase-inclusive solution count");
  check_count(phrase_search.nodes_visited(), 54250,
              "phrase-inclusive node count");

  fclose(fp);
  return 0;
}

int main(int argc, char* argv[]) {
  if (argc == 1) return smoke_test();
  if (argc == 2 && strcmp(argv[1], "--validate-14") == 0)
    return validate_14_letters();
  fprintf(stderr, "usage: %s [--validate-14]\n", argv[0]);
  return 2;
}
