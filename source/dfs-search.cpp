#include "dfs-search.h"

#include <assert.h>
#include <math.h>

#include <algorithm>

static double make_restart_log_rate(double restart, int64_t corpus_total) {
  assert(restart > 0.0);
  assert(corpus_total > 0);
  return log(restart) - log(double(corpus_total));
}

static size_t derived_max_depth(DfsClassList const* classes,
                                size_t letter_count) {
  assert(classes != NULL);
  return letter_count / size_t(classes->min_word_length());
}

DfsAnagramSearch::DfsAnagramSearch(DfsClassList const* classes,
                                   std::string const& letters,
                                   double restart, int64_t corpus_total):
    class_list(classes),
    letters(letters),
    restart_log_rate(make_restart_log_rate(restart, corpus_total)),
    max_depth(derived_max_depth(classes, letters.size())),
    nodes(0),
    solutions(0) {
  assert(class_list != NULL);

  std::vector<DfsAnagramClass> const& all_classes = class_list->classes();
  best_member_log_scores.reserve(all_classes.size());
  for (size_t i = 0; i < all_classes.size(); ++i) {
    assert(!all_classes[i].members.empty());
    assert(all_classes[i].members[0].count > 0);
    best_member_log_scores.push_back(
        log(double(all_classes[i].members[0].count)));
  }
}

void DfsAnagramSearch::run(DfsSolutionSink* sink) {
  bag.fill(0);
  for (size_t i = 0; i < letters.size(); ++i) {
    int const symbol = dfs_symbol_index((unsigned char) letters[i]);
    assert(symbol >= 0);
    ++bag[size_t(symbol)];
  }

  path.clear();
  path.reserve(letters.size());
  nodes = 0;
  solutions = 0;
  walk(letters.size(), 0, 0, 0.0, sink);
}

void DfsAnagramSearch::walk(size_t letters_left, int old_rarest_rank,
                            size_t entry_point,
                            double representative_log_score,
                            DfsSolutionSink* sink) {
  ++nodes;

  // Removing letters can only advance this rank, so resume at the parent's
  // forced symbol instead of rescanning the whole priority order.
  std::array<int, DFS_SYMBOL_COUNT> const& rank_to_symbol =
      class_list->rank_to_symbol();
  int rank = old_rarest_rank;
  while (rank < DFS_SYMBOL_COUNT &&
         bag[size_t(rank_to_symbol[size_t(rank)])] == 0)
    ++rank;
  if (rank == DFS_SYMBOL_COUNT) return;

  int const forced_symbol = rank_to_symbol[size_t(rank)];
  size_t start = class_list->candidate_begin(forced_symbol);
  size_t const end = class_list->candidate_end(forced_symbol);

  // When the same forced symbol survives into the child, only choose classes
  // at or after the parent's position. Passing the position itself permits a
  // class to repeat. If the forced symbol advances, its bucket begins after
  // the old bucket and this max has no effect.
  start = std::max(start, entry_point);

  std::vector<DfsAnagramClass> const& classes = class_list->classes();
  for (size_t class_index = start; class_index < end; ++class_index) {
    DfsAnagramClass const& candidate = classes[class_index];
    bool fits = true;
    for (size_t i = 0; i < candidate.letters.size(); ++i) {
      int const symbol = candidate.letters[i].first;
      uint32_t const count = candidate.letters[i].second;
      if (bag[size_t(symbol)] < count) {
        fits = false;
        break;
      }
    }
    if (!fits) continue;

    double const candidate_log_score =
        best_member_log_scores[class_index];
    double const next_log_score =
        path.empty()
            ? candidate_log_score
            : representative_log_score + restart_log_rate +
                  candidate_log_score;

    size_t const candidate_length = candidate.key.size();
    assert(candidate_length <= letters_left);
    size_t const next_letters_left = letters_left - candidate_length;

    path.push_back(class_index);
    if (next_letters_left == 0) {
      ++solutions;
      if (sink != NULL) sink->emit(path, next_log_score);
      path.pop_back();
      continue;
    }

    // The cap is derived from min_word_len rather than configured separately.
    // It cannot exclude a solution because every class consumes at least that
    // many letters; retaining it also matches Phase 0's node-count convention.
    if (path.size() >= max_depth) {
      path.pop_back();
      continue;
    }

    for (size_t i = 0; i < candidate.letters.size(); ++i)
      bag[size_t(candidate.letters[i].first)] -= candidate.letters[i].second;
    walk(next_letters_left, rank, class_index, next_log_score, sink);
    for (size_t i = 0; i < candidate.letters.size(); ++i)
      bag[size_t(candidate.letters[i].first)] += candidate.letters[i].second;
    path.pop_back();
  }
}
