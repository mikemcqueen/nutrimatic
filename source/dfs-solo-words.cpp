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

typedef long double (*PairBonus)(DfsPairBonusKind, void const*);

long double scalar_pair_bonus(DfsPairBonusKind kind, void const* context) {
  if (kind == DFS_PAIR_BONUS_NONE) return 0.0L;
  return *static_cast<long double const*>(context);
}

long double model_pair_bonus(DfsPairBonusKind kind, void const* context) {
  DfsScoreModel const* const model =
      static_cast<DfsScoreModel const*>(context);
  return static_cast<long double>(model->pair_log_bonus(kind));
}

DfsPairBonusKind pair_kind(DfsSoloMasks const& masks, int index) {
  DfsPairBonusKind const kind = masks.pair_kinds[index];
  return kind == DFS_PAIR_BONUS_NONE &&
          (masks.pair_mask & (uint16_t(1) << index)) != 0
      ? DFS_PAIR_BONUS_LEGACY : kind;
}

long double local_upper(DfsSoloMasks const& masks,
                        long double word_log_bonus,
                        PairBonus pair_bonus, void const* context) {
  assert((masks.pair_mask & ~masks.word_mask) == 0);
  long double strongest_pair = 0.0L;
  for (int i = 0; i < 16; ++i)
    strongest_pair = std::max(
        strongest_pair, pair_bonus(pair_kind(masks, i), context));
  if (masks.pair_mask != 0) return word_log_bonus + strongest_pair;
  if (masks.word_mask != 0) return word_log_bonus;
  return 0.0L;
}

double rounded_local_upper(DfsSoloMasks const& masks,
                           double word_log_bonus,
                           PairBonus pair_bonus, void const* context) {
  if (masks.pair_mask != 0) {
    long double strongest_pair = 0.0L;
    for (int i = 0; i < 16; ++i)
      strongest_pair = std::max(
          strongest_pair, pair_bonus(pair_kind(masks, i), context));
    return word_log_bonus + static_cast<double>(strongest_pair);
  }
  return masks.word_mask != 0 ? word_log_bonus : 0.0;
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
  long double bonus_cost;
  int pair_cost;
};

void add_edge(std::vector<std::vector<FlowEdge> >* graph,
              int from, int to, int capacity,
              long double bonus_cost, int pair_cost = 0) {
  FlowEdge const forward = {
    to, int((*graph)[size_t(to)].size()), capacity,
    bonus_cost, pair_cost,
  };
  FlowEdge const reverse = {
    from, int((*graph)[size_t(from)].size()), 0,
    -bonus_cost, -pair_cost,
  };
  (*graph)[size_t(from)].push_back(forward);
  (*graph)[size_t(to)].push_back(reverse);
}

DfsSoloMatching exact_matching(
    std::vector<DfsSoloMasks> const& profiles,
    long double word_log_bonus, PairBonus pair_bonus, void const* context) {
  DfsSoloMatching result;
  result.solo_word_indexes.assign(profiles.size(), DFS_NO_SOLO_WORD);
  if (profiles.empty()) return result;
  if (disjoint_profiles(profiles)) {
    for (size_t i = 0; i < profiles.size(); ++i) {
      long double const bonus = local_upper(
          profiles[i], word_log_bonus, pair_bonus, context);
      result.bonus += bonus;
      if (bonus == 0.0L) continue;
      uint16_t const preferred = profiles[i].pair_mask != 0
          ? profiles[i].pair_mask : profiles[i].word_mask;
      if (profiles[i].pair_mask != 0) {
        long double strongest = -1.0L;
        for (int solo = 0; solo < 16; ++solo) {
          uint16_t const bit = uint16_t(1) << solo;
          if ((preferred & bit) == 0) continue;
          long double const candidate = pair_bonus(
              pair_kind(profiles[i], solo), context);
          if (candidate > strongest) {
            strongest = candidate;
            result.solo_word_indexes[i] = uint8_t(solo);
          }
        }
      } else {
        result.solo_word_indexes[i] =
            uint8_t(__builtin_ctz(unsigned(preferred)));
      }
    }
    return result;
  }

  int const segment_count = int(profiles.size());
  int const solo_count = 16;
  int const source = segment_count + solo_count;
  int const sink = source + 1;
  std::vector<std::vector<FlowEdge> > graph(size_t(sink + 1));
  for (int segment = 0; segment < segment_count; ++segment) {
    add_edge(&graph, source, segment, 1, 0.0L);
    uint16_t const word_mask = profiles[size_t(segment)].word_mask;
    assert((profiles[size_t(segment)].pair_mask & ~word_mask) == 0);
    for (int solo = 0; solo < solo_count; ++solo) {
      uint16_t const bit = uint16_t(1) << solo;
      if ((word_mask & bit) == 0) continue;
      DfsPairBonusKind const kind =
          pair_kind(profiles[size_t(segment)], solo);
      add_edge(&graph, segment, segment_count + solo, 1,
               -pair_bonus(kind, context),
               kind == DFS_PAIR_BONUS_NONE ? 0 : -1);
    }
  }
  for (int solo = 0; solo < solo_count; ++solo)
    add_edge(&graph, segment_count + solo, sink, 1, 0.0L);

  long double total_bonus_cost = 0.0L;
  long double const infinity = std::numeric_limits<long double>::infinity();
  for (int cardinality = 1; cardinality <= 16; ++cardinality) {
    std::vector<long double> distance(graph.size(), infinity);
    std::vector<int> pair_distance(graph.size(), 0);
    std::vector<int> previous_node(graph.size(), -1);
    std::vector<int> previous_edge(graph.size(), -1);
    distance[size_t(source)] = 0.0L;
    for (size_t pass = 1; pass < graph.size(); ++pass) {
      bool changed = false;
      for (size_t from = 0; from < graph.size(); ++from) {
        if (!isfinite(distance[from])) continue;
        for (size_t edge = 0; edge < graph[from].size(); ++edge) {
          FlowEdge const& candidate = graph[from][edge];
          if (candidate.capacity == 0) continue;
          long double const next_bonus =
              distance[from] + candidate.bonus_cost;
          int const next_pair = pair_distance[from] + candidate.pair_cost;
          bool const better =
              next_bonus < distance[size_t(candidate.to)] ||
              (next_bonus == distance[size_t(candidate.to)] &&
               next_pair < pair_distance[size_t(candidate.to)]);
          if (!better) continue;
          distance[size_t(candidate.to)] = next_bonus;
          pair_distance[size_t(candidate.to)] = next_pair;
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
    total_bonus_cost += distance[size_t(sink)];
    long double const bonus =
        static_cast<long double>(cardinality) * word_log_bonus -
        total_bonus_cost;
    if (bonus <= result.bonus) continue;
    result.bonus = bonus;
    std::fill(result.solo_word_indexes.begin(),
              result.solo_word_indexes.end(), DFS_NO_SOLO_WORD);
    for (int segment = 0; segment < segment_count; ++segment) {
      for (size_t edge = 0; edge < graph[size_t(segment)].size(); ++edge) {
        FlowEdge const& candidate = graph[size_t(segment)][edge];
        if (candidate.to < segment_count ||
            candidate.to >= segment_count + solo_count ||
            candidate.capacity != 0)
          continue;
        result.solo_word_indexes[size_t(segment)] =
            uint8_t(candidate.to - segment_count);
        break;
      }
    }
  }
  return result;
}

double score_correction(
    std::vector<DfsSoloMasks> const& profiles,
    double word_log_bonus, PairBonus pair_bonus, void const* context,
    std::vector<uint8_t>* solo_word_indexes) {
  assert(isfinite(word_log_bonus) && word_log_bonus >= 0.0);
  bool const needs_correction =
      profiles.size() > 1 && !disjoint_profiles(profiles);
  if (!needs_correction && solo_word_indexes == NULL) return 0.0;

  DfsSoloMatching matching = exact_matching(
      profiles, static_cast<long double>(word_log_bonus),
      pair_bonus, context);
  if (solo_word_indexes != NULL)
    *solo_word_indexes = std::move(matching.solo_word_indexes);
  if (!needs_correction) return 0.0;

  long double mathematical_upper = 0.0L;
  long double pending_local_upper = 0.0L;
  for (size_t i = 0; i < profiles.size(); ++i) {
    mathematical_upper += local_upper(
        profiles[i], static_cast<long double>(word_log_bonus),
        pair_bonus, context);
    pending_local_upper += static_cast<long double>(rounded_local_upper(
        profiles[i], word_log_bonus, pair_bonus, context));
  }
  long double const exact = matching.bonus;
  assert(exact <= mathematical_upper);
  long double correction = exact - pending_local_upper;
  if (correction > 0.0L) correction = 0.0L;

  double rounded = static_cast<double>(correction);
  if (static_cast<long double>(rounded) > correction)
    rounded = nextafter(rounded, -HUGE_VAL);
  if (rounded > 0.0) rounded = 0.0;
  assert(rounded <= 0.0);
  assert(static_cast<long double>(rounded) <= correction);
  return rounded;
}

}  // namespace

uint16_t dfs_pair_bonus_score_flags(DfsPairBonusKind kind) {
  if (kind == DFS_PAIR_BONUS_NONE) return 0;
  if (kind == DFS_PAIR_BONUS_LEGACY) return DFS_MEMBER_KNOWN_PAIR;
  return uint16_t(kind) << DFS_MEMBER_PAIR_KIND_SHIFT;
}

DfsPairBonusKind dfs_member_pair_bonus_kind(uint16_t score_flags) {
  uint16_t const encoded =
      (score_flags & DFS_MEMBER_PAIR_KIND_MASK) >>
      DFS_MEMBER_PAIR_KIND_SHIFT;
  if (encoded != 0) return DfsPairBonusKind(encoded);
  return (score_flags & DFS_MEMBER_KNOWN_PAIR) != 0
      ? DFS_PAIR_BONUS_LEGACY : DFS_PAIR_BONUS_NONE;
}

DfsPairBonusKind dfs_member_solo_pair_bonus_kind(uint16_t score_flags) {
  uint16_t const encoded =
      (score_flags & DFS_MEMBER_SOLO_PAIR_KIND_MASK) >>
      DFS_MEMBER_SOLO_PAIR_KIND_SHIFT;
  if (encoded != 0) return DfsPairBonusKind(encoded);
  return (score_flags & DFS_MEMBER_SOLO_PAIR_EDGE) != 0
      ? DFS_PAIR_BONUS_LEGACY : DFS_PAIR_BONUS_NONE;
}

uint16_t dfs_solo_score_flags(DfsSoloMasks masks) {
  assert((masks.pair_mask & ~masks.word_mask) == 0);
  uint16_t flags = 0;
  if (masks.word_mask != 0) flags |= DFS_MEMBER_SOLO_WORD_EDGE;
  if (masks.pair_mask != 0) {
    flags |= DFS_MEMBER_SOLO_PAIR_EDGE;
    DfsPairBonusKind strongest = DFS_PAIR_BONUS_NONE;
    for (int i = 0; i < 16; ++i)
      strongest = std::max(strongest, pair_kind(masks, i));
    if (strongest != DFS_PAIR_BONUS_LEGACY)
      flags |= uint16_t(strongest) << DFS_MEMBER_SOLO_PAIR_KIND_SHIFT;
  }
  return flags;
}

DfsSoloWords::DfsSoloWords(
    IndexReader const* reader, std::vector<std::string> const& words,
    std::unordered_set<std::string> const* pairs,
    DfsScoreModel const* score_model,
    DfsPairBonusMap const* weighted_pairs):
    reader(reader),
    words(words),
    pairs(pairs),
    weighted_pairs(weighted_pairs),
    probe_word_edges(score_model != NULL &&
                     score_model->multi_word_log_bonus() != 0.0),
    probe_pair_edges(score_model != NULL &&
                     (pairs != NULL || weighted_pairs != NULL) &&
                     (score_model->multi_word_log_bonus() != 0.0 ||
                      score_model->pair_log_bonus() != 0.0 ||
                      (weighted_pairs != NULL && !weighted_pairs->empty()))),
    word_edges(0),
    pair_edges(0),
    frozen(false) {
  assert(reader != NULL);
  assert(score_model != NULL);
  assert(words.size() <= 16);
  assert(score_model->multi_word_log_bonus() >= 0.0);
  assert(score_model->pair_log_bonus() >= 0.0);
  solo_positions.reserve(words.size());
  for (size_t i = 0; i < words.size(); ++i) {
    SoloPosition saved;
    IndexReader::EntryPosition position;
    saved.present = probe_word_edges &&
        reader->aggregate_entry_position(words[i], &position) &&
        position.continuation != off_t(-1);
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
    DfsPairBonusKind kind = DFS_PAIR_BONUS_NONE;
    if (probe_pair_edges) {
      std::string key(candidate);
      key.push_back(' ');
      key += words[i];
      if (weighted_pairs != NULL) {
        DfsPairBonusMap::const_iterator const found =
            weighted_pairs->find(key);
        if (found != weighted_pairs->end()) kind = found->second;
      }
      if (kind == DFS_PAIR_BONUS_NONE && pairs != NULL &&
          pairs->count(key) != 0)
        kind = DFS_PAIR_BONUS_LEGACY;
    }
    if (kind != DFS_PAIR_BONUS_NONE) {
      result.word_mask |= bit;
      result.pair_mask |= bit;
      result.pair_kinds[i] = kind;
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
      candidate, candidate_continuation, candidate_aggregate_count,
      candidate_continuation != off_t(-1));
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
  return dfs_solo_score_flags(masks);
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

DfsSoloMatching dfs_solo_exact_matching(
    std::vector<DfsSoloMasks> const& profiles,
    long double word_log_bonus, long double pair_log_bonus) {
  assert(word_log_bonus >= 0.0L && pair_log_bonus >= 0.0L);
  return exact_matching(
      profiles, word_log_bonus, scalar_pair_bonus, &pair_log_bonus);
}

DfsSoloMatching dfs_solo_exact_matching(
    std::vector<DfsSoloMasks> const& profiles,
    DfsScoreModel const& score_model) {
  return exact_matching(
      profiles, score_model.multi_word_log_bonus(),
      model_pair_bonus, &score_model);
}

long double dfs_solo_exact_bonus(
    std::vector<DfsSoloMasks> const& profiles,
    long double word_log_bonus, long double pair_log_bonus) {
  return dfs_solo_exact_matching(
      profiles, word_log_bonus, pair_log_bonus).bonus;
}

double dfs_solo_score_correction(
    std::vector<DfsSoloMasks> const& profiles,
    double word_log_bonus, double pair_log_bonus,
    std::vector<uint8_t>* solo_word_indexes) {
  assert(isfinite(pair_log_bonus) && pair_log_bonus >= 0.0);
  long double const scalar = pair_log_bonus;
  return score_correction(
      profiles, word_log_bonus, scalar_pair_bonus, &scalar,
      solo_word_indexes);
}

double dfs_solo_score_correction(
    std::vector<DfsSoloMasks> const& profiles,
    DfsScoreModel const& score_model,
    std::vector<uint8_t>* solo_word_indexes) {
  return score_correction(
      profiles, score_model.multi_word_log_bonus(),
      model_pair_bonus, &score_model, solo_word_indexes);
}
