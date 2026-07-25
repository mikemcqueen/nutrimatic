#ifndef NUTRIMATIC_DFS_SEARCH_H
#define NUTRIMATIC_DFS_SEARCH_H

#include <stdio.h>
#include <stdint.h>

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "dfs-class-list.h"

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
                   double restart, int64_t corpus_total,
                   size_t score_cache_bytes = 0,
                   size_t preprocess_threads = 1);

  // A null sink runs the search as a counter. Statistics are reset on each run.
  // When progress is non-null, report every 100k * progress_factor nodes.
  // When cache fallback is disallowed, return false instead of using a partial
  // score table when the configured cache cannot hold the complete table.
  bool run(DfsSolutionSink* sink, FILE* progress = NULL,
           int progress_factor = 1, bool allow_cache_fallback = true);

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
  bool score_bound_projected_quotient_enabled() const {
    return projected_quotient_enabled;
  }
  int64_t score_bound_prunes() const { return bound_prunes; }
  double phase_two_setup_seconds() const { return setup_seconds; }
  double phase_two_search_seconds() const { return search_seconds; }
  size_t preprocess_threads_used() const {
    return actual_preprocess_threads;
  }

 private:
  struct AlignedFree {
    void operator()(void* pointer) const;
  };

  struct AtomicWord {
    std::atomic<uint64_t> value;
  };

  struct AtomicFloatWord {
    std::atomic<uint32_t> value;
  };

  struct alignas(16) FitClass {
    uint64_t support_mask;
    uint32_t letters_offset;
    uint32_t packed_length_and_count;
  };

  struct alignas(16) ProjectedAction {
    uint64_t exact_support_mask;
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

  bool prepare_hot_classes();
  bool prepare_projected_actions();
  void prepare_score_bounds(uint64_t state_count, DfsSolutionSink* sink);
  void clear_score_bounds();

  void walk(size_t letters_left, size_t entry_point,
            double representative_log_score, DfsSolutionSink* sink);
  void walk_unoptimized(size_t letters_left, int old_rarest_rank,
                        size_t entry_point, double representative_log_score,
                        DfsSolutionSink* sink);

  bool hot_class_fits(uint32_t class_index) const;
  bool hot_class_multiplicity_fits(uint32_t class_index) const;
  bool hot_class_fits(uint32_t class_index,
                      BoundWorker const& worker) const;
  bool hot_class_multiplicity_fits(
      uint32_t class_index, BoundWorker const& worker) const;
  bool projected_action_fits(
      ProjectedAction const& action, BoundWorker const& worker) const;
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
  bool compute_score_bound_parallel(
      size_t requested_threads, FILE* progress);
  double compute_projected_score_bound(BoundWorker* worker);
  void consider_projected_bound_candidate(
      ProjectedAction const& action, BoundWorker* worker, double* best,
      double* max_rounding_error);
  bool compute_projected_score_bound_parallel(
      size_t requested_threads, FILE* progress);
  bool load_score_bound(uint64_t key, double* value) const;
  bool store_score_bound(uint64_t key, double value);
  void publish_parallel_score_bound(uint64_t key, double value);
  bool should_prune(double representative_log_score,
                    DfsSolutionSink* sink);

  void visit_fitting_class(uint32_t class_index, size_t letters_left,
                           double representative_log_score,
                           DfsSolutionSink* sink);
  void visit_unoptimized_class(size_t class_index, size_t letters_left,
                               int rank, double representative_log_score,
                               DfsSolutionSink* sink);

  DfsClassList const* const class_list;
  std::string const letters;
  double const restart_log_rate;
  std::vector<double> best_member_log_scores;
  size_t const max_depth;
  size_t const score_cache_budget;
  size_t const requested_preprocess_threads;

  // The hot bag and all masks use rarest-rank order.
  std::array<uint32_t, DFS_SYMBOL_COUNT> bag;
  std::array<uint64_t, DFS_SYMBOL_COUNT> score_multipliers;
  uint64_t bag_mask;
  uint64_t current_score_key;
  size_t current_letters_left;
  uint64_t score_exact_mask;
  uint64_t score_state_count;
  uint64_t score_effective_states;
  size_t score_exact_letters;
  size_t score_wild_letters;
  size_t score_wild_span;
  bool score_projection_requested;

  std::unique_ptr<FitClass, AlignedFree> fit_classes;
  std::unique_ptr<uint64_t, AlignedFree> score_key_deltas;
  std::unique_ptr<uint16_t, AlignedFree> score_wild_lengths;
  std::unique_ptr<uint32_t, AlignedFree> packed_letters;
  std::vector<ProjectedAction> projected_actions;
  std::vector<uint32_t> projected_repeated_requirements;
  std::array<size_t, DFS_SYMBOL_COUNT + 2> projected_bucket_starts;
  bool projected_actions_ready;
  bool projected_quotient_enabled;
  bool hot_classes_ready;

  ScoreBoundMode bound_mode;
  std::unique_ptr<AtomicWord, AlignedFree> bound_values;
  std::unique_ptr<AtomicFloatWord, AlignedFree> bound_float_values;
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

  std::vector<size_t> path;
  FILE* progress_stream;
  int64_t progress_interval;
  int64_t next_progress;
  int64_t nodes;
  int64_t solutions;
  double setup_seconds;
  double search_seconds;
  size_t actual_preprocess_threads;
};

#endif
