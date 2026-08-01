#ifndef NUTRIMATIC_DFS_ANY_RUNNER_H
#define NUTRIMATIC_DFS_ANY_RUNNER_H

#include <stdint.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include "dfs-alloc.h"
#include "dfs-class-list.h"
#include "dfs-search-data.h"
#include "dfs-search-stats.h"

// The boolean completion traversal has a separate worker and owns its
// short-lived shared memo table.
class DfsAnySolutionRunner {
 public:
  explicit DfsAnySolutionRunner(DfsSearchData data);
  // Accumulates into the call's statistics, as DfsAllSolutionsRunner does.
  bool run(std::vector<bool>* completable, DfsSearchStats* results);

 private:
  struct AtomicWord {
    std::atomic<uint64_t> value;
  };

  struct Memo {
    std::unique_ptr<AtomicWord, DfsAlignedFree> slots;
    size_t capacity = 0;
    size_t entry_limit = 0;
    std::atomic<size_t> entries{0};
  };

  // Workers share only the memo table, so their own counters get a cache
  // line to themselves; stats.nodes moves on every recurrence step.
  struct alignas(64) Worker {
    DfsSearchStats::AnySolution stats;
    std::array<uint32_t, DFS_SYMBOL_COUNT> bag;
    uint64_t bag_mask = 0;
    uint64_t score_key = 0;
    uint64_t exact_key = 0;
  };

  // How one root class was decided, so the batch loop can count the memo,
  // bound, and full-search verdicts separately.
  enum ResultSource {
    RESULT_EMPTY,
    RESULT_MEMO,
    RESULT_BOUND_NO,
    RESULT_BOUND_YES,
    RESULT_SEARCH,
  };

  enum ChildResult {
    CHILD_FALSE,
    CHILD_TRUE,
    CHILD_UNKNOWN,
  };

  bool prepare_memo(DfsClassSpan classes);
  bool memo_lookup(Worker* worker, uint64_t exact_key, bool* value) const;
  void memo_store(Worker* worker, uint64_t exact_key, bool value);
  bool class_fits(uint32_t class_index, Worker const& worker) const;
  bool multiplicity_fits(uint32_t class_index, Worker const& worker) const;
  void subtract_class(uint32_t class_index, Worker* worker,
                      uint64_t* parent_bag_mask) const;
  void restore_class(uint32_t class_index, Worker* worker,
                     uint64_t parent_bag_mask) const;
  ChildResult classify_child(Worker* worker, uint32_t class_index,
                             size_t candidate_length, size_t letters_left);
  bool remainder_completable(Worker* worker, size_t letters_left,
                             ResultSource* source);
  // The tail of remainder_completable(), entered directly when the caller
  // has already probed the memo and the score bound for this exact key.
  bool expand_node(Worker* worker, size_t letters_left);
  bool candidates_immediate(Worker* worker, size_t letters_left);
  bool candidates_lookahead(Worker* worker, size_t letters_left);
  bool buffered_candidates(Worker* worker, size_t letters_left,
                           uint32_t const* class_ids, size_t count);

  DfsSearchData const data;
  Memo memo;
  size_t const lookahead;
};

#endif
