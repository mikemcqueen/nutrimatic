#include "dfs-class-list.h"

#include "dfs-diagnostic.h"
#include "index.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

int dfs_symbol_index(unsigned char ch) {
  if (ch >= 'a' && ch <= 'z') return ch - 'a';
  if (ch >= '0' && ch <= '9') return 26 + ch - '0';
  return -1;
}

namespace {

size_t const CHUNK_BYTES = size_t(64) * 1024 * 1024;
size_t const INITIAL_TABLE_SLOTS = 1024;

[[noreturn]] void fatal_allocation(char const* what) {
  dfs_diagnostic_to_stream(
      stderr, "error: phase 1 could not allocate %s\n", what);
  abort();
}

struct IntermediateMember {      // 24 bytes
  char const* text;
  uint32_t class_id;
  uint32_t count;
  uint8_t text_length;
  uint8_t word_count;
  uint16_t reserved;
};

// Appends fixed-size records, or byte runs, to chunks that are allocated once
// and never move, so pointers into them stay valid for the arena's life. The
// element count per chunk is what keeps a record from straddling a boundary.
class ChunkedArena {
 public:
  explicit ChunkedArena(size_t element_bytes):
      element_bytes(element_bytes),
      chunk_elements(std::max<size_t>(CHUNK_BYTES / element_bytes, 1)),
      cursor(NULL),
      left(0) { }

  void* append(size_t count) {
    assert(count != 0 && count <= chunk_elements);
    if (count > left && !add_chunk()) return NULL;
    void* const result = cursor;
    cursor += count * element_bytes;
    left -= count;
    used.back() += count;
    return result;
  }

  size_t chunk_count() const { return chunks.size(); }
  char const* chunk(size_t index) const { return chunks[index].get(); }
  size_t chunk_used(size_t index) const { return used[index]; }

  std::vector<std::unique_ptr<char, DfsAlignedFree> >& storage() {
    return chunks;
  }

  void clear() {
    chunks.clear();
    used.clear();
    cursor = NULL;
    left = 0;
  }

 private:
  bool add_chunk() {
    void* const raw = dfs_allocate_aligned(chunk_elements * element_bytes);
    if (raw == NULL) return false;
    chunks.push_back(std::unique_ptr<char, DfsAlignedFree>(
        static_cast<char*>(raw)));
    used.push_back(0);
    cursor = static_cast<char*>(raw);
    left = chunk_elements;
    return true;
  }

  size_t const element_bytes;
  size_t const chunk_elements;
  std::vector<std::unique_ptr<char, DfsAlignedFree> > chunks;
  std::vector<size_t> used;
  char* cursor;
  size_t left;
};

// Open-addressed signature -> dense class id, as two slot-indexed parallel
// arrays: 12 bytes per slot rather than the 16 an eight-byte-aligned struct
// would cost. A signature identifies a class exactly, so a probe is an integer
// compare with no key comparison.
class SignatureTable {
 public:
  SignatureTable(): slot_count(0), slot_mask(0), classes(0) { }

  bool intern(uint64_t signature, uint32_t* id) {
    if (classes * 5 >= slot_count * 3 &&
        !grow(slot_count == 0 ? INITIAL_TABLE_SLOTS : slot_count * 2))
      return false;

    uint64_t const key = signature + 1;
    uint64_t* const keys = key_array.get();
    uint32_t* const ids = id_array.get();
    size_t slot = size_t(mix(key)) & slot_mask;
    while (keys[slot] != 0) {
      if (keys[slot] == key) {
        *id = ids[slot];
        return true;
      }
      slot = (slot + 1) & slot_mask;
    }
    DFS_CHECK(classes < UINT32_MAX);
    keys[slot] = key;
    ids[slot] = uint32_t(classes);
    *id = uint32_t(classes);
    ++classes;
    return true;
  }

  size_t class_count() const { return classes; }
  size_t slots() const { return slot_count; }
  uint64_t const* keys() const { return key_array.get(); }
  uint32_t const* ids() const { return id_array.get(); }

  void release() {
    key_array.reset();
    id_array.reset();
    slot_count = 0;
    slot_mask = 0;
  }

 private:
  static uint64_t mix(uint64_t value) {
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
  }

  bool grow(size_t slots) {
    void* const raw_keys =
        dfs_allocate_aligned(slots * sizeof(uint64_t));
    if (raw_keys == NULL) return false;
    std::unique_ptr<uint64_t, DfsAlignedFree> new_keys(
        static_cast<uint64_t*>(raw_keys));
    void* const raw_ids = dfs_allocate_aligned(slots * sizeof(uint32_t));
    if (raw_ids == NULL) return false;
    std::unique_ptr<uint32_t, DfsAlignedFree> new_ids(
        static_cast<uint32_t*>(raw_ids));

    memset(new_keys.get(), 0, slots * sizeof(uint64_t));
    size_t const new_mask = slots - 1;
    for (size_t old = 0; old < slot_count; ++old) {
      uint64_t const key = key_array.get()[old];
      if (key == 0) continue;
      size_t slot = size_t(mix(key)) & new_mask;
      while (new_keys.get()[slot] != 0) slot = (slot + 1) & new_mask;
      new_keys.get()[slot] = key;
      new_ids.get()[slot] = id_array.get()[old];
    }

    key_array = std::move(new_keys);
    id_array = std::move(new_ids);
    slot_count = slots;
    slot_mask = new_mask;
    return true;
  }

  std::unique_ptr<uint64_t, DfsAlignedFree> key_array;
  std::unique_ptr<uint32_t, DfsAlignedFree> id_array;
  size_t slot_count;
  size_t slot_mask;
  size_t classes;
};

// Production form of measure-f's Extractor. It follows only trie edges allowed
// by the remaining bag and emits at every terminating space, continuing beyond
// it when enough letters remain for another word.
class DfsExtractor {
 public:
  DfsExtractor(IndexReader const* reader, std::string const& letters,
               int min_word_len, bool include_phrases,
               DfsDictionary const* dictionary):
      text_arena(1),
      member_arena(sizeof(IntermediateMember)),
      entries(0),
      reader(reader),
      min_len(std::max(min_word_len, 1)),
      max_words(include_phrases ? int(letters.size()) / min_len : 1),
      dictionary(dictionary),
      letters_left(int(letters.size())),
      nodes(0),
      signature(0) {
    bag.fill(0);
    for (size_t i = 0; i < letters.size(); ++i) {
      unsigned char const ch = (unsigned char) letters[i];
      assert(dfs_symbol_index(ch) >= 0);
      ++bag[ch];
    }
    choices.resize(letters.size() + size_t(max_words) + 2);

    multiplier_by_char.fill(0);
    uint64_t place = 1;
    for (int symbol = 0; symbol < DFS_SYMBOL_COUNT; ++symbol) {
      unsigned char const ch = (unsigned char) dfs_symbol_char(symbol);
      int const limit = bag[ch];
      if (limit == 0) continue;
      DfsSignatureDigit const entry = { place, uint8_t(symbol) };
      digits.push_back(entry);
      multiplier_by_char[ch] = place;
      if (!dfs_checked_multiply_u64(place, uint64_t(limit) + 1, &place)) {
        dfs_diagnostic_to_stream(stderr,
            "error: %zu-letter bag needs a class signature wider than 64 bits"
            " (overflowed multiplying radix %d for symbol '%c')\n",
            letters.size(), limit + 1, ch);
        abort();
      }
    }
  }

  void run() {
    text.clear();
    walk(reader->root(), reader->count(), 0, 0, 0);
  }

  int64_t nodes_visited() const { return nodes; }

  SignatureTable table;
  std::vector<uint32_t> class_members;
  std::vector<DfsSignatureDigit> digits;
  ChunkedArena text_arena;
  ChunkedArena member_arena;
  size_t entries;

 private:
  void emit(int64_t count, int word_count) {
    if (count > int64_t(UINT32_MAX)) {
      dfs_diagnostic_to_stream(stderr,
          "error: corpus count %lld for \"%.*s\" exceeds the %llu a packed"
          " member record holds\n",
          (long long) count, int(text.size() - 1), text.data(),
          (unsigned long long) UINT32_MAX);
      abort();
    }

    size_t const length = text.size() - 1;
    DFS_CHECK(length <= UINT8_MAX && word_count <= UINT8_MAX);
    char* const stored = static_cast<char*>(text_arena.append(length));
    if (stored == NULL) fatal_allocation("spelling text");
    memcpy(stored, text.data(), length);

    uint32_t id;
    if (!table.intern(signature, &id))
      fatal_allocation("the class signature table");
    if (size_t(id) == class_members.size())
      class_members.push_back(1);
    else
      ++class_members[id];

    IntermediateMember* const record =
        static_cast<IntermediateMember*>(member_arena.append(1));
    if (record == NULL) fatal_allocation("an intermediate member record");
    record->text = stored;
    record->class_id = id;
    record->count = uint32_t(count);
    record->text_length = uint8_t(length);
    record->word_count = uint8_t(word_count);
    record->reserved = 0;
    ++entries;
  }

  void walk(IndexReader::Node node, int64_t count, int word_len,
            int words, size_t depth) {
    ++nodes;

    IndexReader::CharSet allowed;
    allowed.clear();
    for (int ch = 0; ch < 256; ++ch)
      if (bag[size_t(ch)] > 0) allowed.set((unsigned char) ch);
    if (word_len >= min_len) allowed.set(' ');

    std::vector<IndexReader::Choice>& here = choices[depth];
    here.clear();
    reader->children(node, count, allowed, &here);

    for (size_t i = 0; i < here.size(); ++i) {
      IndexReader::Choice const choice = here[i];
      if (choice.ch == ' ') {
        if (dictionary != NULL &&
            dictionary->find(
                text.substr(text.size() - size_t(word_len),
                            size_t(word_len))) == dictionary->end())
          continue;
        text.push_back(' ');
        emit(choice.count, words + 1);
        if (words + 1 < max_words && letters_left >= min_len &&
            choice.next != IndexReader::Node(-1))
          walk(choice.next, choice.count, 0, words + 1, depth + 1);
        text.pop_back();
        continue;
      }

      unsigned char const ch = (unsigned char) choice.ch;
      --bag[ch];
      --letters_left;
      signature += multiplier_by_char[ch];
      text.push_back(choice.ch);
      walk(choice.next, choice.count, word_len + 1, words, depth + 1);
      text.pop_back();
      signature -= multiplier_by_char[ch];
      ++letters_left;
      ++bag[ch];
    }
  }

  IndexReader const* const reader;
  int const min_len;
  int const max_words;
  DfsDictionary const* const dictionary;
  std::array<int, 256> bag;
  std::array<uint64_t, 256> multiplier_by_char;
  int letters_left;
  std::string text;
  std::vector<std::vector<IndexReader::Choice> > choices;
  int64_t nodes;
  uint64_t signature;
};

bool member_order(DfsPackedMember const& a, DfsPackedMember const& b) {
  if (a.count != b.count) return a.count > b.count;
  int const order = dfs_member_text_compare(a, b);
  if (order != 0) return order < 0;
  return a.word_count < b.word_count;
}

bool same_member(DfsPackedMember const& a, DfsPackedMember const& b) {
  return a.count == b.count && a.word_count == b.word_count &&
      a.text_length == b.text_length &&
      memcmp(a.text, b.text, a.text_length) == 0;
}

std::string letters_key(uint16_t const* letters, size_t count) {
  std::string key;
  for (size_t i = 0; i < count; ++i) {
    key.append(size_t(dfs_class_letter_count(letters[i])),
               dfs_symbol_char(dfs_class_letter_symbol(letters[i])));
  }
  return key;
}

}  // namespace

DfsClassList::DfsClassList(IndexReader const* reader,
                           std::string const& letters,
                           int min_word_len, bool include_phrases,
                           DfsDictionary const* dictionary):
    class_count(0),
    minimum_word_len(std::max(min_word_len, 1)),
    entries(0),
    nodes(0),
    grouping_dropped(false) {
  static_assert(sizeof(DfsClassRecord) == 24,
                "class records must stay 24 bytes; the whole footprint "
                "argument is per-class");
  static_assert(sizeof(DfsPackedMember) == 16,
                "member records must stay four per cache line");
  static_assert(sizeof(IntermediateMember) == 24,
                "the scatter peak is sized on 24-byte intermediates");

  frequencies.fill(0);
  symbols_by_rank.fill(0);
  ranks_by_symbol.fill(0);
  bucket_starts.fill(0);

  if (letters.size() > DFS_MAX_BAG_LETTERS) {
    dfs_diagnostic_to_stream(stderr,
        "error: %zu-letter bag exceeds the %zu phase 1 can represent\n",
        letters.size(), DFS_MAX_BAG_LETTERS);
    abort();
  }

  DfsExtractor extractor(
      reader, letters, minimum_word_len, include_phrases, dictionary);
  extractor.run();
  nodes = extractor.nodes_visited();
  signature_digits = std::move(extractor.digits);
  text_chunks = std::move(extractor.text_arena.storage());

  class_count = extractor.table.class_count();
  if (class_count != 0) {
    void* const raw =
        dfs_allocate_aligned(class_count * sizeof(DfsClassRecord));
    if (raw == NULL) fatal_allocation("the class record array");
    class_records.reset(static_cast<DfsClassRecord*>(raw));
    uint64_t const* const keys = extractor.table.keys();
    uint32_t const* const ids = extractor.table.ids();
    for (size_t slot = 0; slot < extractor.table.slots(); ++slot)
      if (keys[slot] != 0)
        class_records.get()[ids[slot]].signature = keys[slot] - 1;
  }
  extractor.table.release();

  DFS_CHECK(extractor.entries <= UINT32_MAX);
  if (extractor.entries != 0) {
    void* const raw =
        dfs_allocate_aligned(extractor.entries * sizeof(DfsPackedMember));
    if (raw == NULL) fatal_allocation("the member arena");
    members_arena.reset(static_cast<DfsPackedMember*>(raw));
  }
  DfsPackedMember* const members = members_arena.get();

  std::vector<uint32_t>& class_sizes = extractor.class_members;
  assert(class_sizes.size() == class_count);
  std::vector<uint32_t> base(class_count, 0);
  {
    uint32_t running = 0;
    for (size_t id = 0; id < class_count; ++id) {
      base[id] = running;
      running += class_sizes[id];
    }
  }

  for (size_t chunk = 0; chunk < extractor.member_arena.chunk_count();
       ++chunk) {
    IntermediateMember const* const source =
        reinterpret_cast<IntermediateMember const*>(
            extractor.member_arena.chunk(chunk));
    size_t const used = extractor.member_arena.chunk_used(chunk);
    for (size_t i = 0; i < used; ++i) {
      DfsPackedMember& target = members[base[source[i].class_id]++];
      target.text = source[i].text;
      target.count = source[i].count;
      target.text_length = source[i].text_length;
      target.word_count = source[i].word_count;
      target.reserved = 0;
    }
  }
  extractor.member_arena.clear();

  size_t write = 0;
  for (size_t id = 0; id < class_count; ++id) {
    DfsPackedMember* const first = members + base[id] - class_sizes[id];
    DfsPackedMember* const end = first + class_sizes[id];
    std::sort(first, end, member_order);
    size_t const kept = size_t(std::unique(first, end, same_member) - first);
    if (members + write != first)
      memmove(members + write, first, kept * sizeof(DfsPackedMember));
    base[id] = uint32_t(write);
    class_sizes[id] = uint32_t(kept);
    write += kept;
  }
  entries = write;

  for (size_t i = 0; i < entries; ++i) {
    DfsPackedMember const& packed = members[i];
    for (size_t j = 0; j < packed.text_length; ++j) {
      int const symbol = dfs_symbol_index((unsigned char) packed.text[j]);
      if (symbol >= 0) ++frequencies[size_t(symbol)];
    }
  }

  for (int symbol = 0; symbol < DFS_SYMBOL_COUNT; ++symbol)
    symbols_by_rank[size_t(symbol)] = symbol;
  std::sort(symbols_by_rank.begin(), symbols_by_rank.end(),
            [&](int a, int b) {
    if (frequencies[size_t(a)] != frequencies[size_t(b)])
      return frequencies[size_t(a)] < frequencies[size_t(b)];
    return a < b;
  });
  for (int rank = 0; rank < DFS_SYMBOL_COUNT; ++rank)
    ranks_by_symbol[size_t(symbols_by_rank[size_t(rank)])] = rank;

  DfsClassRecord* const records = class_records.get();
  uint16_t decoded[DFS_SYMBOL_COUNT];
  for (size_t id = 0; id < class_count; ++id) {
    DfsClassRecord& record = records[id];
    size_t const count = decode_signature(record.signature, decoded);
    size_t key_length = 0;
    int rarest = DFS_SYMBOL_COUNT;
    for (size_t i = 0; i < count; ++i) {
      key_length += dfs_class_letter_count(decoded[i]);
      rarest = std::min(
          rarest,
          ranks_by_symbol[size_t(dfs_class_letter_symbol(decoded[i]))]);
    }
    if (class_sizes[id] > UINT8_MAX) {
      std::string const key = letters_key(decoded, count);
      dfs_diagnostic_to_stream(stderr,
          "error: class \"%s\" has %u members, %u maximum\n",
          key.c_str(), (unsigned) class_sizes[id], (unsigned) UINT8_MAX);
      abort();
    }
    record.members = members + base[id];
    record.member_count = uint8_t(class_sizes[id]);
    record.key_length = uint8_t(key_length);
    record.letters_count = uint8_t(count);
    record.rarest_rank = uint8_t(rarest);
    record.reserved = 0;
  }

  std::sort(records, records + class_count,
            [](DfsClassRecord const& a, DfsClassRecord const& b) {
    if (a.rarest_rank != b.rarest_rank)
      return a.rarest_rank < b.rarest_rank;
    if (a.key_length != b.key_length) return a.key_length > b.key_length;
    return a.signature < b.signature;
  });

  size_t pos = 0;
  for (int rank = 0; rank < DFS_SYMBOL_COUNT; ++rank) {
    bucket_starts[size_t(rank)] = pos;
    while (pos < class_count && records[pos].rarest_rank == rank) ++pos;
  }
  bucket_starts[DFS_SYMBOL_COUNT] = class_count;
}

size_t DfsClassList::decode_signature(
    uint64_t signature, uint16_t* out) const {
  size_t written = 0;
  uint64_t remainder = signature;
  for (size_t i = signature_digits.size(); i != 0 && remainder != 0; --i) {
    DfsSignatureDigit const& digit = signature_digits[i - 1];
    uint64_t const value = remainder / digit.multiplier;
    remainder %= digit.multiplier;
    if (value == 0) continue;
    assert(value <= UINT16_MAX >> 6);
    out[written++] = uint16_t((value << 6) | digit.symbol);
  }
  std::reverse(out, out + written);
  return written;
}

size_t DfsClassList::decode_class_letters(size_t ci, uint16_t* out) const {
  assert(ci < class_count);
  return decode_signature(class_records.get()[ci].signature, out);
}

size_t DfsClassList::class_length(size_t ci) const {
  assert(ci < class_count);
  return class_records.get()[ci].key_length;
}

size_t DfsClassList::member_count(size_t ci) const {
  assert(ci < class_count);
  assert(!grouping_dropped);
  return class_records.get()[ci].member_count;
}

DfsMemberView DfsClassList::member(size_t ci, size_t mi) const {
  assert(ci < class_count);
  assert(!grouping_dropped);
  DfsClassRecord const& record = class_records.get()[ci];
  assert(mi < record.member_count);
  DfsPackedMember const& packed = record.members[mi];
  DfsMemberView const view = {
    packed.text, packed.text_length, int64_t(packed.count),
    int(packed.word_count),
  };
  return view;
}

std::string DfsClassList::class_key(size_t ci) const {
  uint16_t decoded[DFS_SYMBOL_COUNT];
  size_t const count = decode_class_letters(ci, decoded);
  return letters_key(decoded, count);
}

size_t DfsClassList::candidate_begin(int symbol) const {
  assert(symbol >= 0 && symbol < DFS_SYMBOL_COUNT);
  return bucket_starts[size_t(ranks_by_symbol[size_t(symbol)])];
}

size_t DfsClassList::candidate_end(int symbol) const {
  assert(symbol >= 0 && symbol < DFS_SYMBOL_COUNT);
  return bucket_starts[size_t(ranks_by_symbol[size_t(symbol)] + 1)];
}

DfsMemberSpan DfsClassList::retain_members(
    std::vector<bool> const& keep_class, bool words_only) {
  assert(keep_class.size() == class_count);
  assert(!grouping_dropped);
  DfsPackedMember* const members = members_arena.get();

  // Records are sorted by rarest rank, so walking them is not walking the arena
  // in address order, and compacting in record order would overwrite ranges not
  // yet read. Mark first, then compact forward.
  std::vector<bool> keep_member(entries, false);
  DfsClassRecord const* const records = class_records.get();
  for (size_t ci = 0; ci < class_count; ++ci) {
    if (!keep_class[ci]) continue;
    DfsClassRecord const& record = records[ci];
    size_t const start = size_t(record.members - members);
    for (size_t mi = 0; mi < record.member_count; ++mi)
      if (!words_only || record.members[mi].word_count == 1)
        keep_member[start + mi] = true;
  }

  size_t write = 0;
  for (size_t i = 0; i < entries; ++i)
    if (keep_member[i]) members[write++] = members[i];

  invalidate_members();
  DfsMemberSpan const span = { members, write };
  return span;
}

void DfsClassList::invalidate_members() {
  DfsClassRecord* const records = class_records.get();
  for (size_t ci = 0; ci < class_count; ++ci) {
    records[ci].members = NULL;
    records[ci].member_count = 0;
  }
  grouping_dropped = true;
}

void DfsClassList::release_members() {
  invalidate_members();
  members_arena.reset();
  text_chunks.clear();
}
