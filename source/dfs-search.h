#ifndef NUTRIMATIC_DFS_SEARCH_H
#define NUTRIMATIC_DFS_SEARCH_H

#include <stdint.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "dfs-alloc.h"
#include "dfs-class-list.h"
#include "dfs-projected-actions.h"
#include "dfs-score-bounds.h"
#include "dfs-score-key-layout.h"
#include "dfs-score.h"
#include "dfs-search-data.h"
#include "dfs-search-stats.h"
#include "dfs-solution-sink.h"

// Phase 2 of dfs-anagrams: subtract whole anagram classes from the input bag.
// The rarest remaining symbol selects one class bucket at each node, while an
// entry-point tie-break collapses permutations when that symbol occurs more
// than once.
class DfsAnagramSearch {
 public:
  DfsAnagramSearch(DfsClassList const* classes, std::string const& letters,
                   double segment_penalty, int64_t corpus_total,
                   size_t score_cache_bytes = 0,
                   size_t preprocess_threads = 1,
                   size_t search_threads = 1,
                   double word_bonus = 0.0);

  // A null sink runs the search as a counter. `stats` receives everything this
  // call measured and may be NULL. When the ambient diagnostic stream
  // (dfs_set_diagnostic_stream()) is set, report every 100k * progress_factor
  // nodes. A nonnegative exact_letters fixes the number of exact letters in
  // the projection; a negative value selects the largest depth that fits. When
  // cache fallback is disallowed, return false instead of using a weaker mode
  // when the requested table does not fit. verbose reports serial task
  // splitting to the diagnostic stream when parallel search is selected.
  bool run(DfsSolutionSink* sink, DfsSearchStats* stats = NULL,
           int64_t progress_factor = 1, bool allow_cache_fallback = true,
           int exact_letters = -1,
           bool verbose = false);

  // Tests every phase-1 class against one shared phase-2 preparation. The
  // result is index-parallel to DfsClassList::classes() and is exact even when
  // a projected score bound merges letter identities. Candidate classes are
  // validated using the constructor's requested search-thread count.
  bool find_completable_classes(
      std::vector<bool>* completable, DfsSearchStats* stats = NULL,
      int64_t progress_factor = 1,
      bool allow_cache_fallback = false,
      int exact_letters = -1);

 private:
  // Preparation writes straight into the DfsSearchData it is building; nothing
  // it produces is retained by the facade.
  bool prepare_phase_two(
      DfsSearchData* data, DfsSearchStats* stats,
      int64_t progress_factor, bool allow_cache_fallback, int exact_letters,
      bool score_bounds_requested);
  bool prepare_hot_classes(
      DfsSearchData* data, ScoreKeyLayout const& layout,
      std::unique_ptr<uint16_t[], DfsAlignedFree>* score_wild_lengths,
      char const** failure_reason);
  bool prepare_length_certificate(DfsSearchData* data);
  // The query. Fixed at construction and reused by every call.
  DfsClassList const* const class_list;
  std::string const letters;
  DfsScoreModel const score_model;
  double const segment_boundary_log_score;
  std::vector<double> best_member_log_scores;
  size_t const max_depth;
  size_t const score_cache_budget;
  size_t const requested_preprocess_threads;
  size_t const requested_search_threads;
  // Resolved in the constructor, before any worker starts, so no dispatch
  // happens on the scan path itself.
  bool const support_scan_vector;

};

#endif
