#ifndef NUTRIMATIC_DFS_OUTPUT_H
#define NUTRIMATIC_DFS_OUTPUT_H

#include <stdint.h>

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dfs-class-list.h"
#include "dfs-score.h"
#include "dfs-solo-words.h"
#include "dfs-solution-sink.h"

struct DfsSpelling {
  double log_score;
  std::string text;
  std::string word_set_key;
  // Byte length of each index entry making up text, in text order. The
  // separating spaces are not counted, so the segments start at successive
  // offsets of length + 1. An entry's own text may contain spaces, so this is
  // the only way back to the segmentation.
  std::vector<uint8_t> segment_lengths;
  // Aligned with segment_lengths when any external partner was selected.
  // Multi-word and unmatched entries contain DFS_NO_SOLO_WORD.
  std::vector<uint8_t> solo_word_indexes;
  // Aligned with segment_lengths only when bonus annotation was requested.
  // The low bit records the word bonus; the remaining bits encode at most one
  // DfsPairBonusKind.
  std::vector<uint8_t> segment_bonus_flags;
};

inline constexpr uint8_t DFS_SEGMENT_WORD_BONUS = 1 << 0;
inline constexpr int DFS_SEGMENT_PAIR_BONUS_SHIFT = 1;
inline constexpr uint8_t DFS_SEGMENT_PAIR_BONUS_MASK = uint8_t(7) << 1;

// Rewrites a spelling's text with a comma between index entries, leaving the
// spaces inside an entry alone. Without solo_words the result is exactly the
// sequence syntax that "query-index --score" parses. With solo_words, selected
// external partners are appended as human-readable parenthetical annotations.
std::string dfs_spelling_entry_list(
    DfsSpelling const& spelling, DfsSoloWords const* solo_words = NULL);

// Renders one W-then-pair-source token per segment, using "-" for a segment
// with no active bonus. segment_bonus_flags must be populated and aligned.
std::string dfs_spelling_bonus_list(DfsSpelling const& spelling);

// The dedup table's payload. The map key (not duplicated here) is the
// word-set key. When the result limit is nonzero, heap_pos is this entry's
// current slot in DfsTopN::heap so a heap swap can fix up both sides in O(1);
// it is not meaningful in unlimited mode.
struct RetainedSpelling {
  std::string text;
  std::vector<uint8_t> segment_lengths;
  std::vector<uint8_t> solo_word_indexes;
  std::vector<uint8_t> segment_bonus_flags;
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
  // The model must be the one phase 2 scored with: a spelling's score is its
  // solution's upper score adjusted by the upper-score difference between
  // each chosen member and its class's member 0. solo_words supplies the exact
  // profiles used to correct a concrete spelling before retention. Bonus
  // metadata is retained only when retain_segment_bonuses is true.
  DfsTopN(DfsClassList const* classes, DfsScoreModel const* model,
          size_t limit, DfsSoloWords const* solo_words = NULL,
          bool retain_segment_bonuses = false);

  void emit(std::vector<size_t> const& class_indexes,
            double representative_upper_log_score);
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
  DfsScoreModel const* const score_model;
  DfsSoloWords const* const solo_words;
  size_t const result_limit;
  bool const retain_segment_bonuses;
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
