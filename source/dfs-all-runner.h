#ifndef NUTRIMATIC_DFS_ALL_RUNNER_H
#define NUTRIMATIC_DFS_ALL_RUNNER_H

#include <stdint.h>

#include <array>
#include <atomic>
#include <vector>

#include "dfs-class-list.h"
#include "dfs-search-data.h"
#include "dfs-search-stats.h"
#include "dfs-solution-sink.h"

// The exhaustive traversal owns its workers and short-lived task queue.
class DfsAllSolutionsRunner {
 public:
  explicit DfsAllSolutionsRunner(DfsSearchData data);
  // Accumulates into the call's statistics: the traversal counters, the
  // certificate counters, and how the search was actually run.
  void run(DfsSolutionSink* sink, int64_t progress_factor, bool verbose,
           DfsSearchStats* results);

 private:
  static size_t const MAX_SPLIT_DEPTH = 6;

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
    DfsSearchStats::AllSolutions stats;
    DfsSearchStats::Certificate::Counters certificate;
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
                    DfsSolutionSink* sink);
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
  void run_parallel(DfsSolutionSink* sink, size_t threads,
                    size_t target_tasks, uint64_t task_progress_factor,
                    bool verbose);

  DfsSearchData const data;
  // The caller's statistics, borrowed for the duration of run().
  DfsSearchStats* results = NULL;
  std::atomic<int64_t> progress_nodes{0};
  std::atomic<int64_t> progress_solutions{0};
};

#endif
