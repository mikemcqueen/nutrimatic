#include "dfs-solo-words.h"

#include "dfs-score.h"
#include "index.h"

#include <assert.h>
#include <math.h>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

uint16_t profile_flags(DfsSoloMasks masks) {
  assert((masks.pair_mask & ~masks.word_mask) == 0);
  uint16_t flags = 0;
  if (masks.word_mask != 0) flags |= DFS_MEMBER_SOLO_WORD_EDGE;
  if (masks.pair_mask != 0) flags |= DFS_MEMBER_SOLO_PAIR_EDGE;
  return flags;
}

long double local_upper(DfsSoloMasks masks,
                        long double word_log_bonus,
                        long double pair_log_bonus) {
  assert((masks.pair_mask & ~masks.word_mask) == 0);
  if (masks.pair_mask != 0)
    return word_log_bonus + pair_log_bonus;
  if (masks.word_mask != 0) return word_log_bonus;
  return 0.0L;
}

bool disjoint_profiles(std::vector<DfsSoloMasks> const& profiles) {
  uint16_t used = 0;
  for (size_t i = 0; i < profiles.size(); ++i) {
    if ((profiles[i].word_mask & used) != 0) return false;
    used |= profiles[i].word_mask;
  }
  return true;
}

struct FlowEdge {
  int to;
  int reverse;
  int capacity;
  int cost;
};

void add_edge(std::vector<std::vector<FlowEdge> >* graph,
              int from, int to, int capacity, int cost) {
  FlowEdge const forward = {
    to, int((*graph)[size_t(to)].size()), capacity, cost,
  };
  FlowEdge const reverse = {
    from, int((*graph)[size_t(from)].size()), 0, -cost,
  };
  (*graph)[size_t(from)].push_back(forward);
  (*graph)[size_t(to)].push_back(reverse);
}

}  // namespace

DfsSoloWords::DfsSoloWords(
    IndexReader const* reader, std::vector<std::string> const& words,
    std::unordered_set<std::string> const* pairs,
    DfsScoreModel const* score_model):
    reader(reader),
    words(words),
    pairs(pairs),
    probe_word_edges(score_model != NULL &&
                     score_model->multi_word_log_bonus() != 0.0),
    probe_pair_edges(score_model != NULL && pairs != NULL &&
                     (score_model->multi_word_log_bonus() != 0.0 ||
                      score_model->pair_log_bonus() != 0.0)),
    word_edges(0),
    pair_edges(0),
    frozen(false) {
  assert(reader != NULL);
  assert(words.size() <= 16);
  solo_positions.reserve(words.size());
  for (size_t i = 0; i < words.size(); ++i) {
    SoloPosition saved;
    IndexReader::EntryPosition position;
    saved.present = probe_word_edges &&
        reader->aggregate_entry_position(words[i], &position);
    saved.continuation = saved.present ? position.continuation : off_t(-1);
    saved.aggregate_count = saved.present ? position.aggregate_count : 0;
    solo_positions.push_back(saved);
  }
}

DfsSoloMasks DfsSoloWords::resolve_from_position(
    std::string_view candidate,
    off_t candidate_continuation,
    int64_t candidate_aggregate_count,
    bool candidate_present) const {
  DfsSoloMasks result;
  for (size_t i = 0; i < words.size(); ++i) {
    uint16_t const bit = uint16_t(1) << i;
    bool pair_edge = false;
    if (probe_pair_edges) {
      std::string key(candidate);
      key.push_back(' ');
      key += words[i];
      pair_edge = pairs->count(key) != 0;
    }
    if (pair_edge) {
      result.word_mask |= bit;
      result.pair_mask |= bit;
      continue;
    }
    if (!probe_word_edges) continue;

    IndexReader::EntryPosition ignored;
    IndexReader::EntryPosition const candidate_position = {
      candidate_continuation, candidate_aggregate_count,
    };
    IndexReader::EntryPosition const solo_position = {
      solo_positions[i].continuation, solo_positions[i].aggregate_count,
    };
    bool const candidate_first = candidate_present &&
        reader->continuation_entry_position(
            candidate_position, words[i], &ignored);
    bool const solo_first = solo_positions[i].present &&
        reader->continuation_entry_position(
            solo_position, candidate, &ignored);
    if (candidate_first || solo_first) result.word_mask |= bit;
  }
  return result;
}

DfsSoloMasks DfsSoloWords::resolve(
    std::string_view candidate,
    off_t candidate_continuation,
    int64_t candidate_aggregate_count) const {
  return resolve_from_position(
      candidate, candidate_continuation, candidate_aggregate_count, true);
}

DfsSoloMasks DfsSoloWords::resolve(std::string_view candidate) const {
  IndexReader::EntryPosition position;
  bool const found = reader->aggregate_entry_position(candidate, &position);
  return resolve_from_position(
      candidate, found ? position.continuation : off_t(-1),
      found ? position.aggregate_count : 0, found);
}

uint16_t DfsSoloWords::register_profile(
    std::string_view candidate,
    off_t candidate_continuation,
    int64_t candidate_aggregate_count) {
  assert(!frozen);
  DfsSoloMasks const masks = resolve(
      candidate, candidate_continuation, candidate_aggregate_count);
  if (masks.word_mask == 0) return 0;
  Profile profile;
  profile.text.assign(candidate.data(), candidate.size());
  profile.masks = masks;
  profiles.push_back(std::move(profile));
  word_edges += size_t(__builtin_popcount(unsigned(masks.word_mask)));
  pair_edges += size_t(__builtin_popcount(unsigned(masks.pair_mask)));
  return profile_flags(masks);
}

void DfsSoloWords::freeze() {
  assert(!frozen);
  std::sort(profiles.begin(), profiles.end(),
            [](Profile const& a, Profile const& b) {
    return a.text < b.text;
  });
  for (size_t i = 1; i < profiles.size(); ++i) {
    if (profiles[i - 1].text != profiles[i].text) continue;
    assert(profiles[i - 1].masks.word_mask == profiles[i].masks.word_mask);
    assert(profiles[i - 1].masks.pair_mask == profiles[i].masks.pair_mask);
  }
  profiles.erase(
      std::unique(profiles.begin(), profiles.end(),
                  [](Profile const& a, Profile const& b) {
        return a.text == b.text;
      }),
      profiles.end());
  frozen = true;
}

DfsSoloMasks DfsSoloWords::lookup(std::string_view candidate) const {
  assert(frozen);
  std::vector<Profile>::const_iterator const found = std::lower_bound(
      profiles.begin(), profiles.end(), candidate,
      [](Profile const& profile, std::string_view text) {
        return profile.text < text;
      });
  if (found == profiles.end() || found->text != candidate)
    return DfsSoloMasks();
  return found->masks;
}

long double dfs_solo_exact_bonus(
    std::vector<DfsSoloMasks> const& profiles,
    long double word_log_bonus, long double pair_log_bonus) {
  assert(word_log_bonus >= 0.0L && pair_log_bonus >= 0.0L);
  if (profiles.empty()) return 0.0L;
  if (disjoint_profiles(profiles)) {
    long double total = 0.0L;
    for (size_t i = 0; i < profiles.size(); ++i)
      total += local_upper(
          profiles[i], word_log_bonus, pair_log_bonus);
    return total;
  }

  int const segment_count = int(profiles.size());
  int const solo_count = 16;
  int const source = segment_count + solo_count;
  int const sink = source + 1;
  std::vector<std::vector<FlowEdge> > graph(size_t(sink + 1));
  for (int segment = 0; segment < segment_count; ++segment) {
    add_edge(&graph, source, segment, 1, 0);
    uint16_t const word_mask = profiles[size_t(segment)].word_mask;
    uint16_t const pair_mask = profiles[size_t(segment)].pair_mask;
    assert((pair_mask & ~word_mask) == 0);
    for (int solo = 0; solo < solo_count; ++solo) {
      uint16_t const bit = uint16_t(1) << solo;
      if ((word_mask & bit) == 0) continue;
      add_edge(&graph, segment, segment_count + solo, 1,
               (pair_mask & bit) != 0 ? -1 : 0);
    }
  }
  for (int solo = 0; solo < solo_count; ++solo)
    add_edge(&graph, segment_count + solo, sink, 1, 0);

  int total_cost = 0;
  long double best = 0.0L;
  int const infinity = std::numeric_limits<int>::max() / 4;
  for (int cardinality = 1; cardinality <= 16; ++cardinality) {
    std::vector<int> distance(graph.size(), infinity);
    std::vector<int> previous_node(graph.size(), -1);
    std::vector<int> previous_edge(graph.size(), -1);
    distance[size_t(source)] = 0;
    for (size_t pass = 1; pass < graph.size(); ++pass) {
      bool changed = false;
      for (size_t from = 0; from < graph.size(); ++from) {
        if (distance[from] == infinity) continue;
        for (size_t edge = 0; edge < graph[from].size(); ++edge) {
          FlowEdge const& candidate = graph[from][edge];
          if (candidate.capacity == 0) continue;
          int const next = distance[from] + candidate.cost;
          if (next >= distance[size_t(candidate.to)]) continue;
          distance[size_t(candidate.to)] = next;
          previous_node[size_t(candidate.to)] = int(from);
          previous_edge[size_t(candidate.to)] = int(edge);
          changed = true;
        }
      }
      if (!changed) break;
    }
    if (previous_node[size_t(sink)] < 0) break;
    for (int node = sink; node != source;
         node = previous_node[size_t(node)]) {
      int const from = previous_node[size_t(node)];
      int const edge_index = previous_edge[size_t(node)];
      FlowEdge& edge = graph[size_t(from)][size_t(edge_index)];
      --edge.capacity;
      ++graph[size_t(node)][size_t(edge.reverse)].capacity;
    }
    total_cost += distance[size_t(sink)];
    int const high_edges = -total_cost;
    long double const bonus =
        static_cast<long double>(cardinality) * word_log_bonus +
        static_cast<long double>(high_edges) * pair_log_bonus;
    best = std::max(best, bonus);
  }
  return best;
}

double dfs_solo_score_correction(
    std::vector<DfsSoloMasks> const& profiles,
    double word_log_bonus, double pair_log_bonus) {
  assert(isfinite(word_log_bonus) && word_log_bonus >= 0.0);
  assert(isfinite(pair_log_bonus) && pair_log_bonus >= 0.0);
  if (profiles.size() <= 1 || disjoint_profiles(profiles)) return 0.0;

  long double upper = 0.0L;
  for (size_t i = 0; i < profiles.size(); ++i)
    upper += local_upper(
        profiles[i], static_cast<long double>(word_log_bonus),
        static_cast<long double>(pair_log_bonus));
  long double correction = dfs_solo_exact_bonus(
      profiles, static_cast<long double>(word_log_bonus),
      static_cast<long double>(pair_log_bonus)) - upper;
  assert(correction <= 0.0L);
  if (correction > 0.0L) correction = 0.0L;

  double rounded = static_cast<double>(correction);
  if (static_cast<long double>(rounded) > correction)
    rounded = nextafter(rounded, -HUGE_VAL);
  if (rounded > 0.0) rounded = 0.0;
  assert(rounded <= 0.0);
  assert(static_cast<long double>(rounded) <= correction);
  return rounded;
}
