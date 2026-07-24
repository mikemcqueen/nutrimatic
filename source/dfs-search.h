#ifndef NUTRIMATIC_DFS_SEARCH_H
#define NUTRIMATIC_DFS_SEARCH_H

#include <stdio.h>
#include <stdint.h>

#include <array>
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
  virtual ~DfsSolutionSink() { }
};

// Phase 2 of dfs-anagrams: subtract whole anagram classes from the input bag.
// The rarest remaining symbol selects one class bucket at each node, while an
// entry-point tie-break collapses permutations when that symbol occurs more
// than once.
class DfsAnagramSearch {
 public:
  DfsAnagramSearch(DfsClassList const* classes, std::string const& letters,
                   double restart, int64_t corpus_total);

  // A null sink runs the search as a counter. Statistics are reset on each run.
  // When progress is non-null, report every 100k * progress_factor nodes.
  void run(DfsSolutionSink* sink, FILE* progress = NULL,
           int progress_factor = 1);

  int64_t nodes_visited() const { return nodes; }
  int64_t solutions_found() const { return solutions; }

 private:
  void walk(size_t letters_left, int old_rarest_rank, size_t entry_point,
            double representative_log_score, DfsSolutionSink* sink);

  DfsClassList const* const class_list;
  std::string const letters;
  double const restart_log_rate;
  std::vector<double> best_member_log_scores;
  size_t const max_depth;

  std::array<uint32_t, DFS_SYMBOL_COUNT> bag;
  std::vector<size_t> path;
  FILE* progress_stream;
  int64_t progress_interval;
  int64_t nodes;
  int64_t solutions;
};

#endif
