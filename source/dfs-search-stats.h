#ifndef NUTRIMATIC_DFS_SEARCH_STATS_H
#define NUTRIMATIC_DFS_SEARCH_STATS_H

#include <stddef.h>
#include <stdint.h>

// Which score bound a phase-2 call prepared. Declared with the statistics
// rather than on ScoreBounds because the statistics record it and outlive the
// table they describe.
enum DfsScoreBoundMode {
  DFS_SCORE_BOUND_OFF,
  DFS_SCORE_BOUND_PROJECTED,
};

// Everything one phase-2 call measured, grouped by the component that produced
// it. This is the result of a call, not state: the search object retains none
// of it.
//
// Every phase-2 counter is declared here and nowhere else. A component that
// accumulates one accumulates into the struct below that owns it, so a worker's
// running total and the reported result are the same declaration and cannot
// drift apart.
struct DfsSearchStats {
  // The score-bound table's projection shape, storage, and construction work.
  // ScoreBounds keeps one of these while it builds; the search snapshots it
  // once preparation finishes, because the table itself is moved into the
  // runner and destroyed with it.
  struct Bounds {
    // The only part accumulated per worker, so the only part that merges.
    struct Projected {
      size_t states_computed = 0;
      uint64_t candidate_tests = 0;
      uint64_t fitting_transitions = 0;
      uint64_t transitions = 0;
      uint64_t nextafter_calls = 0;

      void clear() { *this = {}; }

      void add(Projected const& other) {
        states_computed += other.states_computed;
        candidate_tests += other.candidate_tests;
        fitting_transitions += other.fitting_transitions;
        transitions += other.transitions;
        nextafter_calls += other.nextafter_calls;
      }
    };

    DfsScoreBoundMode mode = DFS_SCORE_BOUND_OFF;
    size_t entries = 0;
    size_t capacity = 0;
    size_t value_bytes = 0;
    size_t bytes_charged = 0;
    bool complete = false;
    // The projection shape describes the table the bound would use, and is
    // recorded even when the mode ends up OFF. The search fills these in after
    // preparation; the bound table itself never writes them.
    size_t exact_letters = 0;
    size_t wild_letters = 0;
    size_t projected_actions = 0;
    Projected projected;
  };

  // Whether preparation built a length certificate, and what it saved.
  struct Certificate {
    // The only part accumulated per worker, so the only part that merges.
    struct Counters {
      uint64_t group_tests = 0;
      uint64_t group_rejects = 0;
      uint64_t scans_skipped = 0;
      uint64_t scans_kept = 0;

      void add(Counters const& other) {
        group_tests += other.group_tests;
        group_rejects += other.group_rejects;
        scans_skipped += other.scans_skipped;
        scans_kept += other.scans_kept;
      }
    };

    bool ready = false;
    bool shadow = false;
    size_t table_bytes = 0;
    Counters counters;

    bool skipping() const { return ready && !shadow; }
  };

  struct AllSolutions {
    int64_t nodes = 0;
    int64_t solutions = 0;
    int64_t bound_prunes = 0;

    void add(AllSolutions const& other) {
      nodes += other.nodes;
      solutions += other.solutions;
      bound_prunes += other.bound_prunes;
    }
  };

  struct AnySolution {
    struct Memo {
      size_t states = 0;
      size_t hits = 0;

      void add(Memo const& other) {
        states += other.states;
        hits += other.hits;
      }
    };

    struct Lookahead {
      uint64_t full_windows = 0;
      uint64_t known_true_wins = 0;
      uint64_t reprobes_decided = 0;
      uint64_t recursive_expansions = 0;

      void add(Lookahead const& other) {
        full_windows += other.full_windows;
        known_true_wins += other.known_true_wins;
        reprobes_decided += other.reprobes_decided;
        recursive_expansions += other.recursive_expansions;
      }
    };

    size_t classes_checked = 0;
    size_t bound_rejects = 0;
    size_t exact_bound_accepts = 0;
    size_t exact_validations = 0;
    int64_t nodes = 0;
    Memo memo;
    Lookahead lookahead;

    void add(AnySolution const& other) {
      classes_checked += other.classes_checked;
      bound_rejects += other.bound_rejects;
      exact_bound_accepts += other.exact_bound_accepts;
      exact_validations += other.exact_validations;
      nodes += other.nodes;
      memo.add(other.memo);
      lookahead.add(other.lookahead);
    }
  };

  struct Execution {
    double setup_seconds = 0.0;
    double search_seconds = 0.0;
    size_t preprocess_threads = 1;
    size_t search_threads = 1;
    uint64_t search_tasks = 0;
  };

  Bounds bounds;
  Certificate certificate;
  AllSolutions all_solutions;
  AnySolution any_solution;
  Execution execution;
};

#endif
