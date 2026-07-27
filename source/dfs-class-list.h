#ifndef NUTRIMATIC_DFS_CLASS_LIST_H
#define NUTRIMATIC_DFS_CLASS_LIST_H

#include <stdint.h>

#include <array>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

class IndexReader;

// Phase 1 of dfs-anagrams: extract every makeable corpus segment, then collapse
// spellings with the same letter multiset into one class for the phase-2 DFS.
static int const DFS_SYMBOL_COUNT = 36;

typedef std::unordered_set<std::string> DfsDictionary;

int dfs_symbol_index(unsigned char ch);

struct DfsClassMember {
  std::string text;  // no trailing space; internal phrase spaces are retained
  int64_t count;
  int word_count;
};

struct DfsAnagramClass {
  std::string key;  // sorted letters, with spaces removed
  std::vector<std::pair<uint8_t, uint32_t> > letters;
  std::vector<DfsClassMember> members;  // highest count first
  int rarest_rank;
};

class DfsClassList {
 public:
  // letters must contain only lowercase a-z and digits. Phrases are extracted
  // by default, up to the cap implied by min_word_len and the bag length.
  // include_phrases=false exists for words-only validation against the phase-0
  // reference counts; dfs-anagrams leaves it at its production default. The
  // optional dictionary is borrowed and restricts every emitted phrase word.
  DfsClassList(IndexReader const* reader, std::string const& letters,
               int min_word_len, bool include_phrases = true,
               DfsDictionary const* dictionary = NULL);

  std::vector<DfsAnagramClass> const& classes() const { return class_list; }
  size_t entry_count() const { return entries; }
  int64_t nodes_visited() const { return nodes; }
  int min_word_length() const { return minimum_word_len; }

  // Dictionary frequency: occurrences across distinct extracted spellings,
  // not corpus-weighted counts.
  std::array<int64_t, DFS_SYMBOL_COUNT> const& letter_frequencies() const {
    return frequencies;
  }
  std::array<int, DFS_SYMBOL_COUNT> const& rank_to_symbol() const {
    return symbols_by_rank;
  }
  std::array<int, DFS_SYMBOL_COUNT> const& symbol_to_rank() const {
    return ranks_by_symbol;
  }

  // Classes are stored in rarest-letter buckets. This range is the O(1)
  // candidate lookup used when symbol is the rarest one left in the DFS bag.
  size_t candidate_begin(int symbol) const;
  size_t candidate_end(int symbol) const;

 private:
  std::vector<DfsAnagramClass> class_list;
  std::array<int64_t, DFS_SYMBOL_COUNT> frequencies;
  std::array<int, DFS_SYMBOL_COUNT> symbols_by_rank;
  std::array<int, DFS_SYMBOL_COUNT> ranks_by_symbol;
  std::array<size_t, DFS_SYMBOL_COUNT + 1> bucket_starts;
  int minimum_word_len;
  size_t entries;
  int64_t nodes;
};

#endif
