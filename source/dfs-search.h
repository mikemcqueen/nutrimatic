#ifndef NUTRIMATIC_DFS_SEARCH_H
#define NUTRIMATIC_DFS_SEARCH_H

#include <stdio.h>
#include <stdint.h>

#include <array>
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
  enum CandidateCacheMode {
    CANDIDATE_CACHE_OFF,
    CANDIDATE_CACHE_DENSE,
    CANDIDATE_CACHE_SPARSE,
  };

  enum ScoreBoundMode {
    SCORE_BOUND_OFF,
    SCORE_BOUND_DENSE,
    SCORE_BOUND_SPARSE,
  };

  DfsAnagramSearch(DfsClassList const* classes, std::string const& letters,
                   double restart, int64_t corpus_total,
                   size_t candidate_cache_bytes = 0);

  // A null sink runs the search as a counter. Statistics are reset on each run.
  // When progress is non-null, report every 100k * progress_factor nodes.
  void run(DfsSolutionSink* sink, FILE* progress = NULL,
           int progress_factor = 1);

  int64_t nodes_visited() const { return nodes; }
  int64_t solutions_found() const { return solutions; }
  CandidateCacheMode candidate_cache_mode() const { return cache_mode; }
  size_t candidate_cache_entries() const { return admitted_entries; }
  size_t candidate_cache_bytes_charged() const { return charged_bytes; }
  ScoreBoundMode score_bound_mode() const { return bound_mode; }
  size_t score_bound_entries() const { return bound_entries; }
  size_t score_bound_states_computed() const {
    return bound_states_computed;
  }
  size_t score_bound_bytes_charged() const { return bound_charged_bytes; }
  int64_t score_bound_prunes() const { return bound_prunes; }

 private:
  struct AlignedFree {
    void operator()(void* pointer) const;
  };

  struct alignas(32) HotClass {
    double best_member_log_score;
    uint64_t bag_key_delta;
    uint64_t support_mask;
    uint32_t letters_offset;
    uint32_t packed_length_and_count;
  };

  enum WalkMode {
    WALK_UNCACHED,
    WALK_DENSE,
    WALK_SPARSE,
  };

  bool prepare_hot_classes();
  void prepare_score_bounds(uint64_t state_count, DfsSolutionSink* sink);
  void prepare_cache(uint64_t state_count);
  void clear_cache();
  void clear_score_bounds();

  template<WalkMode mode>
  void walk(size_t letters_left, size_t entry_point,
            double representative_log_score, DfsSolutionSink* sink);
  void walk_unoptimized(size_t letters_left, int old_rarest_rank,
                        size_t entry_point, double representative_log_score,
                        DfsSolutionSink* sink);

  bool hot_class_fits(uint32_t class_index) const;
  template<WalkMode mode>
  double compute_score_bound();
  template<WalkMode mode>
  void consider_bound_candidate(uint32_t class_index, double* best);
  bool load_score_bound(uint64_t key, double* value) const;
  bool store_score_bound(uint64_t key, double value);
  bool should_prune(double representative_log_score,
                    DfsSolutionSink* sink) const;
  bool build_candidate_entry(size_t begin, size_t end, uint64_t* metadata);
  void publish_dense(uint64_t key, uint64_t metadata);
  void publish_sparse(size_t slot, uint64_t key, uint64_t metadata);
  uint64_t sparse_lookup(uint64_t key, size_t* slot,
                         bool* may_insert) const;

  template<WalkMode mode>
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
  size_t const candidate_cache_budget;
  size_t active_candidate_cache_budget;

  // The hot bag and all masks use rarest-rank order.
  std::array<uint32_t, DFS_SYMBOL_COUNT> bag;
  std::array<uint64_t, DFS_SYMBOL_COUNT> multipliers;
  uint64_t bag_mask;
  uint64_t current_bag_key;

  std::unique_ptr<HotClass, AlignedFree> hot_classes;
  std::unique_ptr<uint32_t, AlignedFree> packed_letters;
  bool hot_classes_ready;

  ScoreBoundMode bound_mode;
  std::unique_ptr<double, AlignedFree> bound_values;
  std::unique_ptr<uint64_t, AlignedFree> bound_keys;
  size_t bound_capacity;
  size_t bound_max_entries;
  size_t bound_entries;
  size_t bound_states_computed;
  size_t bound_charged_bytes;
  bool bound_aborted;
  int64_t bound_prunes;

  CandidateCacheMode cache_mode;
  std::unique_ptr<uint64_t, AlignedFree> cache_metadata;
  std::unique_ptr<uint64_t, AlignedFree> sparse_keys;
  std::unique_ptr<uint32_t, AlignedFree> candidate_ids;
  size_t cache_capacity;
  size_t sparse_max_entries;
  size_t sparse_filled;
  size_t candidate_capacity;
  size_t candidate_used;
  size_t admitted_entries;
  size_t charged_bytes;
  std::vector<uint32_t> candidate_build_buffer;

  std::vector<size_t> path;
  FILE* progress_stream;
  int64_t progress_interval;
  int64_t next_progress;
  int64_t nodes;
  int64_t solutions;
};

#endif
