#ifndef NUTRIMATIC_DFS_CLASS_LIST_H
#define NUTRIMATIC_DFS_CLASS_LIST_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "dfs-alloc.h"

class IndexReader;

// Phase 1 of dfs-anagrams: extract every makeable corpus segment, then collapse
// spellings with the same letter multiset into one class for the phase-2 DFS.
static int const DFS_SYMBOL_COUNT = 36;

// The widest bag phase 1 represents. A stored spelling is at most
// letters + letters / min_word_len - 1 bytes, which at the worst case -m 1 is
// 128 + 127 = 255 exactly, so this is what makes DfsPackedMember::text_length a
// uint8_t; it also bounds key_length and word_count. Flat rather than
// min_word_len-dependent so one number covers every -m.
static size_t const DFS_MAX_BAG_LETTERS = 128;

typedef std::unordered_set<std::string> DfsDictionary;

int dfs_symbol_index(unsigned char ch);

inline char dfs_symbol_char(int symbol) {
  return symbol < 26 ? char('a' + symbol) : char('0' + symbol - 26);
}

// One extracted spelling. text points into the class list's text arena and is
// not NUL terminated; internal phrase spaces are retained, the trailing one is
// not.
struct DfsPackedMember {         // 16 bytes
  char const* text;
  uint32_t count;
  uint8_t text_length;
  uint8_t word_count;
  uint16_t reserved;             // tail padding, named
};

// One anagram class. Its letters are not stored: they decode from the
// mixed-radix signature on demand, which is why letters_count is here but the
// letters themselves are not.
struct DfsClassRecord {          // 24 bytes
  uint64_t signature;            // mixed-radix subbag code
  DfsPackedMember const* members;  // member_count entries, highest count first
  uint8_t member_count;
  uint8_t key_length;            // total letters
  uint8_t letters_count;         // distinct symbols
  uint8_t rarest_rank;           // 0..36, 36 = none
  uint32_t reserved;             // tail padding, named
};

struct DfsClassSpan {
  DfsClassRecord const* data;
  size_t count;

  size_t size() const { return count; }
  bool empty() const { return count == 0; }
  DfsClassRecord const& operator[](size_t index) const { return data[index]; }
};

struct DfsMemberSpan {
  DfsPackedMember* data;
  size_t count;

  size_t size() const { return count; }
};

struct DfsMemberView {
  char const* text;
  size_t text_length;
  int64_t count;
  int word_count;
};

// A decoded letter requirement: (count << 6) | symbol. The count occupies the
// top ten bits, capping one symbol at 1023, comfortably above the bag cap.
inline int dfs_class_letter_symbol(uint16_t entry) {
  return int(entry & 0x3f);
}

inline uint32_t dfs_class_letter_count(uint16_t entry) {
  return uint32_t(entry >> 6);
}

// Orders packed text exactly as std::string::operator< orders the spellings it
// replaced: memcmp over the shorter length, then shorter is less. Any other
// convention is a different but still valid total order, so member order inside
// a class would silently diverge from the std::string era.
inline int dfs_member_text_compare(
    DfsPackedMember const& a, DfsPackedMember const& b) {
  size_t const shorter =
      std::min<size_t>(a.text_length, b.text_length);
  int const order = memcmp(a.text, b.text, shorter);
  if (order != 0) return order;
  if (a.text_length == b.text_length) return 0;
  return a.text_length < b.text_length ? -1 : 1;
}

// One symbol present in the input bag, with the place value of its digit in a
// class signature. Ascending by symbol.
struct DfsSignatureDigit {
  uint64_t multiplier;
  uint8_t symbol;
};

class DfsClassList {
 public:
  // letters must contain only lowercase a-z and digits, and must be no longer
  // than DFS_MAX_BAG_LETTERS. Phrases are extracted by default, up to the cap
  // implied by min_word_len and the bag length. include_phrases=false exists
  // for words-only validation against the phase-0 reference counts;
  // dfs-anagrams leaves it at its production default. The optional dictionary
  // is borrowed and restricts every emitted phrase word. max_extract_words
  // caps the words in one extracted entry; 0 means no cap beyond the one the
  // bag and min_word_len already imply.
  DfsClassList(IndexReader const* reader, std::string const& letters,
               int min_word_len, bool include_phrases = true,
               DfsDictionary const* dictionary = NULL,
               int max_extract_words = 0);

  DfsClassSpan classes() const {
    DfsClassSpan const span = { class_records.get(), class_count };
    return span;
  }
  size_t entry_count() const { return entries; }
  int64_t nodes_visited() const { return nodes; }
  int min_word_length() const { return minimum_word_len; }

  // Writes (count << 6) | symbol in ascending symbol order and returns the
  // number written, which equals classes()[ci].letters_count. out must hold
  // DFS_SYMBOL_COUNT entries; a stack buffer will do, since this allocates
  // nothing.
  size_t decode_class_letters(size_t ci, uint16_t* out) const;

  size_t class_length(size_t ci) const;
  size_t member_count(size_t ci) const;
  DfsMemberView member(size_t ci, size_t mi) const;

  // Rebuilds a class's sorted spelling. Test and debug only: it allocates, and
  // it returns symbol order, so digits follow letters rather than preceding
  // them as raw character order would put them.
  std::string class_key(size_t ci) const;

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

  // Moves to the front of the member arena every member of every class whose
  // keep_class entry is true, dropping multi-word members when words_only, and
  // returns a mutable span of the survivors. This reorders members across class
  // boundaries, so it drops the class -> member grouping; the survivors stay
  // valid until release_members().
  DfsMemberSpan retain_members(
      std::vector<bool> const& keep_class, bool words_only);

  // Drops the class -> member grouping: zeroes every record's members pointer
  // and member_count, and latches the flag. The member records and their text
  // stay alive, because output is about to read them.
  void invalidate_members();
  bool members_invalidated() const { return grouping_dropped; }

  // Frees the member arena and the text arena outright. Legal only once no
  // spelling will be read again, so the caller must already have copied out
  // whatever it prints.
  void release_members();

 private:
  size_t decode_signature(uint64_t signature, uint16_t* out) const;

  std::vector<std::unique_ptr<char, DfsAlignedFree> > text_chunks;
  std::unique_ptr<DfsPackedMember, DfsAlignedFree> members_arena;
  std::unique_ptr<DfsClassRecord, DfsAlignedFree> class_records;
  size_t class_count;
  std::vector<DfsSignatureDigit> signature_digits;
  std::array<int64_t, DFS_SYMBOL_COUNT> frequencies;
  std::array<int, DFS_SYMBOL_COUNT> symbols_by_rank;
  std::array<int, DFS_SYMBOL_COUNT> ranks_by_symbol;
  std::array<size_t, DFS_SYMBOL_COUNT + 1> bucket_starts;
  int minimum_word_len;
  size_t entries;
  int64_t nodes;
  bool grouping_dropped;
};

#endif
