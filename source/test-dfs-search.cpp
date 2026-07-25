#include "dfs-class-list.h"
#include "dfs-output.h"
#include "dfs-search.h"
#include "index.h"

#include <fenv.h>
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

class FixedFloorSolutions: public DfsSolutionSink {
 public:
  explicit FixedFloorSolutions(double floor): floor(floor) { }

  void emit(std::vector<size_t> const& indexes, double log_score) {
    ordered_indexes.push_back(indexes);
    ordered_scores.push_back(log_score);
  }

  bool supports_score_pruning() const { return true; }
  bool score_floor(double* out) const {
    *out = floor;
    return true;
  }

  double floor;
  std::vector<std::vector<size_t> > ordered_indexes;
  std::vector<double> ordered_scores;
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

static void check_same_spellings(
    std::vector<DfsSpelling> const& expected,
    std::vector<DfsSpelling> const& actual, char const* message) {
  check(actual.size() == expected.size(), message);
  for (size_t i = 0; i < expected.size(); ++i) {
    check(actual[i].log_score == expected[i].log_score, message);
    check(actual[i].text == expected[i].text, message);
    check(actual[i].word_set_key == expected[i].word_set_key, message);
  }
}

static std::string read_stream(FILE* fp) {
  check(fflush(fp) == 0, "could not flush diagnostic stream");
  rewind(fp);
  std::string result;
  char buffer[512];
  while (fgets(buffer, sizeof(buffer), fp) != NULL)
    result += buffer;
  return result;
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
      check(cached.score_bound_mode() ==
                DfsAnagramSearch::SCORE_BOUND_OFF,
            "non-score sink unexpectedly enabled score pruning");
    }

    DfsTopN expected_output(&classes, 2);
    DfsAnagramSearch exhaustive(
        &classes, "aabb", 1e-6, reader.count(), 0);
    exhaustive.run(&expected_output);
    std::vector<DfsSpelling> const expected_spellings =
        expected_output.take_sorted_results();

    size_t const bound_budget = 4096;
    DfsTopN bounded_output(&classes, 2);
    DfsAnagramSearch bounded(
        &classes, "aabb", 1e-6, reader.count(), bound_budget);
    bounded.run(&bounded_output);
    std::vector<DfsSpelling> const bounded_spellings =
        bounded_output.take_sorted_results();
    check(bounded.score_bound_mode() ==
              DfsAnagramSearch::SCORE_BOUND_DENSE,
          "small score memo did not use dense storage");
    check(bounded.score_bound_entries() > 0,
          "dense score memo stored no states");
    check(bounded.score_bound_transitions() > 0,
          "dense score memo counted no successful transitions");
    check(bounded.score_bound_nextafter_calls() > 0,
          "dense score memo counted no nextafter calls");
    check(bounded.phase_two_setup_seconds() >= 0.0,
          "phase-2 setup time was negative");
    check(bounded.phase_two_search_seconds() >= 0.0,
          "phase-2 search time was negative");
    check(bounded.score_bound_bytes_charged() +
              bounded.candidate_cache_bytes_charged() +
              bounded.support_cache_bytes_charged() <=
              bound_budget,
          "combined caches exceeded their byte budget");
    check(bounded.support_cache_entries() > 0,
          "support cache admitted no entries");
    check(bounded.nodes_visited() <= exhaustive.nodes_visited(),
          "score bound visited more nodes than exhaustive DFS");
    check_same_spellings(
        expected_spellings, bounded_spellings,
        "score bound changed the retained spellings");

    DfsTopN isolated_output(&classes, 2);
    size_t const score_only_budget = 128;
    DfsAnagramSearch isolated(
        &classes, "aabb", 1e-6, reader.count(),
        score_only_budget, 1, false);
    isolated.run(&isolated_output);
    check(isolated.score_bound_mode() ==
              DfsAnagramSearch::SCORE_BOUND_DENSE,
          "candidate-cache gate disabled dense score memo");
    check(isolated.candidate_cache_mode() ==
              DfsAnagramSearch::CANDIDATE_CACHE_OFF,
          "candidate-cache gate did not disable candidate storage");
    check(isolated.candidate_cache_entries() == 0 &&
              isolated.candidate_cache_bytes_charged() == 0,
          "disabled candidate cache retained storage");
    check(isolated.support_cache_entries() == 0 &&
              isolated.support_cache_bytes_charged() == 0,
          "disabled candidate cache retained support storage");
    check(isolated.score_bound_bytes_charged() == 64,
          "root-slab compaction charged the wrong dense size");
    check(isolated.score_bound_capacity() == 6 &&
              isolated.score_bound_value_bytes() == sizeof(double) &&
              isolated.score_bound_complete(),
          "root-slab compaction did not retain complete effective coverage");
    check_same_spellings(
        expected_spellings, isolated_output.take_sorted_results(),
        "candidate-cache gate changed retained spellings");

    DfsTopN threaded_output(&classes, 2);
    DfsAnagramSearch threaded(
        &classes, "aabb", 1e-6, reader.count(), bound_budget, 4);
    threaded.run(&threaded_output);
    std::vector<DfsSpelling> const threaded_spellings =
        threaded_output.take_sorted_results();
    check(threaded.preprocess_threads_used() > 1,
          "dense score memo did not use requested preprocessing threads");
    check(threaded.score_bound_mode() == bounded.score_bound_mode(),
          "threading changed score-bound mode");
    check(threaded.score_bound_entries() == bounded.score_bound_entries(),
          "threading changed score-bound entry count");
    check(threaded.score_bound_states_computed() ==
              bounded.score_bound_states_computed(),
          "threading changed computed bound-state count");
    check(threaded.score_bound_transitions() ==
              bounded.score_bound_transitions(),
          "threading changed successful bound transitions");
    check(threaded.score_bound_nextafter_calls() ==
              bounded.score_bound_nextafter_calls(),
          "threading changed bound nextafter calls");
    check(threaded.nodes_visited() == bounded.nodes_visited(),
          "threading changed phase-2 node count");
    check(threaded.solutions_found() == bounded.solutions_found(),
          "threading changed phase-2 solution count");
    check_same_spellings(
        bounded_spellings, threaded_spellings,
        "threaded score bound changed the retained spellings");

    std::string const exhausted_letters = "aaaaabbbbb";
    DfsTopN exhausted_expected_output(&classes, 2);
    DfsAnagramSearch exhausted_expected(
        &classes, exhausted_letters, 1e-6, reader.count(), 0);
    exhausted_expected.run(&exhausted_expected_output);
    std::vector<DfsSpelling> const exhausted_expected_spellings =
        exhausted_expected_output.take_sorted_results();

    DfsTopN expanded_dense_output(&classes, 2);
    size_t const exact_dense_budget = 256;
    DfsAnagramSearch expanded_dense(
        &classes, exhausted_letters, 1e-6, reader.count(),
        exact_dense_budget);
    expanded_dense.run(&expanded_dense_output);
    check(expanded_dense.score_bound_mode() ==
              DfsAnagramSearch::SCORE_BOUND_DENSE,
          "full-budget dense score memo was not selected");
    check(expanded_dense.score_bound_bytes_charged() ==
              exact_dense_budget,
          "dense score memo did not use its exact full-budget fit");
    check(expanded_dense.score_bound_capacity() == 30,
          "dense score memo retained the unused root slab");
    check(expanded_dense.score_bound_prunes() > 0,
          "deep dense score memo did not prune");
    check_same_spellings(
        exhausted_expected_spellings,
        expanded_dense_output.take_sorted_results(),
        "full-budget dense score memo changed retained spellings");

    check(setenv("NUTRIMATIC_PROJECTED_SCORE_D", "0", 1) == 0,
          "could not enable projected score-bound experiment");
    DfsTopN projected_output(&classes, 2);
    DfsAnagramSearch projected(
        &classes, exhausted_letters, 1e-6, reader.count(), 64, 4);
    projected.run(&projected_output);
    check(unsetenv("NUTRIMATIC_PROJECTED_SCORE_D") == 0,
          "could not disable projected score-bound experiment");
    check(projected.score_bound_mode() ==
              DfsAnagramSearch::SCORE_BOUND_PROJECTED &&
              projected.score_bound_complete(),
          "projected score memo did not retain complete coverage");
    check(projected.score_bound_exact_letters() == 0 &&
              projected.score_bound_wild_letters() ==
                  exhausted_letters.size(),
          "projected score memo used the wrong abstraction");
    check(projected.score_bound_capacity() ==
              exhausted_letters.size() + 1,
          "wildcard-only projected score memo has the wrong size");
    check_same_spellings(
        exhausted_expected_spellings,
        projected_output.take_sorted_results(),
        "projected score memo changed retained spellings");

    CollectSolutions boundary_expected(&classes);
    DfsAnagramSearch boundary_exhaustive(
        &classes, exhausted_letters, 1e-6, reader.count(), 0);
    boundary_exhaustive.run(&boundary_expected);
    check(!boundary_expected.ordered_scores.empty(),
          "rounding-boundary test found no solutions");
    size_t const best_position = size_t(std::max_element(
        boundary_expected.ordered_scores.begin(),
        boundary_expected.ordered_scores.end()) -
        boundary_expected.ordered_scores.begin());
    double const best_score =
        boundary_expected.ordered_scores[best_position];
    std::vector<size_t> const& best_path =
        boundary_expected.ordered_indexes[best_position];
    FixedFloorSolutions boundary_output(
        nextafter(best_score, -HUGE_VAL));
    DfsAnagramSearch boundary(
        &classes, exhausted_letters, 1e-6, reader.count(), 128);
    boundary.run(&boundary_output);
    check(boundary.score_bound_mode() ==
              DfsAnagramSearch::SCORE_BOUND_DENSE &&
              boundary.score_bound_value_bytes() == sizeof(float) &&
              boundary.score_bound_complete(),
          "rounding-boundary test did not use complete float bounds");
    check(std::find(
              boundary_output.ordered_indexes.begin(),
              boundary_output.ordered_indexes.end(),
              best_path) != boundary_output.ordered_indexes.end(),
          "one-ulp score floor pruned the best deep solution");

    int const original_rounding = fegetround();
    if (original_rounding != -1 && fesetround(FE_DOWNWARD) == 0) {
      DfsTopN downward_output(&classes, 2);
      DfsAnagramSearch downward(
          &classes, exhausted_letters, 1e-6, reader.count(), 768);
      downward.run(&downward_output);
      check(downward.score_bound_mode() ==
                DfsAnagramSearch::SCORE_BOUND_OFF,
            "score memo ignored a non-nearest rounding mode");
      check(fesetround(original_rounding) == 0,
            "could not restore floating-point rounding mode");
    }

    DfsTopN exhausted_output(&classes, 2);
    size_t const exhausted_budget = 64;
    DfsAnagramSearch exhausted(
        &classes, exhausted_letters, 1e-6, reader.count(),
        exhausted_budget);
    FILE* exhausted_diagnostics = tmpfile();
    check(exhausted_diagnostics != NULL,
          "could not create exhaustion diagnostic stream");
    exhausted.run(&exhausted_output, exhausted_diagnostics);
    std::string const exhaustion_message =
        read_stream(exhausted_diagnostics);
    fclose(exhausted_diagnostics);
    std::vector<DfsSpelling> const exhausted_spellings =
        exhausted_output.take_sorted_results();
    check(exhausted.score_bound_mode() ==
              DfsAnagramSearch::SCORE_BOUND_PREFIX,
          "partial dense score prefix was not selected");
    check(exhausted.score_bound_states_computed() > 0,
          "dense score prefix did not lazily construct any bounds");
    check(exhausted.score_bound_entries() ==
              exhausted.score_bound_states_computed(),
          "dense score prefix lost completed bounds");
    check(exhausted.score_bound_bytes_charged() > 0,
          "dense score prefix released its storage");
    check(exhausted.score_bound_bytes_charged() +
              exhausted.candidate_cache_bytes_charged() +
              exhausted.support_cache_bytes_charged() <=
              exhausted_budget,
          "dense score prefix exceeded the shared cache budget");
    check(exhausted.score_bound_bytes_charged() == exhausted_budget,
          "dense score prefix did not use the full cache budget");
    check(exhausted.candidate_cache_mode() ==
              DfsAnagramSearch::CANDIDATE_CACHE_OFF &&
              exhausted.candidate_cache_bytes_charged() == 0 &&
              exhausted.support_cache_bytes_charged() == 0,
          "dense score prefix left budget for candidate caching");
    check(exhaustion_message.find(
              "score-bound mode dense prefix "
              "(4-byte values, capacity 16, partial coverage)") !=
              std::string::npos,
          "dense score prefix capacity diagnostic is missing");
    check(exhaustion_message.find(
              "precomputed 0 bounded states") != std::string::npos,
          "dense score prefix was eagerly constructed");
    check(exhaustion_message.find(
              "dense-prefix bounds will be constructed lazily during search "
              "for score keys below 16 once a score floor is available") !=
              std::string::npos,
          "dense score prefix lazy-construction diagnostic is missing");
    check_same_spellings(
        exhausted_expected_spellings, exhausted_spellings,
        "exhausted score memo changed the retained spellings");
  }

  fclose(fp);
  return 0;
}

static void float_score_bound_test() {
  FILE* fp = tmpfile();
  check(fp != NULL, "could not create sparse-bound test index");
  {
    IndexWriter writer(fp);
    writer.next("abcdefgh ", 0, 10);
    writer.next(NULL, 0, 0);
  }
  fflush(fp);
  rewind(fp);

  {
    IndexReader reader(fp);
    std::string const letters = "abcdefgh";
    DfsClassList classes(&reader, letters, 1, false);
    DfsTopN expected_output(&classes, 1);
    DfsAnagramSearch exhaustive(
        &classes, letters, 1e-6, reader.count(), 0);
    exhaustive.run(&expected_output);
    std::vector<DfsSpelling> const expected_spellings =
        expected_output.take_sorted_results();

    DfsTopN output(&classes, 1);
    size_t const budget = 512;
    DfsAnagramSearch search(
        &classes, letters, 1e-6, reader.count(), budget);
    search.run(&output);
    check(search.score_bound_mode() ==
              DfsAnagramSearch::SCORE_BOUND_DENSE,
          "complete float score memo was not selected");
    check(search.score_bound_value_bytes() == sizeof(float) &&
              search.score_bound_capacity() == 128 &&
              search.score_bound_complete(),
          "complete float score memo has the wrong layout");
    check(search.score_bound_entries() == 1,
          "float score memo stored the wrong states");
    check(search.score_bound_transitions() == 1,
          "float score memo counted the wrong transitions");
    check(search.score_bound_nextafter_calls() >= 2,
          "float score memo counted too few nextafter calls");
    check(search.score_bound_bytes_charged() +
              search.candidate_cache_bytes_charged() +
              search.support_cache_bytes_charged() <= budget,
          "float score and candidate caches exceeded their budget");
    check(output.size() == 1, "float score bound lost its solution");
    check_same_spellings(
        expected_spellings, output.take_sorted_results(),
        "float score bound changed the retained spellings");
  }

  fclose(fp);
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
    float_score_bound_test();
    return 0;
  }
  if (argc == 2 && strcmp(argv[1], "--validate-14") == 0)
    return validate_14_letters();
  fprintf(stderr, "usage: %s [--validate-14]\n", argv[0]);
  return 2;
}
