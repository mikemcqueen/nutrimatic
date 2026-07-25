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
  static size_t const MAX_MODULAR_BOUND_COUNT = 8;

  struct ProjectedDiagnostics {
    uint64_t action_scans;
    uint64_t wild_length_rejects;
    uint64_t support_rejects;
    uint64_t multiplicity_rejects;
    uint64_t fitting_edges;
    uint64_t dead_child_edges;
    uint64_t ready_child_hits;
    uint64_t states_claimed;
    uint64_t ownership_conflicts;
    uint64_t dependency_spins;
    uint64_t finite_states;
    uint64_t dead_states;
    uint64_t coarse_certificate_checks;
    uint64_t coarse_certificate_edges;
    uint64_t coarse_certificate_skips;
    uint64_t certificate_fallback_queries;
    uint64_t certificate_fallback_unique_keys;
    uint64_t certificate_fallback_prunes;
    uint64_t final_queries_without_floor;
    uint64_t final_bound_queries;
    uint64_t final_unique_bound_keys;
    uint64_t final_bound_prunes;
    uint64_t final_length_bound_prunes;
    uint64_t final_rich_only_vs_length_prunes;
    uint64_t final_length_only_prunes;
    uint64_t final_modular_bound_prunes;
    uint64_t final_modular_rich_only_prunes;
    uint64_t final_modular_only_prunes;
    std::array<uint64_t, MAX_MODULAR_BOUND_COUNT>
        final_modular_prefix_bound_prunes;
    std::array<uint64_t, MAX_MODULAR_BOUND_COUNT>
        final_modular_prefix_rich_only_prunes;
    std::array<uint64_t, MAX_MODULAR_BOUND_COUNT>
        final_modular_prefix_only_prunes;
  };

  struct ProjectedLayerDiagnostics {
    uint64_t outgoing_fitting_edges;
    uint64_t incoming_dead_child_edges;
    uint64_t finite_states;
    uint64_t dead_states;
  };

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
  size_t projected_action_count() const {
    return projected_action_offsets[DFS_SYMBOL_COUNT + 1];
  }
  uint64_t projected_logical_fitting_edges() const {
    return projected_dense_fitting_edges;
  }
  size_t projected_reverse_perimeter_letters() const {
    return projected_reverse_perimeter_depth;
  }
  size_t projected_reverse_perimeter_states() const {
    return projected_reverse_perimeter_state_count;
  }
  int64_t score_bound_prunes() const { return bound_prunes; }
  double phase_two_setup_seconds() const { return setup_seconds; }
  double phase_two_search_seconds() const { return search_seconds; }
  double projected_support_group_prepare_seconds() const {
    return support_group_prepare_seconds;
  }
  size_t preprocess_threads_used() const {
    return actual_preprocess_threads;
  }
  bool projected_diagnostics_enabled() const {
    return projected_diagnostics_requested;
  }
  bool projected_query_diagnostics_enabled() const {
    return projected_query_diagnostics_requested;
  }
  size_t projected_modular_bound_bits() const {
    return modular_bound_bits;
  }
  size_t projected_modular_bound_count() const {
    return modular_bound_count;
  }
  uint32_t projected_modular_bound_seed() const {
    return modular_bound_seed;
  }
  size_t projected_modular_bound_table_bytes() const {
    return modular_bound_count * projected_modular_bound_span *
        (letters.size() + 1) * sizeof(double);
  }
  size_t projected_modular_bound_delta_bytes() const {
    return projected_modular_class_deltas8.size() *
            sizeof(uint8_t) +
        projected_modular_class_deltas16.size() *
            sizeof(uint16_t);
  }
  double projected_modular_bound_prepare_seconds() const {
    return modular_bound_prepare_seconds;
  }
  size_t projected_modular_bound_actions(size_t table) const {
    return modular_bound_action_counts[table];
  }
  uint64_t projected_modular_bound_candidate_scans(
      size_t table) const {
    return modular_bound_candidate_scans[table];
  }
  ProjectedDiagnostics const& projected_diagnostics() const {
    return projected_diagnostic_counts;
  }
  std::vector<ProjectedLayerDiagnostics> const&
  projected_layer_diagnostics() const {
    return projected_layer_diagnostic_counts;
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

  struct alignas(16) ProjectedFit {
    uint64_t exact_support_mask;
    uint32_t requirements_offset;
    uint16_t wild_length;
    uint8_t requirement_count;
    uint8_t repeated_count;
  };

  struct alignas(16) ProjectedValue {
    uint64_t score_delta;
    double class_score;
  };

  struct BoundWorker {
    std::array<uint32_t, DFS_SYMBOL_COUNT> bag;
    uint64_t bag_mask;
    uint64_t projected_support_key;
    uint64_t score_key;
    size_t letters_left;
    size_t wild_left;
    size_t states_computed;
    uint64_t transitions;
    uint64_t nextafter_calls;
    double best;
    double lower_best;
    double max_rounding_error;
    ProjectedDiagnostics projected_diagnostics;
    std::vector<ProjectedLayerDiagnostics> projected_layer_diagnostics;
  };

  bool prepare_hot_classes();
  bool prepare_projected_actions();
  bool prepare_projected_support_groups();
  bool prepare_projected_length_bounds();
  bool prepare_projected_modular_bounds();
  uint16_t projected_modular_class_delta(
      size_t class_index, size_t table) const;
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
      uint32_t action_index, BoundWorker const& worker) const;
  bool diagnose_projected_action_fit(
      uint32_t action_index, BoundWorker* worker) const;
  bool projected_grouped_action_fits(
      uint32_t action_index, BoundWorker const& worker) const;
  bool diagnose_projected_grouped_action_fit(
      uint32_t action_index, BoundWorker* worker) const;
  size_t first_length_candidate(
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
      uint32_t action_index, BoundWorker* worker, double* best,
      double* lower_best, double* max_child_magnitude);
  bool compute_projected_score_bound_parallel(
      size_t requested_threads, FILE* progress);
  bool load_score_bound(uint64_t key, double* value) const;
  bool store_score_bound(uint64_t key, double value);
  void publish_parallel_score_bound(uint64_t key, double value);
  bool should_prune(double representative_log_score,
                    DfsSolutionSink* sink, size_t letters_left);

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
  std::array<uint64_t, DFS_SYMBOL_COUNT> score_support_bits;
  uint64_t bag_mask;
  uint64_t current_score_key;
  size_t current_letters_left;
  uint64_t score_exact_mask;
  uint64_t score_state_count;
  uint64_t score_effective_states;
  size_t score_exact_letters;
  size_t score_wild_letters;
  size_t score_wild_span;
  double projected_max_class_score_magnitude;
  uint64_t projected_dense_fitting_edges;
  size_t projected_reverse_perimeter_depth;
  size_t projected_reverse_perimeter_state_count;
  bool score_projection_requested;
  bool projected_diagnostics_requested;
  bool projected_certificate_diagnostics_requested;
  bool projected_certificate_prune_requested;
  bool projected_certificate_fallback_requested;
  bool projected_query_diagnostics_requested;
  bool projected_support_groups_requested;

  std::unique_ptr<FitClass, AlignedFree> fit_classes;
  std::unique_ptr<uint64_t, AlignedFree> score_key_deltas;
  std::unique_ptr<uint16_t, AlignedFree> score_wild_lengths;
  std::unique_ptr<uint32_t, AlignedFree> packed_letters;
  std::unique_ptr<ProjectedFit, AlignedFree> projected_fits;
  std::unique_ptr<ProjectedValue, AlignedFree> projected_values;
  std::unique_ptr<uint32_t, AlignedFree> projected_requirements;
  std::array<size_t, DFS_SYMBOL_COUNT + 2> projected_action_offsets;
  std::vector<size_t> projected_support_offsets;
  std::vector<uint32_t> projected_support_actions;
  std::vector<float> projected_lower_bounds;
  std::vector<uint64_t> projected_query_bits;
  std::vector<double> projected_length_bounds;
  size_t modular_bound_bits;
  size_t modular_bound_count;
  size_t projected_modular_bound_span;
  uint32_t modular_bound_seed;
  std::array<uint16_t, MAX_MODULAR_BOUND_COUNT>
      current_modular_signatures;
  std::vector<uint8_t> projected_modular_class_deltas8;
  std::vector<uint16_t> projected_modular_class_deltas16;
  std::array<std::vector<double>, MAX_MODULAR_BOUND_COUNT>
      projected_modular_bounds;
  std::array<size_t, MAX_MODULAR_BOUND_COUNT>
      modular_bound_action_counts;
  std::array<uint64_t, MAX_MODULAR_BOUND_COUNT>
      modular_bound_candidate_scans;
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
  uint64_t bound_transitions;
  uint64_t bound_nextafter_calls;
  ProjectedDiagnostics projected_diagnostic_counts;
  std::vector<ProjectedLayerDiagnostics>
      projected_layer_diagnostic_counts;
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
  double support_group_prepare_seconds;
  double modular_bound_prepare_seconds;
  size_t actual_preprocess_threads;
};

#endif
