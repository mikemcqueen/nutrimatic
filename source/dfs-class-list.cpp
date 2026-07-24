#include "dfs-class-list.h"
#include "index.h"

#include <assert.h>

#include <algorithm>
#include <unordered_map>

int dfs_symbol_index(unsigned char ch) {
  if (ch >= 'a' && ch <= 'z') return ch - 'a';
  if (ch >= '0' && ch <= '9') return 26 + ch - '0';
  return -1;
}

static std::string make_class_key(std::string const& text) {
  std::string key;
  key.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i)
    if (text[i] != ' ') key.push_back(text[i]);
  std::sort(key.begin(), key.end());
  return key;
}

// Production form of measure-f's Extractor. It follows only trie edges allowed
// by the remaining bag and emits at every terminating space, continuing beyond
// it when enough letters remain for another word.
class DfsExtractor {
 public:
  DfsExtractor(IndexReader const* reader, std::string const& letters,
               int min_word_len, bool include_phrases):
      reader(reader),
      min_len(std::max(min_word_len, 1)),
      max_words(include_phrases ? int(letters.size()) / min_len : 1),
      letters_left(int(letters.size())),
      nodes(0) {
    bag.fill(0);
    for (size_t i = 0; i < letters.size(); ++i) {
      unsigned char const ch = (unsigned char) letters[i];
      assert(dfs_symbol_index(ch) >= 0);
      ++bag[ch];
    }
    choices.resize(letters.size() + size_t(max_words) + 2);
  }

  void run() {
    text.clear();
    walk(reader->root(), reader->count(), 0, 0, 0);
  }

  int64_t nodes_visited() const { return nodes; }

  std::unordered_map<std::string, std::vector<DfsClassMember> > grouped;

 private:
  void emit(int64_t count, int word_count) {
    DfsClassMember member;
    member.text.assign(text, 0, text.size() - 1);
    member.count = count;
    member.word_count = word_count;
    grouped[make_class_key(member.text)].push_back(member);
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
      text.push_back(choice.ch);
      walk(choice.next, choice.count, word_len + 1, words, depth + 1);
      text.pop_back();
      ++letters_left;
      ++bag[ch];
    }
  }

  IndexReader const* const reader;
  int const min_len;
  int const max_words;
  std::array<int, 256> bag;
  int letters_left;
  std::string text;
  std::vector<std::vector<IndexReader::Choice> > choices;
  int64_t nodes;
};

static bool member_order(DfsClassMember const& a, DfsClassMember const& b) {
  if (a.count != b.count) return a.count > b.count;
  if (a.text != b.text) return a.text < b.text;
  return a.word_count < b.word_count;
}

static bool same_member(DfsClassMember const& a, DfsClassMember const& b) {
  return a.count == b.count && a.text == b.text &&
      a.word_count == b.word_count;
}

DfsClassList::DfsClassList(IndexReader const* reader,
                           std::string const& letters,
                           int min_word_len, bool include_phrases):
    minimum_word_len(std::max(min_word_len, 1)), entries(0), nodes(0) {
  frequencies.fill(0);

  DfsExtractor extractor(
      reader, letters, minimum_word_len, include_phrases);
  extractor.run();
  nodes = extractor.nodes_visited();

  class_list.reserve(extractor.grouped.size());
  for (std::unordered_map<std::string, std::vector<DfsClassMember> >::iterator
       it = extractor.grouped.begin(); it != extractor.grouped.end(); ++it) {
    DfsAnagramClass anagram_class;
    anagram_class.key = it->first;
    anagram_class.members.swap(it->second);
    std::sort(anagram_class.members.begin(), anagram_class.members.end(),
              member_order);
    anagram_class.members.erase(
        std::unique(anagram_class.members.begin(),
                    anagram_class.members.end(), same_member),
        anagram_class.members.end());
    entries += anagram_class.members.size();

    for (size_t mi = 0; mi < anagram_class.members.size(); ++mi) {
      std::string const& text = anagram_class.members[mi].text;
      for (size_t i = 0; i < text.size(); ++i) {
        int const symbol = dfs_symbol_index((unsigned char) text[i]);
        if (symbol >= 0) ++frequencies[size_t(symbol)];
      }
    }

    int counts[DFS_SYMBOL_COUNT] = { 0 };
    for (size_t i = 0; i < anagram_class.key.size(); ++i) {
      int const symbol =
          dfs_symbol_index((unsigned char) anagram_class.key[i]);
      assert(symbol >= 0);
      ++counts[symbol];
    }
    for (int symbol = 0; symbol < DFS_SYMBOL_COUNT; ++symbol)
      if (counts[symbol] > 0)
        anagram_class.letters.push_back(
            std::make_pair(uint8_t(symbol), uint32_t(counts[symbol])));
    class_list.push_back(std::move(anagram_class));
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

  for (size_t ci = 0; ci < class_list.size(); ++ci) {
    DfsAnagramClass& anagram_class = class_list[ci];
    int rarest = DFS_SYMBOL_COUNT;
    for (size_t i = 0; i < anagram_class.letters.size(); ++i) {
      int const symbol = anagram_class.letters[i].first;
      rarest = std::min(rarest, ranks_by_symbol[size_t(symbol)]);
    }
    anagram_class.rarest_rank = rarest;
  }

  std::sort(class_list.begin(), class_list.end(),
            [](DfsAnagramClass const& a, DfsAnagramClass const& b) {
    if (a.rarest_rank != b.rarest_rank)
      return a.rarest_rank < b.rarest_rank;
    if (a.key.size() != b.key.size()) return a.key.size() > b.key.size();
    return a.key < b.key;
  });

  size_t pos = 0;
  for (int rank = 0; rank < DFS_SYMBOL_COUNT; ++rank) {
    bucket_starts[size_t(rank)] = pos;
    while (pos < class_list.size() &&
           class_list[pos].rarest_rank == rank)
      ++pos;
  }
  bucket_starts[DFS_SYMBOL_COUNT] = class_list.size();
}

size_t DfsClassList::candidate_begin(int symbol) const {
  assert(symbol >= 0 && symbol < DFS_SYMBOL_COUNT);
  return bucket_starts[size_t(ranks_by_symbol[size_t(symbol)])];
}

size_t DfsClassList::candidate_end(int symbol) const {
  assert(symbol >= 0 && symbol < DFS_SYMBOL_COUNT);
  return bucket_starts[size_t(ranks_by_symbol[size_t(symbol)] + 1)];
}
