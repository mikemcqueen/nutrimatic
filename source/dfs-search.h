#ifndef NUTRIMATIC_DFS_SEARCH_H
#define NUTRIMATIC_DFS_SEARCH_H

#include <stdio.h>
#include <stdint.h>

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "dfs-alloc.h"
#include "dfs-class-list.h"
#include "dfs-score.h"

// Receives each phase-2 solution as a canonical list of indexes into
// DfsClassList::classes(). The path storage is owned by the search and is only
// valid for the duration of emit().
class DfsSolutionSink {
 public:
  virtual void emit(std::vector<size_t> const& class_indexes,
                    double representative_log_score) = 0;

  // Score-aware sinks may expose a floor once they are full. Search uses this
  // only with an admissible upper bound on every completion below a DFS node.
  virtual bool supports_score_pruning() const { return false; }
  virtual bool score_floor(double* floor) const { return false; }

  // Parallel search shares one sink between workers. Sinks may opt in only
  // when emit() and score_floor() are safe to call concurrently.
  virtual bool supports_parallel_search() const { return false; }

  // Search checks this after every emitted solution at each recursion level
  // and unwinds as soon as it returns true. Default false preserves today's
  // exhaustive enumeration for every existing sink.
  virtual bool should_stop() const { return false; }

  virtual ~DfsSolutionSink() { }
};

// Phase 2 of dfs-anagrams: subtract whole anagram classes from the input bag.
// The rarest remaining symbol selects one class bucket at each node, while an
// entry-point tie-break collapses permutations when that symbol occurs more
// than once.
class DfsAnagramSearch {
 public:
  enum ScoreBoundMode {
    SCORE_BOUND_OFF,
    SCORE_BOUND_DENSE,
    SCORE_BOUND_PREFIX,
    SCORE_BOUND_PROJECTED,
  };

  DfsAnagramSearch(DfsClassList const* classes, std::string const& letters,
                   double segment_penalty, int64_t corpus_total,
                   size_t score_cache_bytes = 0,
                   size_t preprocess_threads = 1,
                   size_t search_threads = 1,
                   double word_bonus = 0.0);

  // A null sink runs the search as a counter. Statistics are reset on each run.
  // When the ambient diagnostic stream (dfs_set_diagnostic_stream()) is set,
  // report every 100k * progress_factor nodes. dense_cache selects exact
  // dense bounds; false selects projected dense bounds. A nonnegative
  // exact_letters fixes the number of exact letters in the projection; a
  // negative value selects the largest depth that fits. When cache fallback
  // is disallowed, return false instead of using a weaker mode when the
  // requested table does not fit. verbose reports serial task splitting to
  // the diagnostic stream when parallel search is selected.
  bool run(DfsSolutionSink* sink,
           int64_t progress_factor = 1, bool allow_cache_fallback = true,
           bool dense_cache = true, int exact_letters = -1,
           bool verbose = false);

  // Tests every phase-1 class against one shared phase-2 preparation. The
  // result is index-parallel to DfsClassList::classes() and is exact even when
  // a projected score bound merges letter identities. Candidate classes are
  // validated using the constructor's requested search-thread count.
  bool find_completable_classes(
      std::vector<bool>* completable, int64_t progress_factor = 1,
      bool allow_cache_fallback = false, bool dense_cache = false,
      int exact_letters = -1);

  int64_t nodes_visited() const { return nodes; }
  int64_t solutions_found() const { return solutions; }
  ScoreBoundMode score_bound_mode() const { return bound_mode; }
  size_t score_bound_entries() const { return bound_entries; }
  size_t score_bound_states_computed() const {
    return bound_states_computed;
  }
  uint64_t score_bound_transitions() const {
    return bound_transitions;
  }
  uint64_t score_bound_candidate_tests() const {
    return bound_candidate_tests;
  }
  uint64_t score_bound_fitting_transitions() const {
    return bound_fitting_transitions;
  }
  uint64_t score_bound_nextafter_calls() const {
    return bound_nextafter_calls;
  }
  size_t score_bound_bytes_charged() const { return bound_charged_bytes; }
  size_t score_bound_capacity() const { return bound_capacity; }
  size_t score_bound_value_bytes() const { return bound_value_bytes; }
  bool score_bound_complete() const { return bound_complete; }
  size_t score_bound_exact_letters() const {
    return score_exact_letters;
  }
  size_t score_bound_wild_letters() const {
    return score_wild_letters;
  }
  size_t score_bound_projected_actions() const {
    return projected_actions.size();
  }
  int64_t score_bound_prunes() const { return bound_prunes; }
  size_t completable_classes_checked() const {
    return completable_checked;
  }
  size_t completable_bound_rejects() const {
    return completable_bound_reject_count;
  }
  size_t completable_exact_bound_accepts() const {
    return completable_exact_bound_accept_count;
  }
  size_t completable_exact_validations() const {
    return completable_exact_validation_count;
  }
  size_t exact_memo_states_computed() const {
    return exact_memo_states;
  }
  size_t exact_memo_hits() const { return exact_memo_hit_count; }

  // Test hook: one projected AVX2 wildcard update over `count` contiguous
  // wildcard counts, exactly as the bottom-up evaluator performs it.
  static uint64_t test_projected_wild_update(
      double partial_score, double rounding_error_base,
      float const* children, double* best, double* max_rounding_error,
      size_t count);

  double phase_two_setup_seconds() const { return setup_seconds; }
  double phase_two_search_seconds() const { return search_seconds; }
  size_t preprocess_threads_used() const {
    return actual_preprocess_threads;
  }
  size_t search_threads_used() const {
    return actual_search_threads;
  }
  uint64_t search_tasks_generated() const {
    return search_tasks_created;
  }
  bool length_certificate_enabled() const {
    return length_certificate_ready;
  }
  bool length_certificate_skipping() const {
    return length_certificate_ready && !length_certificate_shadow;
  }
  uint64_t length_certificate_group_tests() const {
    return certificate_group_tests;
  }
  uint64_t length_certificate_group_rejects() const {
    return certificate_group_rejects;
  }
  uint64_t length_certificate_scans_skipped() const {
    return certificate_scans_skipped;
  }
  uint64_t length_certificate_scans_kept() const {
    return certificate_scans_kept;
  }
  size_t length_certificate_table_bytes() const {
    return certificate_max_score.size() * sizeof(double) +
        certificate_group_end.size() * sizeof(uint32_t) +
        length_tail_bounds.size() * sizeof(double);
  }

 private:
  static size_t const MAX_SPLIT_DEPTH = 6;

  struct AtomicWord {
    std::atomic<uint64_t> value;
  };

  struct AtomicFloatWord {
    std::atomic<uint32_t> value;
  };

  struct FitClassMetadata {
    uint32_t letters_offset;
    uint32_t packed_length_and_count;
  };

  struct alignas(16) FitClass {
    FitClassMetadata metadata;
    uint64_t support_mask;
  };

  struct alignas(16) ProjectedAction {
    uint64_t score_key_delta;
    double partial_score;
    double rounding_error_base;
    uint32_t repeated_offset;
    uint32_t packed_lengths;
    uint32_t repeated_count;
  };

  struct BoundWorker {
    std::array<uint32_t, DFS_SYMBOL_COUNT> bag;
    uint64_t bag_mask;
    uint64_t score_key;
    size_t letters_left;
    size_t wild_left;
    size_t states_computed;
    uint64_t candidate_tests;
    uint64_t fitting_transitions;
    uint64_t transitions;
    uint64_t nextafter_calls;
    double best;
    double max_rounding_error;
  };

  struct SearchTask {
    std::array<uint32_t, DFS_SYMBOL_COUNT> bag;
    uint64_t bag_mask;
    uint64_t score_key;
    std::array<uint32_t, MAX_SPLIT_DEPTH> path;
    uint32_t path_size;
    uint32_t entry_point;
    uint32_t letters_left;
    double representative_log_score;
  };

  struct SearchWorker {
    std::array<uint32_t, DFS_SYMBOL_COUNT> bag;
    uint64_t bag_mask;
    uint64_t score_key;
    // Phase-1's exact mixed-radix signature, maintained incrementally for
    // completability memoization. Unlike score_key, it never projects letters.
    uint64_t exact_key;
    std::vector<size_t> path;
    int64_t nodes;
    int64_t solutions;
    int64_t bound_prunes;
    uint64_t certificate_group_tests;
    uint64_t certificate_group_rejects;
    uint64_t certificate_scans_skipped;
    uint64_t certificate_scans_kept;
    size_t split_depth;
    std::vector<SearchTask>* produced;
    int64_t next_progress;
    int64_t reported_solutions;
    size_t exact_memo_states;
    size_t exact_memo_hits;
    uint64_t exact_lookahead_full_windows;
    uint64_t exact_lookahead_known_true_wins;
    uint64_t exact_lookahead_reprobes_decided;
    uint64_t exact_lookahead_recursive_expansions;
#if 1
    // dummy padding somehow significantly decreases runtime of:
    // build/dfs-anagrams $IDX "${S1:0:50}" -m 4 -n 1000 -S 20  -d 15
    std::array<uint64_t, 46> dummy1;
#endif
  };

  enum Reachability {
    REACHABILITY_UNKNOWN,
    REACHABILITY_NO,
    REACHABILITY_YES,
  };

  enum ExactResultSource {
    EXACT_RESULT_EMPTY,
    EXACT_RESULT_MEMO,
    EXACT_RESULT_BOUND_NO,
    EXACT_RESULT_BOUND_YES,
    EXACT_RESULT_SEARCH,
  };

  enum ExactChildResult {
    EXACT_CHILD_FALSE,
    EXACT_CHILD_TRUE,
    EXACT_CHILD_UNKNOWN,
  };

  bool prepare_phase_two(
      int64_t progress_factor, bool allow_cache_fallback, bool dense_cache,
      int exact_letters, bool score_bounds_requested);
  void require_hot_classes() const;
  bool prepare_hot_classes();
  bool prepare_projected_actions();
  bool prepare_length_certificate();
  bool length_certificate_rejects(
      size_t base, size_t length, size_t letters_left,
      double representative_log_score, double floor) const;
  void prepare_score_bounds(uint64_t state_count, bool requested);
  void clear_score_bounds();
  Reachability cached_reachability(
      uint64_t score_key, bool original_root) const;
  bool exact_remainder_completable(
      SearchWorker* worker, size_t letters_left,
      ExactResultSource* source = NULL);
  ExactChildResult classify_exact_child(
      SearchWorker* worker, uint32_t class_index,
      size_t candidate_length, size_t letters_left);
  // The tail of exact_remainder_completable, entered directly when the caller
  // has already probed the memo and the score bound for this exact key.
  bool exact_expand_node(SearchWorker* worker, size_t letters_left);
  bool exact_candidates_immediate(
      SearchWorker* worker, size_t letters_left);
  bool exact_candidates_lookahead(
      SearchWorker* worker, size_t letters_left);
  bool exact_buffered_candidates(
      SearchWorker* worker, size_t letters_left,
      uint32_t const* class_ids, size_t count);
  bool exact_memo_lookup(
      SearchWorker* worker, uint64_t exact_key, bool* value);
  void exact_memo_store(
      SearchWorker* worker, uint64_t exact_key, bool value);
  bool exact_class_fits(
      size_t class_index, SearchWorker const& worker) const;
  void subtract_exact_class(
      size_t class_index, SearchWorker* worker,
      uint64_t* parent_bag_mask);
  void restore_exact_class(
      size_t class_index, SearchWorker* worker,
      uint64_t parent_bag_mask);

  void walk(SearchWorker* worker, size_t letters_left,
            size_t entry_point, double representative_log_score,
            DfsSolutionSink* sink);
  void walk_certified(SearchWorker* worker, int rank, size_t start,
                      size_t end, size_t letters_left,
                      double representative_log_score, double floor,
                      DfsSolutionSink* sink);
  bool visit_fitting_range(SearchWorker* worker, size_t begin, size_t end,
                           size_t letters_left,
                           double representative_log_score,
                           DfsSolutionSink* sink);
  void walk_unoptimized(size_t letters_left, int old_rarest_rank,
                        size_t entry_point, double representative_log_score,
                        DfsSolutionSink* sink);
  void start_search_worker(SearchWorker* worker);
  void report_search_progress(SearchWorker* worker);
  void merge_search_worker(SearchWorker const& worker);
  bool run_parallel_search(
      DfsSolutionSink* sink, size_t threads, size_t target_tasks,
      uint64_t task_progress_factor, bool verbose);

  bool hot_class_fits(uint32_t class_index) const;
  bool hot_class_multiplicity_fits(uint32_t class_index) const;
  bool hot_class_fits(uint32_t class_index,
                      SearchWorker const& worker) const;
  bool hot_class_multiplicity_fits(
      uint32_t class_index, SearchWorker const& worker) const;
  bool hot_class_multiplicity_fits(
      FitClassMetadata metadata, SearchWorker const& worker) const;
  bool hot_class_fits(uint32_t class_index,
                      BoundWorker const& worker) const;
  bool hot_class_multiplicity_fits(
      uint32_t class_index, BoundWorker const& worker) const;
  bool projected_action_fits(
      size_t action_index, BoundWorker const& worker) const;
  size_t first_length_candidate(
      size_t begin, size_t end, size_t letters_left) const;
  size_t first_projected_length_candidate(
      size_t begin, size_t end, size_t letters_left) const;
  double compute_score_bound();
  void consider_bound_candidate(uint32_t class_index, double* best,
                                double* max_rounding_error);
  double compute_parallel_score_bound(BoundWorker* worker);
  void consider_parallel_bound_candidate(
      uint32_t class_index, BoundWorker* worker, double* best,
      double* max_rounding_error);
  bool compute_score_bound_parallel(size_t requested_threads);
  double compute_projected_score_bound(BoundWorker* worker);
  void consider_projected_bound_candidate(
      size_t action_index, BoundWorker* worker, double* best,
      double* max_rounding_error);
  bool compute_projected_score_bound_parallel(size_t requested_threads);
  bool compute_projected_score_bound_bottom_up(size_t requested_threads);
  bool load_score_bound(uint64_t key, double* value) const;
  bool store_score_bound(uint64_t key, double value);
  void publish_parallel_score_bound(uint64_t key, double value);
  bool should_prune(SearchWorker* worker,
                    double representative_log_score,
                    DfsSolutionSink* sink, size_t letters_left);

  void visit_fitting_class(SearchWorker* worker, uint32_t class_index,
                           FitClassMetadata metadata, size_t letters_left,
                           double representative_log_score,
                           DfsSolutionSink* sink);
  void visit_unoptimized_class(size_t class_index, size_t letters_left,
                               int rank, double representative_log_score,
                               DfsSolutionSink* sink);

  DfsClassList const* const class_list;
  std::string const letters;
  DfsScoreModel const score_model;
  double const segment_boundary_log_score;
  std::vector<double> best_member_log_scores;
  size_t const max_depth;
  size_t const score_cache_budget;
  size_t const requested_preprocess_threads;
  size_t const requested_search_threads;

  // The hot bag and all masks use rarest-rank order.
  std::array<uint32_t, DFS_SYMBOL_COUNT> bag;
  std::array<uint64_t, DFS_SYMBOL_COUNT> score_multipliers;
  uint64_t bag_mask;
  uint64_t current_score_key;
  uint64_t exact_root_key;
  size_t current_letters_left;
  uint64_t score_exact_mask;
  uint64_t score_state_count;
  uint64_t score_effective_states;
  size_t score_exact_letters;
  size_t score_wild_letters;
  size_t score_wild_span;
  bool score_projection_requested;

  std::unique_ptr<FitClass, DfsAlignedFree> fit_classes;
  // The exact scan rejects ~99.98% of candidates on the support mask alone,
  // so the masks also live in their own contiguous array: 8 bytes per stride
  // instead of FitClass's 16, and vector-loadable four at a time.
  std::unique_ptr<uint64_t, DfsAlignedFree> class_supports;
  std::unique_ptr<uint64_t, DfsAlignedFree> score_key_deltas;
  std::unique_ptr<uint16_t, DfsAlignedFree> score_wild_lengths;
  std::unique_ptr<uint32_t, DfsAlignedFree> packed_letters;
  std::vector<ProjectedAction> projected_actions;
  // Index-parallel to projected_actions, and the only copy of an action's
  // exact-support mask: the bottom-up scan rejects most actions on it alone,
  // without touching the cold record. Built in one place, strictly after the
  // per-bucket sort.
  std::vector<uint64_t> projected_action_support;
  std::vector<uint32_t> projected_repeated_requirements;
  std::array<size_t, DFS_SYMBOL_COUNT + 2> projected_bucket_starts;
  bool projected_actions_ready;
  // Resolved in the constructor, before any worker starts, so no dispatch
  // happens on the scan path itself.
  bool const support_scan_vector;
  bool hot_classes_ready;
  // A bag with no classes has no results, which is not the same thing as a bag
  // phase 2 cannot prepare; only the latter aborts.
  bool empty_class_list;
  // Which of prepare_hot_classes()'s failure paths fired, for the abort
  // message. NULL until one does.
  char const* unsupported_reason;
  bool length_certificate_requested;
  bool length_certificate_shadow;
  bool length_certificate_ready;
  size_t certificate_stride;
  std::vector<double> certificate_max_score;
  std::vector<uint32_t> certificate_group_end;
  std::vector<double> length_tail_bounds;
  uint64_t certificate_group_tests;
  uint64_t certificate_group_rejects;
  uint64_t certificate_scans_skipped;
  uint64_t certificate_scans_kept;

  ScoreBoundMode bound_mode;
  // With bounds off cached_reachability answers UNKNOWN for every key, so the
  // exact recurrence can skip maintaining and probing score keys entirely.
  bool score_bounds_active() const { return bound_mode != SCORE_BOUND_OFF; }
  std::unique_ptr<AtomicWord, DfsAlignedFree> bound_values;
  std::unique_ptr<AtomicFloatWord, DfsAlignedFree> bound_float_values;
  std::unique_ptr<float, DfsAlignedFree> bound_plain_float_values;
  size_t bound_capacity;
  size_t bound_value_bytes;
  bool bound_complete;
  double root_score_bound;
  bool root_score_bound_ready;
  size_t bound_entries;
  size_t bound_states_computed;
  uint64_t bound_candidate_tests;
  uint64_t bound_fitting_transitions;
  uint64_t bound_transitions;
  uint64_t bound_nextafter_calls;
  size_t bound_charged_bytes;
  int64_t bound_prunes;

  // Packs the exact key and boolean into one atomic open-addressed slot.
  // Completability setup fails rather than falling back to an unbounded map
  // when the packed key, table size, or allocation does not fit.
  std::unique_ptr<AtomicWord, DfsAlignedFree> exact_flat_memo;
  size_t exact_flat_capacity;
  size_t exact_flat_entry_limit;
  std::atomic<size_t> exact_flat_entries;
  size_t completable_checked;
  size_t completable_bound_reject_count;
  size_t completable_exact_bound_accept_count;
  size_t completable_exact_validation_count;
  size_t exact_memo_states;
  size_t exact_memo_hit_count;
  size_t exact_memo_lookahead;
  uint64_t exact_lookahead_full_windows;
  uint64_t exact_lookahead_known_true_wins;
  uint64_t exact_lookahead_reprobes_decided;
  uint64_t exact_lookahead_recursive_expansions;

  std::vector<size_t> path;
  bool progress_enabled;
  int64_t progress_interval;
  int64_t next_progress;
  int64_t nodes;
  int64_t solutions;
  double setup_seconds;
  double search_seconds;
  size_t actual_preprocess_threads;
  size_t actual_search_threads;
  uint64_t search_tasks_created;
  std::atomic<int64_t> progress_nodes;
  std::atomic<int64_t> progress_solutions;
};

#endif
