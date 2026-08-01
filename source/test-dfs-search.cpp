#include "dfs-class-list.h"
#include "dfs-diagnostic.h"
#include "dfs-output.h"
#include "dfs-search-stats.h"
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
      keys.push_back(classes->class_key(indexes[i]));
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
    DfsAnagramSearch search(
        &classes, "aabb", DFS_DEFAULT_SEGMENT_PENALTY, reader.count());
    DfsSearchStats search_stats;
    search.run(&sink, &search_stats);

    check(!sink.supports_parallel_search(),
          "generic collecting sink opted in to parallel search");
    check(search_stats.all_solutions.solutions == 6, "wrong solution count");
    check(sink.solutions.size() == 6, "permutation duplicate emitted");
    check(!search_stats.certificate.ready,
          "sink without a floor enabled the length certificate");
    double const expected =
        2.0 * log(5.0) - log(DFS_DEFAULT_SEGMENT_PENALTY) -
        log(double(reader.count()));
    check(fabs(sink.ab_ab_log_score - expected) < 1e-12,
          "wrong representative score");

    int64_t const first_nodes = search_stats.all_solutions.nodes;
    int64_t const first_solutions = search_stats.all_solutions.solutions;
    CollectSolutions repeated_sink(&classes);
    search.run(&repeated_sink, &search_stats);
    check(search_stats.all_solutions.nodes == first_nodes,
          "reused search changed optimized node count");
    check(search_stats.all_solutions.solutions == first_solutions,
          "reused search changed optimized solution count");
    check(repeated_sink.ordered_indexes == sink.ordered_indexes &&
              repeated_sink.ordered_scores == sink.ordered_scores,
          "reused search changed optimized traversal");

    CollectSolutions serial_only_sink(&classes);
    DfsAnagramSearch serial_only_search(
        &classes, "aabb", DFS_DEFAULT_SEGMENT_PENALTY, reader.count(), 0, 1, 4);
    DfsSearchStats serial_only_search_stats;
    serial_only_search.run(&serial_only_sink, &serial_only_search_stats);
    check(serial_only_search_stats.execution.search_threads == 1 &&
              serial_only_search_stats.execution.search_tasks == 0,
          "generic sink did not fall back to serial search");
    check(serial_only_sink.ordered_indexes == sink.ordered_indexes &&
              serial_only_sink.ordered_scores == sink.ordered_scores,
          "requested parallelism changed generic-sink traversal");

    check(setenv("NUTRIMATIC_SEARCH_TASKS", "2", 1) == 0,
          "could not lower exhaustive parallel task target");
    DfsAnagramSearch parallel_counter(
        &classes, "aabb", DFS_DEFAULT_SEGMENT_PENALTY, reader.count(), 0, 1, 4);
    DfsSearchStats parallel_counter_stats;
    parallel_counter.run(NULL, &parallel_counter_stats);
    check(unsetenv("NUTRIMATIC_SEARCH_TASKS") == 0,
          "could not restore exhaustive parallel task target");
    check(parallel_counter_stats.execution.search_threads > 1 &&
              parallel_counter_stats.all_solutions.nodes ==
                  search_stats.all_solutions.nodes &&
              parallel_counter_stats.all_solutions.solutions ==
                  search_stats.all_solutions.solutions,
          "parallel exhaustive search changed public counters");

    check(setenv(
              "NUTRIMATIC_LENGTH_CERTIFICATE", "0", 1) == 0,
          "could not disable length certificate");
    DfsTopN expected_output(&classes, 2);
    DfsAnagramSearch exhaustive(
        &classes, "aabb", DFS_DEFAULT_SEGMENT_PENALTY, reader.count(), 0);
    DfsSearchStats exhaustive_stats;
    exhaustive.run(&expected_output, &exhaustive_stats);
    check(!exhaustive_stats.certificate.ready,
          "disabled length certificate was prepared");
    std::vector<DfsSpelling> const expected_spellings =
        expected_output.take_sorted_results();

    size_t const bound_budget = 4096;
    DfsTopN disabled_output(&classes, 2);
    DfsAnagramSearch disabled(
        &classes, "aabb", DFS_DEFAULT_SEGMENT_PENALTY, reader.count(),
        bound_budget);
    DfsSearchStats disabled_stats;
    disabled.run(&disabled_output, &disabled_stats);
    std::vector<DfsSpelling> const disabled_spellings =
        disabled_output.take_sorted_results();

    check(setenv(
              "NUTRIMATIC_LENGTH_CERTIFICATE", "shadow", 1) == 0,
          "could not shadow length certificate");
    DfsTopN shadow_output(&classes, 2);
    DfsAnagramSearch shadow(
        &classes, "aabb", DFS_DEFAULT_SEGMENT_PENALTY, reader.count(),
        bound_budget);
    DfsSearchStats shadow_stats;
    shadow.run(&shadow_output, &shadow_stats);
    std::vector<DfsSpelling> const shadow_spellings =
        shadow_output.take_sorted_results();
    check(unsetenv("NUTRIMATIC_LENGTH_CERTIFICATE") == 0,
          "could not enable length certificate");

    DfsTopN bounded_output(&classes, 2);
    DfsAnagramSearch bounded(
        &classes, "aabb", DFS_DEFAULT_SEGMENT_PENALTY, reader.count(),
        bound_budget);
    DfsSearchStats bounded_stats;
    bounded.run(&bounded_output, &bounded_stats);
    std::vector<DfsSpelling> const bounded_spellings =
        bounded_output.take_sorted_results();
    check(bounded_stats.bounds.mode ==
              DFS_SCORE_BOUND_PROJECTED,
          "small score memo did not use projected storage");
    check(bounded_stats.bounds.entries > 0,
          "dense score memo stored no states");
    check(bounded_stats.bounds.projected.transitions > 0,
          "dense score memo counted no successful transitions");
    check(bounded_stats.bounds.projected.nextafter_calls > 0,
          "dense score memo counted no nextafter calls");
    check(bounded_stats.execution.setup_seconds >= 0.0,
          "phase-2 setup time was negative");
    check(bounded_stats.execution.search_seconds >= 0.0,
          "phase-2 search time was negative");
    check(bounded_stats.bounds.bytes_charged <= bound_budget,
          "score cache exceeded its byte budget");
    check(bounded_stats.all_solutions.nodes <=
              exhaustive_stats.all_solutions.nodes,
          "score bound visited more nodes than exhaustive DFS");
    check(shadow_stats.certificate.ready &&
              !shadow_stats.certificate.skipping(),
          "shadow length certificate used the wrong mode");
    check(bounded_stats.certificate.ready &&
              bounded_stats.certificate.skipping(),
          "active length certificate used the wrong mode");
    DfsSearchStats::AllSolutions const& shadow_all =
        shadow_stats.all_solutions;
    DfsSearchStats::AllSolutions const& disabled_all =
        disabled_stats.all_solutions;
    check(shadow_all.nodes == disabled_all.nodes &&
              shadow_all.solutions == disabled_all.solutions &&
              shadow_all.bound_prunes == disabled_all.bound_prunes,
          "shadow length certificate changed traversal counters");
    check(bounded_stats.certificate.counters.scans_skipped > 0,
          "active length certificate skipped no class group");
    check_same_spellings(
        expected_spellings, disabled_spellings,
        "disabled length certificate changed retained spellings");
    check_same_spellings(
        expected_spellings, shadow_spellings,
        "shadow length certificate changed retained spellings");
    check_same_spellings(
        expected_spellings, bounded_spellings,
        "score bound changed the retained spellings");

    check(setenv("NUTRIMATIC_SEARCH_TASKS", "2", 1) == 0,
          "could not lower parallel search task target");
    check(setenv(
              "NUTRIMATIC_LENGTH_CERTIFICATE", "0", 1) == 0,
          "could not disable parallel length certificate");
    DfsTopN parallel_disabled_output(&classes, 2);
    DfsAnagramSearch parallel_disabled(
        &classes, "aabb", DFS_DEFAULT_SEGMENT_PENALTY, reader.count(),
        bound_budget, 1, 4);
    DfsSearchStats parallel_disabled_stats;
    parallel_disabled.run(&parallel_disabled_output, &parallel_disabled_stats);
    check(unsetenv("NUTRIMATIC_LENGTH_CERTIFICATE") == 0,
          "could not restore parallel length certificate");
    check(parallel_disabled_stats.execution.search_threads > 1,
          "certificate-disabled search did not run in parallel");
    check_same_spellings(
        bounded_spellings,
        parallel_disabled_output.take_sorted_results(),
        "certificate-disabled parallel search changed retained spellings");
    for (size_t run = 0; run < 3; ++run) {
      DfsTopN parallel_output(&classes, 2);
      DfsAnagramSearch parallel(
          &classes, "aabb", DFS_DEFAULT_SEGMENT_PENALTY, reader.count(),
          bound_budget, 1, 4);
      DfsSearchStats parallel_stats;
      parallel.run(&parallel_output, &parallel_stats);
      check(parallel_stats.execution.search_threads > 1 &&
                parallel_stats.execution.search_threads <= 4,
            "parallel top-N did not use multiple workers");
      check(parallel_stats.execution.search_tasks > 0,
            "parallel top-N generated no search tasks");
      check_same_spellings(
          bounded_spellings, parallel_output.take_sorted_results(),
          "parallel top-N changed retained spellings");
    }
    check(unsetenv("NUTRIMATIC_SEARCH_TASKS") == 0,
          "could not restore parallel search task target");

    DfsTopN certificate_only_output(&classes, 2);
    DfsAnagramSearch certificate_only(
        &classes, "aabb", DFS_DEFAULT_SEGMENT_PENALTY, reader.count(), 0);
    DfsSearchStats certificate_only_stats;
    certificate_only.run(&certificate_only_output, &certificate_only_stats);
    check(certificate_only_stats.bounds.mode ==
              DFS_SCORE_BOUND_OFF &&
              certificate_only_stats.certificate.ready,
          "zero score cache disabled the length certificate");
    check_same_spellings(
        expected_spellings,
        certificate_only_output.take_sorted_results(),
        "certificate-only search changed retained spellings");

    check(setenv("NUTRIMATIC_SEARCH_TASKS", "2", 1) == 0,
          "could not lower unbounded parallel task target");
    DfsTopN parallel_unbounded_output(&classes, 2);
    DfsAnagramSearch parallel_unbounded(
        &classes, "aabb", DFS_DEFAULT_SEGMENT_PENALTY, reader.count(), 0, 1, 4);
    DfsSearchStats parallel_unbounded_stats;
    parallel_unbounded.run(&parallel_unbounded_output, &parallel_unbounded_stats);
    check(unsetenv("NUTRIMATIC_SEARCH_TASKS") == 0,
          "could not restore unbounded parallel task target");
    check(parallel_unbounded_stats.bounds.mode ==
              DFS_SCORE_BOUND_OFF &&
              parallel_unbounded_stats.execution.search_threads > 1,
          "score-bound-off search did not run in parallel");
    check_same_spellings(
        expected_spellings,
        parallel_unbounded_output.take_sorted_results(),
        "score-bound-off parallel search changed retained spellings");

    DfsTopN isolated_output(&classes, 2);
    size_t const score_only_budget = 128;
    DfsAnagramSearch isolated(
        &classes, "aabb", DFS_DEFAULT_SEGMENT_PENALTY, reader.count(),
        score_only_budget);
    DfsSearchStats isolated_stats;
    isolated.run(&isolated_output, &isolated_stats);
    check(isolated_stats.bounds.mode ==
              DFS_SCORE_BOUND_PROJECTED,
          "small budget disabled projected score memo");
    check(isolated_stats.bounds.complete,
          "small projected score memo did not retain complete coverage");
    check_same_spellings(
        expected_spellings, isolated_output.take_sorted_results(),
        "small score cache changed retained spellings");

    DfsTopN threaded_output(&classes, 2);
    DfsAnagramSearch threaded(
        &classes, "aabb", DFS_DEFAULT_SEGMENT_PENALTY, reader.count(),
        bound_budget, 4);
    DfsSearchStats threaded_stats;
    threaded.run(&threaded_output, &threaded_stats);
    std::vector<DfsSpelling> const threaded_spellings =
        threaded_output.take_sorted_results();
    check(threaded_stats.execution.preprocess_threads > 1,
          "projected score memo did not use requested preprocessing threads");
    DfsSearchStats::Bounds const& threaded_bounds =
        threaded_stats.bounds;
    DfsSearchStats::Bounds const& bounded_bounds =
        bounded_stats.bounds;
    DfsSearchStats::AllSolutions const& threaded_all =
        threaded_stats.all_solutions;
    DfsSearchStats::AllSolutions const& bounded_all =
        bounded_stats.all_solutions;
    check(threaded_bounds.mode == bounded_bounds.mode,
          "threading changed score-bound mode");
    check(threaded_bounds.entries == bounded_bounds.entries,
          "threading changed score-bound entry count");
    check(threaded_bounds.projected.states_computed ==
              bounded_bounds.projected.states_computed,
          "threading changed computed bound-state count");
    check(threaded_bounds.projected.transitions ==
              bounded_bounds.projected.transitions,
          "threading changed successful bound transitions");
    check(threaded_bounds.projected.nextafter_calls ==
              bounded_bounds.projected.nextafter_calls,
          "threading changed bound nextafter calls");
    check(threaded_all.nodes == bounded_all.nodes,
          "threading changed phase-2 node count");
    check(threaded_all.solutions == bounded_all.solutions,
          "threading changed phase-2 solution count");
    check_same_spellings(
        bounded_spellings, threaded_spellings,
        "threaded score bound changed the retained spellings");

    std::string const exhausted_letters = "aaaaabbbbb";
    DfsTopN exhausted_expected_output(&classes, 2);
    DfsAnagramSearch exhausted_expected(
        &classes, exhausted_letters, DFS_DEFAULT_SEGMENT_PENALTY,
        reader.count(), 0);
    DfsSearchStats exhausted_expected_stats;
    exhausted_expected.run(&exhausted_expected_output, &exhausted_expected_stats);
    std::vector<DfsSpelling> const exhausted_expected_spellings =
        exhausted_expected_output.take_sorted_results();

    DfsTopN projected_output(&classes, 2);
    DfsAnagramSearch projected(
        &classes, exhausted_letters, DFS_DEFAULT_SEGMENT_PENALTY,
        reader.count(), 64, 4);
    FILE* projected_diagnostics = tmpfile();
    check(projected_diagnostics != NULL,
          "could not create projected diagnostic stream");
    FILE* const previous_diagnostic_stream =
        dfs_set_diagnostic_stream(projected_diagnostics);
    DfsSearchStats projected_stats;
    projected.run(&projected_output, &projected_stats,
                  /*progress_factor=*/1, /*allow_cache_fallback=*/true,
                  /*exact_letters=*/0);
    std::string const projected_message =
        read_stream(projected_diagnostics);
    dfs_set_diagnostic_stream(previous_diagnostic_stream);
    fclose(projected_diagnostics);
    check(projected_stats.bounds.mode ==
              DFS_SCORE_BOUND_PROJECTED &&
              projected_stats.bounds.complete,
          "projected score memo did not retain complete coverage");
    check(projected_stats.bounds.exact_letters == 0 &&
              projected_stats.bounds.wild_letters ==
                  exhausted_letters.size(),
          "projected score memo used the wrong abstraction");
    check(projected_stats.bounds.capacity ==
              exhausted_letters.size() + 1,
          "wildcard-only projected score memo has the wrong size");
    check(projected_stats.bounds.projected_actions <
              classes.classes().size(),
          "projected-action quotient did not collapse equivalent classes");
    check(projected_stats.bounds.projected.candidate_tests >=
                  projected_stats.bounds.projected.fitting_transitions &&
              projected_stats.bounds.projected.fitting_transitions >=
                  projected_stats.bounds.projected.transitions,
          "projected work counters are inconsistent");
    check(projected_message.find("projected actions") != std::string::npos,
          "projected-action diagnostic is missing");
    check_same_spellings(
        exhausted_expected_spellings,
        projected_output.take_sorted_results(),
        "projected score memo changed retained spellings");
    // A non-zero exact depth puts the layered bottom-up wildcard-update
    // kernel on the path.
    for (size_t exact = 1; exact <= 2; ++exact) {
      DfsTopN depth_output(&classes, 2);
      DfsAnagramSearch depth(
          &classes, exhausted_letters, DFS_DEFAULT_SEGMENT_PENALTY,
          reader.count(), 4096, 4);
      DfsSearchStats depth_stats;
      depth.run(&depth_output, &depth_stats,
                /*progress_factor=*/1, /*allow_cache_fallback=*/true,
                /*exact_letters=*/int(exact));
      check(depth_stats.bounds.mode ==
                DFS_SCORE_BOUND_PROJECTED &&
                depth_stats.bounds.complete,
            "intermediate projected depth did not retain complete bounds");
      check(depth_stats.bounds.exact_letters == exact,
            "projected score memo used the wrong exact depth");
      // One surviving action contributes a whole wildcard span.
      check(depth_stats.bounds.projected.transitions > 0 &&
                depth_stats.bounds.projected.fitting_transitions >=
                depth_stats.bounds.projected.transitions,
            "projected work counters are inconsistent at depth");
      if (exact == 2)
        check(depth_stats.bounds.projected_actions ==
                  classes.classes().size(),
              "all-exact projection collapsed distinct classes");
      check_same_spellings(
          exhausted_expected_spellings,
          depth_output.take_sorted_results(),
          "projected exact depth changed retained spellings");
    }
    CollectSolutions boundary_expected(&classes);
    DfsAnagramSearch boundary_exhaustive(
        &classes, exhausted_letters, DFS_DEFAULT_SEGMENT_PENALTY,
        reader.count(), 0);
    DfsSearchStats boundary_exhaustive_stats;
    boundary_exhaustive.run(&boundary_expected, &boundary_exhaustive_stats);
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
    std::vector<double> const boundary_floors = {
        nextafter(best_score, -HUGE_VAL),
        best_score,
        nextafter(best_score, HUGE_VAL),
    };
    for (size_t i = 0; i < boundary_floors.size(); ++i) {
      check(setenv(
                "NUTRIMATIC_LENGTH_CERTIFICATE", "0", 1) == 0,
            "could not disable boundary length certificate");
      FixedFloorSolutions disabled_boundary_output(
          boundary_floors[i]);
      DfsAnagramSearch disabled_boundary(
          &classes, exhausted_letters, DFS_DEFAULT_SEGMENT_PENALTY,
          reader.count(), 128);
      DfsSearchStats disabled_boundary_stats;
      disabled_boundary.run(&disabled_boundary_output, &disabled_boundary_stats);
      check(unsetenv("NUTRIMATIC_LENGTH_CERTIFICATE") == 0,
            "could not restore boundary length certificate");

      FixedFloorSolutions boundary_output(boundary_floors[i]);
      DfsAnagramSearch boundary(
          &classes, exhausted_letters, DFS_DEFAULT_SEGMENT_PENALTY,
          reader.count(), 128);
      DfsSearchStats boundary_stats;
      boundary.run(&boundary_output, &boundary_stats);
      check(boundary_stats.bounds.mode ==
                DFS_SCORE_BOUND_PROJECTED &&
                boundary_stats.bounds.value_bytes == sizeof(float) &&
                boundary_stats.bounds.complete,
            "rounding-boundary test did not use complete float bounds");
      check(boundary_output.ordered_indexes ==
                    disabled_boundary_output.ordered_indexes &&
                boundary_output.ordered_scores ==
                    disabled_boundary_output.ordered_scores,
            "length certificate changed a boundary-floor result");
      if (i == 0)
        check(std::find(
                  boundary_output.ordered_indexes.begin(),
                  boundary_output.ordered_indexes.end(),
                  best_path) != boundary_output.ordered_indexes.end(),
              "one-ulp score floor pruned the best deep solution");
    }

    int const original_rounding = fegetround();
    if (original_rounding != -1 && fesetround(FE_DOWNWARD) == 0) {
      DfsTopN downward_output(&classes, 2);
      DfsAnagramSearch downward(
          &classes, exhausted_letters, DFS_DEFAULT_SEGMENT_PENALTY,
          reader.count(), 768);
      DfsSearchStats downward_stats;
      downward.run(&downward_output, &downward_stats);
      check(downward_stats.bounds.mode ==
                DFS_SCORE_BOUND_OFF,
            "score memo ignored a non-nearest rounding mode");
      check(!downward_stats.certificate.ready,
            "length certificate ignored a non-nearest rounding mode");
      check(fesetround(original_rounding) == 0,
            "could not restore floating-point rounding mode");
    }

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
        &classes, letters, DFS_DEFAULT_SEGMENT_PENALTY, reader.count(), 0);
    DfsSearchStats exhaustive_stats;
    exhaustive.run(&expected_output, &exhaustive_stats);
    std::vector<DfsSpelling> const expected_spellings =
        expected_output.take_sorted_results();

    DfsTopN output(&classes, 1);
    size_t const budget = 512;
    DfsAnagramSearch search(
        &classes, letters, DFS_DEFAULT_SEGMENT_PENALTY, reader.count(), budget);
    DfsSearchStats search_stats;
    search.run(&output, &search_stats);
    check(search_stats.bounds.mode ==
              DFS_SCORE_BOUND_PROJECTED,
          "complete float score memo was not selected");
    check(search_stats.bounds.value_bytes == sizeof(float) &&
              search_stats.bounds.capacity == 128 &&
              search_stats.bounds.complete,
          "complete float score memo has the wrong layout");
    check(search_stats.bounds.bytes_charged <= budget,
          "float score cache exceeded its budget");
    check(output.size() == 1, "float score bound lost its solution");
    check_same_spellings(
        expected_spellings, output.take_sorted_results(),
        "float score bound changed the retained spellings");
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
  DfsAnagramSearch words_search(
      &words, letters, DFS_DEFAULT_SEGMENT_PENALTY, reader.count());
  DfsSearchStats words_search_stats;
  words_search.run(NULL, &words_search_stats);
  check_count(words_search_stats.all_solutions.solutions, 27177,
              "words-only solution count");
  check_count(words_search_stats.all_solutions.nodes, 117145,
              "words-only node count");

  DfsClassList with_phrases(&reader, letters, 4);
  check_count(with_phrases.entry_count(), 18299,
              "phrase-inclusive entry count");
  check_count(with_phrases.classes().size(), 2760,
              "phrase-inclusive class count");
  DfsAnagramSearch phrase_search(
      &with_phrases, letters, DFS_DEFAULT_SEGMENT_PENALTY, reader.count());
  DfsSearchStats phrase_search_stats;
  phrase_search.run(NULL, &phrase_search_stats);
  check_count(phrase_search_stats.all_solutions.solutions, 27401,
              "phrase-inclusive solution count");
  check_count(phrase_search_stats.all_solutions.nodes, 118311,
              "phrase-inclusive node count");

  fclose(fp);
  return 0;
}

int main(int argc, char* argv[]) {
  if (argc == 1) {
    smoke_test();
    float_score_bound_test();
    return 0;
  }
  if (argc == 2 && strcmp(argv[1], "--validate-14") == 0)
    return validate_14_letters();
  fprintf(stderr, "usage: %s [--validate-14]\n", argv[0]);
  return 2;
}
