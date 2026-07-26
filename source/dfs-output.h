#ifndef NUTRIMATIC_DFS_OUTPUT_H
#define NUTRIMATIC_DFS_OUTPUT_H

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dfs-search.h"

struct DfsSpelling {
  double log_score;
  std::string text;
  std::string word_set_key;
};

// Phase 3 of dfs-anagrams: lazily expand each class solution into spellings and
// retain the global top N. The heap index doubles as the dedup table, so both
// structures remain bounded by N even across arbitrarily many solutions.
class DfsTopN: public DfsSolutionSink {
 public:
  DfsTopN(DfsClassList const* classes, size_t limit);

  void emit(std::vector<size_t> const& class_indexes,
            double representative_log_score);
  bool supports_score_pruning() const { return result_limit != 0; }
  bool score_floor(double* floor) const;
  bool supports_parallel_search() const { return true; }

  size_t size() const { return heap.size(); }
  size_t limit() const { return result_limit; }
  size_t spellings_expanded() const { return expanded; }

  // Drains the heap into descending score order. Equal-score rows use their
  // word-set key and text as deterministic tie-breaks.
  std::vector<DfsSpelling> take_sorted_results();

 private:
  void offer(DfsSpelling const& spelling);
  void swap_heap_entries(size_t a, size_t b);
  void sift_up(size_t position);
  void sift_down(size_t position);
  bool full() const { return heap.size() == result_limit; }
  double floor_log_score() const;

  DfsClassList const* const class_list;
  size_t const result_limit;
  size_t expanded;

  // A min-heap: the weakest retained spelling is always at position zero.
  std::vector<DfsSpelling> heap;
  std::unordered_map<std::string, size_t> positions;

  // A parallel search calls emit() from several threads and score_floor() at
  // nearly every node. The heap is serialized, but the floor is published as a
  // separate atomic so the hot read never contends. The floor only ever rises
  // once the heap is full, so a reader that observes a stale value simply
  // prunes less; it can never discard a retained spelling.
  mutable std::mutex heap_mutex;
  std::atomic<uint64_t> published_floor_bits;
  std::atomic<bool> published_full;
  void publish_floor();
};

#endif
