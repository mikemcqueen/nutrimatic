#ifndef NUTRIMATIC_DFS_SOLUTION_SINK_H
#define NUTRIMATIC_DFS_SOLUTION_SINK_H

#include <stddef.h>

#include <vector>

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

#endif
