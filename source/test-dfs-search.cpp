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
    ordered_indexes.push_back(indexes);
    ordered_scores.push_back(log_score);
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
  std::vector<std::vector<size_t> > ordered_indexes;
  std::vector<double> ordered_scores;
  double ab_ab_log_score = 0.0;
};

static void check_same_run(CollectSolutions const& expected,
                           DfsAnagramSearch const& expected_search,
                           CollectSolutions const& actual,
                           DfsAnagramSearch const& actual_search,
                           char const* message) {
  check(actual_search.nodes_visited() == expected_search.nodes_visited(),
        message);
  check(actual_search.solutions_found() ==
            expected_search.solutions_found(),
        message);
  check(actual.ordered_indexes == expected.ordered_indexes, message);
  check(actual.ordered_scores == expected.ordered_scores, message);
}

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

    size_t const budgets[] = { 1024, 192, 128 };
    DfsAnagramSearch::CandidateCacheMode const modes[] = {
      DfsAnagramSearch::CANDIDATE_CACHE_DENSE,
      DfsAnagramSearch::CANDIDATE_CACHE_SPARSE,
      DfsAnagramSearch::CANDIDATE_CACHE_SPARSE,
    };
    for (size_t i = 0; i < sizeof(budgets) / sizeof(budgets[0]); ++i) {
      CollectSolutions cached_sink(&classes);
      DfsAnagramSearch cached(
          &classes, "aabb", 1e-6, reader.count(), budgets[i]);
      cached.run(&cached_sink);
      check(cached.candidate_cache_mode() == modes[i],
            "wrong candidate cache mode");
      check(cached.candidate_cache_bytes_charged() <= budgets[i],
            "candidate cache exceeded its byte budget");
      check_same_run(sink, search, cached_sink, cached,
                     "candidate cache changed ordered DFS results");
    }
  }

  fclose(fp);
  return 0;
}

static void entry_point_cache_test() {
  FILE* fp = tmpfile();
  check(fp != NULL, "could not create entry-point test index");
  {
    IndexWriter writer(fp);
    writer.next("a ", 0, 2);
    writer.next("aa ", 0, 3);
    writer.next("aaa ", 0, 5);
    writer.next("aaaa ", 0, 7);
    writer.next(NULL, 0, 0);
  }
  fflush(fp);
  rewind(fp);

  {
    IndexReader reader(fp);
    std::string const letters = "aaaaaaaa";
    DfsClassList classes(&reader, letters, 1);

    CollectSolutions expected(&classes);
    DfsAnagramSearch uncached(
        &classes, letters, 1e-6, reader.count(), 0);
    uncached.run(&expected);

    CollectSolutions actual(&classes);
    DfsAnagramSearch cached(
        &classes, letters, 1e-6, reader.count(), 2048);
    cached.run(&actual);
    check(cached.candidate_cache_mode() ==
              DfsAnagramSearch::CANDIDATE_CACHE_DENSE,
          "entry-point regression did not use dense cache");
    check_same_run(expected, uncached, actual, cached,
                   "entry-point-dependent cache reuse changed DFS results");
  }

  fclose(fp);
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
  if (argc == 1) {
    smoke_test();
    entry_point_cache_test();
    return 0;
  }
  if (argc == 2 && strcmp(argv[1], "--validate-14") == 0)
    return validate_14_letters();
  fprintf(stderr, "usage: %s [--validate-14]\n", argv[0]);
  return 2;
}
