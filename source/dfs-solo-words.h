#ifndef NUTRIMATIC_DFS_SOLO_WORDS_H
#define NUTRIMATIC_DFS_SOLO_WORDS_H

#include <stdint.h>
#include <sys/types.h>

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

class IndexReader;
class DfsScoreModel;

enum DfsMemberScoreFlag {
  DFS_MEMBER_KNOWN_PAIR = 1 << 0,
  DFS_MEMBER_SOLO_WORD_EDGE = 1 << 1,
  DFS_MEMBER_SOLO_PAIR_EDGE = 1 << 2,
};

inline constexpr uint8_t DFS_NO_SOLO_WORD = UINT8_MAX;

struct DfsSoloMasks {
  uint16_t word_mask = 0;
  uint16_t pair_mask = 0;
};

uint16_t dfs_solo_score_flags(DfsSoloMasks masks);

// Resolves the score edges from extracted single-word candidates to at most
// sixteen external solo words. Registration is phase-1-only; freeze() sorts
// the compact table before any search worker can read it.
class DfsSoloWords {
 public:
  DfsSoloWords(IndexReader const* reader,
               std::vector<std::string> const& words,
               std::unordered_set<std::string> const* pairs,
               DfsScoreModel const* score_model);

  DfsSoloMasks resolve(
      std::string_view candidate,
      off_t candidate_continuation,
      int64_t candidate_aggregate_count) const;
  DfsSoloMasks resolve(std::string_view candidate) const;
  uint16_t register_profile(
      std::string_view candidate,
      off_t candidate_continuation,
      int64_t candidate_aggregate_count);
  void freeze();
  DfsSoloMasks lookup(std::string_view candidate) const;

  size_t profile_count() const { return profiles.size(); }
  size_t word_edge_count() const { return word_edges; }
  size_t pair_edge_count() const { return pair_edges; }
  std::string const& word(size_t index) const { return words[index]; }

 private:
  struct SoloPosition {
    off_t continuation;
    int64_t aggregate_count;
    bool present;
  };
  struct Profile {
    std::string text;
    DfsSoloMasks masks;
  };

  DfsSoloMasks resolve_from_position(
      std::string_view candidate,
      off_t candidate_continuation,
      int64_t candidate_aggregate_count,
      bool candidate_present) const;

  IndexReader const* const reader;
  std::vector<std::string> const words;
  std::unordered_set<std::string> const* const pairs;
  bool const probe_word_edges;
  bool const probe_pair_edges;
  std::vector<SoloPosition> solo_positions;
  std::vector<Profile> profiles;
  size_t word_edges;
  size_t pair_edges;
  bool frozen;
};

struct DfsSoloMatching {
  long double bonus = 0.0L;
  // One solo-word index per input profile, or DFS_NO_SOLO_WORD when that
  // profile is unmatched in the selected maximum-score assignment.
  std::vector<uint8_t> solo_word_indexes;
};

// Pure graph routines. The exact bonus maximizes over every matching
// cardinality, so zero-weight ordinary edges never displace positive pair
// edges. The correction is conservatively rounded and is always non-positive.
long double dfs_solo_exact_bonus(
    std::vector<DfsSoloMasks> const& profiles,
    long double word_log_bonus, long double pair_log_bonus);
DfsSoloMatching dfs_solo_exact_matching(
    std::vector<DfsSoloMasks> const& profiles,
    long double word_log_bonus, long double pair_log_bonus);
double dfs_solo_score_correction(
    std::vector<DfsSoloMasks> const& profiles,
    double word_log_bonus, double pair_log_bonus,
    std::vector<uint8_t>* solo_word_indexes = NULL);

#endif
