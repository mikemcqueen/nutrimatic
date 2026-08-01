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
#include "dfs-search-internal.h"
#include "dfs-score.h"

// A non-owning view of a letter bag in rarest-rank order.  Both bound
// construction and search workers use the same representation.
struct LetterBagView {
  uint32_t const* counts;
  uint64_t support_mask;
};

// The portion of a DFS state needed to query a score bound.  This remains a
// value type so bound consumers need not borrow a worker object.
struct BoundStateView {
  LetterBagView letter_bag;
  uint64_t score_key;
  size_t letters_left;
  size_t wild_left;
};

// Owns the vocabulary and statistics for score-bound construction.  Storage
// and construction remain on DfsAnagramSearch until the component-ownership
// extraction, but consumers can already use these stable value types.
class ScoreBounds {
 public:
  enum Mode {
    OFF,
    PROJECTED,
  };

  struct ProjectedStats {
    size_t states_computed = 0;
    uint64_t candidate_tests = 0;
    uint64_t fitting_transitions = 0;
    uint64_t transitions = 0;
    uint64_t nextafter_calls = 0;

    void clear() { *this = {}; }

    void add(ProjectedStats const& other) {
      states_computed += other.states_computed;
      candidate_tests += other.candidate_tests;
      fitting_transitions += other.fitting_transitions;
      transitions += other.transitions;
      nextafter_calls += other.nextafter_calls;
    }
  };

  struct Stats {
    Mode mode;
    size_t entries;
    size_t capacity;
    size_t value_bytes;
    size_t bytes_charged;
    bool complete;
    ProjectedStats projected;
  };

 private:
  struct ProjectedWorker {
    ProjectedStats stats;
  };

  struct BottomUpWorker : ProjectedWorker {
    std::array<uint32_t, DFS_SYMBOL_COUNT> bag;
    std::vector<double> best;
    std::vector<double> max_rounding_error;

    explicit BottomUpWorker(size_t wild_span):
        best(wild_span),
        max_rounding_error(wild_span) {
      bag.fill(0);
    }
  };

  struct TopDownWorker : ProjectedWorker {
    std::array<uint32_t, DFS_SYMBOL_COUNT> bag;
    uint64_t bag_mask;
    uint64_t score_key;
    size_t letters_left;
    size_t wild_left;
    double best;
    double max_rounding_error;
  };

  friend class DfsAnagramSearch;
};

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
  struct CertificateStats {
    uint64_t group_tests = 0;
    uint64_t group_rejects = 0;
    uint64_t scans_skipped = 0;
    uint64_t scans_kept = 0;

    void clear() { *this = {}; }
    void add(CertificateStats const& other) {
      group_tests += other.group_tests;
      group_rejects += other.group_rejects;
      scans_skipped += other.scans_skipped;
      scans_kept += other.scans_kept;
    }
  };

  struct AllSolutionsStats {
    int64_t nodes = 0;
    int64_t solutions = 0;
    int64_t bound_prunes = 0;
    CertificateStats certificate;

    void clear() { *this = {}; }
    void add(AllSolutionsStats const& other) {
      nodes += other.nodes;
      solutions += other.solutions;
      bound_prunes += other.bound_prunes;
      certificate.add(other.certificate);
    }
  };

  struct ExactMemoStats {
    size_t states = 0;
    size_t hits = 0;

    void clear() { *this = {}; }
    void add(ExactMemoStats const& other) {
      states += other.states;
      hits += other.hits;
    }
  };

  struct LookaheadStats {
    uint64_t full_windows = 0;
    uint64_t known_true_wins = 0;
    uint64_t reprobes_decided = 0;
    uint64_t recursive_expansions = 0;

    void clear() { *this = {}; }
    void add(LookaheadStats const& other) {
      full_windows += other.full_windows;
      known_true_wins += other.known_true_wins;
      reprobes_decided += other.reprobes_decided;
      recursive_expansions += other.recursive_expansions;
    }
  };

  struct AnySolutionStats {
    size_t classes_checked = 0;
    size_t bound_rejects = 0;
    size_t exact_bound_accepts = 0;
    size_t exact_validations = 0;
    ExactMemoStats memo;
    LookaheadStats lookahead;
    int64_t nodes = 0;
  };

  struct RunStats {
    double setup_seconds = 0.0;
    double search_seconds = 0.0;
    size_t preprocess_threads = 1;
    size_t search_threads = 1;
    uint64_t search_tasks = 0;
  };

  struct DfsSearchStats {
    ScoreBounds::Stats score_bounds;
    AllSolutionsStats all_solutions;
    AnySolutionStats any_solution;
    RunStats run;
  };

  DfsAnagramSearch(DfsClassList const* classes, std::string const& letters,
                   double segment_penalty, int64_t corpus_total,
                   size_t score_cache_bytes = 0,
                   size_t preprocess_threads = 1,
                   size_t search_threads = 1,
                   double word_bonus = 0.0);

  // A null sink runs the search as a counter. Statistics are reset on each run.
  // When the ambient diagnostic stream (dfs_set_diagnostic_stream()) is set,
  // report every 100k * progress_factor nodes. A nonnegative exact_letters
  // fixes the number of exact letters in the projection; a
  // negative value selects the largest depth that fits. When cache fallback
  // is disallowed, return false instead of using a weaker mode when the
  // requested table does not fit. verbose reports serial task splitting to
  // the diagnostic stream when parallel search is selected.
  bool run(DfsSolutionSink* sink,
           int64_t progress_factor = 1, bool allow_cache_fallback = true,
           int exact_letters = -1,
           bool verbose = false);

  // Tests every phase-1 class against one shared phase-2 preparation. The
  // result is index-parallel to DfsClassList::classes() and is exact even when
  // a projected score bound merges letter identities. Candidate classes are
  // validated using the constructor's requested search-thread count.
  bool find_completable_classes(
      std::vector<bool>* completable, int64_t progress_factor = 1,
      bool allow_cache_fallback = false,
      int exact_letters = -1);

  DfsSearchStats const& stats() const { return search_stats; }

  int64_t nodes_visited() const { return search_stats.all_solutions.nodes; }
  int64_t solutions_found() const {
    return search_stats.all_solutions.solutions;
  }
  ScoreBounds::Mode score_bound_mode() const {
    return search_stats.score_bounds.mode;
  }
  size_t score_bound_entries() const {
    return search_stats.score_bounds.entries;
  }
  size_t score_bound_states_computed() const {
    return search_stats.score_bounds.projected.states_computed;
  }
  uint64_t score_bound_transitions() const {
    return search_stats.score_bounds.projected.transitions;
  }
  uint64_t score_bound_candidate_tests() const {
    return search_stats.score_bounds.projected.candidate_tests;
  }
  uint64_t score_bound_fitting_transitions() const {
    return search_stats.score_bounds.projected.fitting_transitions;
  }
  uint64_t score_bound_nextafter_calls() const {
    return search_stats.score_bounds.projected.nextafter_calls;
  }
  size_t score_bound_bytes_charged() const {
    return search_stats.score_bounds.bytes_charged;
  }
  size_t score_bound_capacity() const {
    return search_stats.score_bounds.capacity;
  }
  size_t score_bound_value_bytes() const {
    return search_stats.score_bounds.value_bytes;
  }
  bool score_bound_complete() const {
    return search_stats.score_bounds.complete;
  }
  size_t score_bound_exact_letters() const {
    return score_exact_letters;
  }
  size_t score_bound_wild_letters() const {
    return score_wild_letters;
  }
  size_t score_bound_projected_actions() const {
    return projected_actions.size();
  }
  int64_t score_bound_prunes() const {
    return search_stats.all_solutions.bound_prunes;
  }
  size_t completable_classes_checked() const {
    return search_stats.any_solution.classes_checked;
  }
  size_t completable_bound_rejects() const {
    return search_stats.any_solution.bound_rejects;
  }
  size_t completable_exact_bound_accepts() const {
    return search_stats.any_solution.exact_bound_accepts;
  }
  size_t completable_exact_validations() const {
    return search_stats.any_solution.exact_validations;
  }
  size_t exact_memo_states_computed() const {
    return search_stats.any_solution.memo.states;
  }
  size_t exact_memo_hits() const {
    return search_stats.any_solution.memo.hits;
  }

  // Test hook: one projected AVX2 wildcard update over `count` contiguous
  // wildcard counts, exactly as the bottom-up evaluator performs it.
  static uint64_t test_projected_wild_update(
      double partial_score, double rounding_error_base,
      float const* children, double* best, double* max_rounding_error,
      size_t count);

  double phase_two_setup_seconds() const {
    return search_stats.run.setup_seconds;
  }
  double phase_two_search_seconds() const {
    return search_stats.run.search_seconds;
  }
  size_t preprocess_threads_used() const {
    return search_stats.run.preprocess_threads;
  }
  size_t search_threads_used() const {
    return search_stats.run.search_threads;
  }
  uint64_t search_tasks_generated() const {
    return search_stats.run.search_tasks;
  }
  bool length_certificate_enabled() const {
    return length_certificate_ready;
  }
  bool length_certificate_skipping() const {
    return length_certificate_ready && !length_certificate_shadow;
  }
  uint64_t length_certificate_group_tests() const {
    return search_stats.all_solutions.certificate.group_tests;
  }
  uint64_t length_certificate_group_rejects() const {
    return search_stats.all_solutions.certificate.group_rejects;
  }
  uint64_t length_certificate_scans_skipped() const {
    return search_stats.all_solutions.certificate.scans_skipped;
  }
  uint64_t length_certificate_scans_kept() const {
    return search_stats.all_solutions.certificate.scans_kept;
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

  static BoundStateView bound_state_view(
      ScoreBounds::TopDownWorker const& worker) {
    return {{worker.bag.data(), worker.bag_mask}, worker.score_key,
            worker.letters_left, worker.wild_left};
  }

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

  // The exhaustive traversal owns its workers and short-lived task queue.
  // Prepared query data and bound lookup remain on the facade until the
  // DfsSearchData extraction.
  class DfsAllSolutionsRunner {
   public:
    explicit DfsAllSolutionsRunner(DfsAnagramSearch& search);
    void run(DfsSolutionSink* sink, int64_t progress_factor, bool verbose);

   private:
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

    struct alignas(4096) Worker {
      AllSolutionsStats stats;
      std::array<uint32_t, DFS_SYMBOL_COUNT> bag;
      uint64_t bag_mask;
      uint64_t score_key;
      std::vector<size_t> path;
      size_t split_depth;
      std::vector<SearchTask>* produced;
      int64_t next_progress;
      int64_t reported_solutions;
    };

    bool should_prune(Worker* worker, double representative_log_score,
                      DfsSolutionSink* sink, size_t letters_left);
    void visit_fitting_class(Worker* worker, uint32_t class_index,
                             FitClassMetadata metadata, size_t letters_left,
                             double representative_log_score,
                             DfsSolutionSink* sink);
    bool visit_fitting_range(Worker* worker, size_t begin, size_t end,
                             size_t letters_left,
                             double representative_log_score,
                             DfsSolutionSink* sink);
    void walk(Worker* worker, size_t letters_left, size_t entry_point,
              double representative_log_score, DfsSolutionSink* sink);
    void walk_certified(Worker* worker, int rank, size_t start, size_t end,
                        size_t letters_left,
                        double representative_log_score, double floor,
                        DfsSolutionSink* sink);
    void start_worker(Worker* worker);
    void report_progress(Worker* worker);
    void merge_worker(Worker const& worker);
    void run_parallel(DfsSolutionSink* sink, size_t threads,
                      size_t target_tasks, uint64_t task_progress_factor,
                      bool verbose);
    bool multiplicity_fits(FitClassMetadata metadata,
                           Worker const& worker) const;

    DfsAnagramSearch& search;
    std::atomic<int64_t> progress_nodes{0};
    std::atomic<int64_t> progress_solutions{0};
  };

  // The boolean completion traversal has a separate worker and owns its
  // short-lived shared memo table.  The facade still supplies the prepared
  // query data and projected-bound lookup during this transition.
  class DfsAnySolutionRunner {
   public:
   explicit DfsAnySolutionRunner(DfsAnagramSearch& search);
    bool run(std::vector<bool>* completable);

    struct Memo {
      std::unique_ptr<AtomicWord, DfsAlignedFree> slots;
      size_t capacity = 0;
      size_t entry_limit = 0;
      std::atomic<size_t> entries{0};
    };

    struct Worker {
      AnySolutionStats stats;
      Memo* memo = NULL;
      std::array<uint32_t, DFS_SYMBOL_COUNT> bag;
      uint64_t bag_mask = 0;
      uint64_t score_key = 0;
      uint64_t exact_key = 0;
      size_t lookahead = EXACT_MEMO_LOOKAHEAD_DEFAULT;
      int64_t next_progress = INT64_MAX;
    };

   private:

    bool prepare_memo(DfsClassSpan classes);
    DfsAnagramSearch& search;
    Memo memo;
    size_t lookahead;
  };

  bool prepare_phase_two(
      int64_t progress_factor, bool allow_cache_fallback, int exact_letters,
      bool score_bounds_requested);
  void require_hot_classes() const;
  bool prepare_hot_classes();
  bool prepare_projected_actions();
  bool prepare_length_certificate();
  bool length_certificate_rejects(
      size_t base, size_t length, size_t letters_left,
      double representative_log_score, double floor) const;
  void prepare_score_bounds(bool requested);
  void clear_score_bounds();
  Reachability cached_reachability(
      uint64_t score_key, bool original_root) const;
  bool exact_remainder_completable(
      DfsAnySolutionRunner::Worker* worker, size_t letters_left,
      ExactResultSource* source = NULL);
  ExactChildResult classify_exact_child(
      DfsAnySolutionRunner::Worker* worker, uint32_t class_index,
      size_t candidate_length, size_t letters_left);
  // The tail of exact_remainder_completable, entered directly when the caller
  // has already probed the memo and the score bound for this exact key.
  bool exact_expand_node(
      DfsAnySolutionRunner::Worker* worker, size_t letters_left);
  bool exact_candidates_immediate(
      DfsAnySolutionRunner::Worker* worker, size_t letters_left);
  bool exact_candidates_lookahead(
      DfsAnySolutionRunner::Worker* worker, size_t letters_left);
  bool exact_buffered_candidates(
      DfsAnySolutionRunner::Worker* worker, size_t letters_left,
      uint32_t const* class_ids, size_t count);
  bool exact_memo_lookup(
      DfsAnySolutionRunner::Worker* worker, uint64_t exact_key, bool* value);
  void exact_memo_store(
      DfsAnySolutionRunner::Worker* worker, uint64_t exact_key, bool value);
  bool exact_class_fits(size_t class_index,
                        DfsAnySolutionRunner::Worker const& worker) const;
  void subtract_exact_class(
      size_t class_index, DfsAnySolutionRunner::Worker* worker,
      uint64_t* parent_bag_mask);
  void restore_exact_class(
      size_t class_index, DfsAnySolutionRunner::Worker* worker,
      uint64_t parent_bag_mask);

  void walk_unoptimized(size_t letters_left, int old_rarest_rank,
                        size_t entry_point, double representative_log_score,
                        DfsSolutionSink* sink);

  bool hot_class_fits(uint32_t class_index) const;
  bool hot_class_multiplicity_fits(uint32_t class_index) const;
  bool hot_class_fits(uint32_t class_index,
                      DfsAnySolutionRunner::Worker const& worker) const;
  bool hot_class_multiplicity_fits(
      uint32_t class_index, DfsAnySolutionRunner::Worker const& worker) const;
  bool hot_class_multiplicity_fits(
      FitClassMetadata metadata,
      DfsAnySolutionRunner::Worker const& worker) const;
  bool projected_action_fits(
      size_t action_index, BoundStateView state) const;
  size_t first_length_candidate(
      size_t begin, size_t end, size_t letters_left) const;
  size_t first_projected_length_candidate(
      size_t begin, size_t end, size_t letters_left) const;
  double compute_projected_score_bound_top_down(
      ScoreBounds::TopDownWorker* worker);
  void consider_projected_top_down_candidate(
      size_t action_index, ScoreBounds::TopDownWorker* worker, double* best,
      double* max_rounding_error);
  bool compute_projected_score_bounds_top_down(size_t requested_threads);
  bool compute_projected_score_bounds_bottom_up(size_t requested_threads);
  bool load_score_bound(uint64_t key, double* value) const;
  void publish_top_down_score_bound(uint64_t key, double value);
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
  DfsSearchStats search_stats;
  // With bounds off cached_reachability answers UNKNOWN for every key, so the
  // exact recurrence can skip maintaining and probing score keys entirely.
  bool score_bounds_active() const {
    return search_stats.score_bounds.mode != ScoreBounds::OFF;
  }
  std::unique_ptr<AtomicFloatWord, DfsAlignedFree> bound_float_values;
  std::unique_ptr<float, DfsAlignedFree> bound_plain_float_values;
  double root_score_bound;
  bool root_score_bound_ready;

  std::vector<size_t> path;
  bool progress_enabled;
  int64_t progress_interval;
  int64_t next_progress;
};

#endif
