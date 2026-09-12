#ifndef NUTRIMATIC_DFS_SOLO_WORDS_H
#define NUTRIMATIC_DFS_SOLO_WORDS_H

#include <stdint.h>
#include <sys/types.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class IndexReader;
class DfsScoreModel;

enum DfsMemberScoreFlag {
  DFS_MEMBER_KNOWN_PAIR = 1 << 0,
  DFS_MEMBER_SOLO_WORD_EDGE = 1 << 1,
  DFS_MEMBER_SOLO_PAIR_EDGE = 1 << 2,
};

// Fixed positive-pair sources. Legacy --pairs keeps its operator-selected
// --pair-bonus, while the three workflow tiers use their calibrated bonuses.
// The ordering is meaningful: duplicate weighted inputs retain the strongest
// source by taking the maximum kind.
enum DfsPairBonusKind : uint8_t {
  DFS_PAIR_BONUS_NONE = 0,
  DFS_PAIR_BONUS_LEGACY = 1,
  DFS_PAIR_BONUS_SEED = 2,
  DFS_PAIR_BONUS_YES = 3,
  DFS_PAIR_BONUS_BEST = 4,
};

typedef std::unordered_map<std::string, DfsPairBonusKind> DfsPairBonusMap;

inline constexpr int DFS_MEMBER_PAIR_KIND_SHIFT = 3;
inline constexpr uint16_t DFS_MEMBER_PAIR_KIND_MASK = uint16_t(7) << 3;
inline constexpr int DFS_MEMBER_SOLO_PAIR_KIND_SHIFT = 6;
inline constexpr uint16_t DFS_MEMBER_SOLO_PAIR_KIND_MASK = uint16_t(7) << 6;

uint16_t dfs_pair_bonus_score_flags(DfsPairBonusKind kind);
DfsPairBonusKind dfs_member_pair_bonus_kind(uint16_t score_flags);
DfsPairBonusKind dfs_member_solo_pair_bonus_kind(uint16_t score_flags);

inline constexpr uint8_t DFS_NO_SOLO_WORD = UINT8_MAX;

struct DfsSoloMasks {
  uint16_t word_mask = 0;
  uint16_t pair_mask = 0;
  DfsPairBonusKind pair_kinds[16] = {};
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
               DfsScoreModel const* score_model,
               DfsPairBonusMap const* weighted_pairs = NULL);

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
  DfsPairBonusMap const* const weighted_pairs;
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

// Weighted-source forms used by production scoring. The scalar overloads
// above remain the compact public surface for legacy callers and unit tests.
DfsSoloMatching dfs_solo_exact_matching(
    std::vector<DfsSoloMasks> const& profiles,
    DfsScoreModel const& score_model);
double dfs_solo_score_correction(
    std::vector<DfsSoloMasks> const& profiles,
    DfsScoreModel const& score_model,
    std::vector<uint8_t>* solo_word_indexes = NULL);

#endif
