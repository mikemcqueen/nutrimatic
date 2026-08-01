#ifndef NUTRIMATIC_DFS_OUTPUT_H
#define NUTRIMATIC_DFS_OUTPUT_H

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dfs-class-list.h"
#include "dfs-solution-sink.h"

struct DfsSpelling {
  double log_score;
  std::string text;
  std::string word_set_key;
};

// The dedup table's payload. The map key (not duplicated here) is the
// word-set key. When the result limit is nonzero, heap_pos is this entry's
// current slot in DfsTopN::heap so a heap swap can fix up both sides in O(1);
// it is not meaningful in unlimited mode.
struct RetainedSpelling {
  std::string text;
  double log_score;
  size_t heap_pos;
};

typedef std::unordered_map<std::string, RetainedSpelling> RetainedMap;

// std::pair<std::string const, RetainedSpelling>: the key/value pair the map
// stores. The heap points at this rather than at the mapped value alone, so
// that weaker()'s tie-break can reach the word-set key through ->first.
typedef RetainedMap::value_type RetainedEntry;

struct HeapSlot {
  double log_score;
  RetainedEntry* retained;
};

// Phase 3 of dfs-anagrams: lazily expand each class solution into spellings and
// retain the global top N, or every spelling when N is zero. `retained` is the
// dedup table and owns all string storage; `heap` orders pointers into it by
// score and is bounded by N when N is nonzero. A map node's address is stable
// across rehash, so the heap can hold raw pointers.
class DfsTopN: public DfsSolutionSink {
 public:
  DfsTopN(DfsClassList const* classes, size_t limit);

  void emit(std::vector<size_t> const& class_indexes,
            double representative_log_score);
  bool supports_score_pruning() const { return result_limit != 0; }
  bool score_floor(double* floor) const;
  bool supports_parallel_search() const { return true; }

  // These observers and take_sorted_results() are used only after all search
  // workers have joined.
  size_t size() const { return retained.size(); }
  size_t limit() const { return result_limit; }
  size_t spellings_expanded() const { return expanded; }

  // Drains the retained spellings into descending score order. Equal-score
  // rows use their word-set key and text as deterministic tie-breaks.
  std::vector<DfsSpelling> take_sorted_results();

 private:
  bool offer(DfsSpelling spelling);
  void swap_heap_entries(size_t a, size_t b);
  void sift_up(size_t position);
  void sift_down(size_t position);
  double floor_log_score() const;

  DfsClassList const* const class_list;
  size_t const result_limit;
  size_t expanded;

  RetainedMap retained;
  // A min-heap: the weakest retained spelling is always at position zero.
  std::vector<HeapSlot> heap;

  // During parallel search, emit() owns this mutex only while checking and
  // updating the shared heap, retained map, and expanded count. Its spelling
  // expansion queue is worker-local. score_floor() reads the separately
  // published monotone floor so the search hot path does not contend on the
  // heap. A stale lower floor only causes extra work; it cannot prune a
  // retained spelling.
  mutable std::mutex heap_mutex;
  std::atomic<uint64_t> published_floor_bits;
  std::atomic<bool> published_full;
  // publish_floor() is only ever called while heap_mutex is held (from
  // emit()'s locked section), so this needs no atomicity of its own — it
  // just remembers whether the one-time "queue filled" diagnostic already
  // fired.
  bool floor_announced;
  void publish_floor();
};

#endif
