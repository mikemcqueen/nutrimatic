#ifndef NUTRIMATIC_DFS_SCORE_BOUNDS_H
#define NUTRIMATIC_DFS_SCORE_BOUNDS_H

#include <stdint.h>
#include <math.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include "dfs-alloc.h"
#include "dfs-class-list.h"
#include "dfs-search-stats.h"

class ProjectedActions;
struct ScoreKeyLayout;

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

struct AtomicFloatWord {
  std::atomic<uint32_t> value;
};

// Owns active score-bound construction, storage, statistics, and lookup. The
// facade supplies preparation-local inputs only after selecting active policy.
class ScoreBounds {
 public:
  bool build(
      BoundStateView root, ScoreKeyLayout const& layout,
      ProjectedActions const& actions, size_t budget, size_t threads,
      DfsSearchStats* stats);
  bool lookup(uint64_t key, double* value) const;
  bool root_lookup(double* value) const;

  bool active() const { return stats_.mode != DFS_SCORE_BOUND_OFF; }
  DfsSearchStats::Bounds const& stats() const { return stats_; }

 private:
  struct ProjectedWorker {
    DfsSearchStats::Bounds::Projected stats;
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

  DfsSearchStats::Bounds stats_;
  std::unique_ptr<AtomicFloatWord, DfsAlignedFree> float_values_;
  std::unique_ptr<float, DfsAlignedFree> plain_float_values_;
  double root_score_bound_ = HUGE_VAL;
  bool root_score_bound_ready_ = false;

  void clear();
  bool prepare(size_t state_count, size_t cache_budget,
               bool bottom_up_eligible);
  static BoundStateView bound_state_view(TopDownWorker const& worker) {
    return {{worker.bag.data(), worker.bag_mask}, worker.score_key,
            worker.letters_left, worker.wild_left};
  }
  void consider_projected_top_down_candidate(
      ProjectedActions const& actions, size_t action_index,
      TopDownWorker* worker, double* best, double* max_rounding_error);
  double compute_projected_score_bound_top_down(
      ProjectedActions const& actions, TopDownWorker* worker);
  bool compute_projected_score_bounds_top_down(
      BoundStateView root, ScoreKeyLayout const& layout,
      ProjectedActions const& actions, DfsSearchStats* stats,
      size_t requested_threads);
  bool compute_projected_score_bounds_bottom_up(
      BoundStateView root, ScoreKeyLayout const& layout,
      ProjectedActions const& actions, DfsSearchStats* stats,
      size_t requested_threads);
  void publish_top_down(uint64_t key, double value);
  void set_root(double value);
};

#endif
