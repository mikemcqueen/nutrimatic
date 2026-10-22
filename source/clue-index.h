#ifndef NUTRIMATIC_CLUE_INDEX_H
#define NUTRIMATIC_CLUE_INDEX_H

#include "mapped-file.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// The index format version ClueIndex reads.
inline constexpr int CLUE_INDEX_VERSION = 3;

// What crossword clues say about a pair of words.
struct CluePairCounts {
    // Clues containing both words.
    uint32_t together = 0;
    // Of those, the clues where the two are adjacent, in either order, one
    // space or hyphen apart.
    uint32_t adjacent = 0;
    // Clues of answer A containing word B, plus clues of answer B containing
    // word A.
    uint32_t answer_links = 0;
    // Of `together`, the clues that are the two words and nothing else.
    uint32_t exact = 0;
    // Of `adjacent`, the clues of at most three words.
    uint32_t adjacent_short = 0;
    // The fewest words in a clue of `together`, 0 without one.
    uint32_t min_words = 0;
    // Clues whose answer is the two words run together, in either order.
    uint32_t phrase_answer = 0;
    // Of `answer_links`, the clues that are the other word and nothing else.
    uint32_t answer_exact = 0;
    // The fewest words in a clue of `answer_links`, 0 without one.
    uint32_t answer_min_words = 0;
    // Clues containing each word.
    uint32_t clues_a = 0;
    uint32_t clues_b = 0;
};

// cluer's whole-word clue index (../words/cluer/index.py), as build_index.py
// writes it to the `index` directory beside its cluedata. Words are compared
// as the index sees them: lower case, apostrophes deleted.
class ClueIndex {
  public:
    // Opens the index beside `cluedata`. False, with `*error` set, when it
    // can't be read, isn't CLUE_INDEX_VERSION, or was built from a different
    // cluedata.
    bool open(std::filesystem::path const& cluedata, std::string* error);

    CluePairCounts pair(std::string_view a, std::string_view b) const;

  private:
    // The sorted clue ids containing `word`, empty when no clue has it.
    NpyArray<uint32_t> postings(std::string_view word) const;
    // The sorted clue ids whose answer is `word`, upper-cased.
    NpyArray<uint32_t> answer_clues(std::string_view word) const;
    std::string_view clue(uint32_t id) const;

    MappedFile data_;
    MappedFile postings_file_;
    MappedFile postings_start_file_;
    MappedFile clue_off_file_;
    NpyArray<uint32_t> postings_;
    NpyArray<uint32_t> postings_start_;
    NpyArray<uint32_t> clue_off_;
    std::unordered_map<std::string, uint32_t> words_;
    // Each distinct answer's clues: answer_start_[i] to answer_start_[i + 1]
    // in answer_clues_.
    std::unordered_map<std::string, uint32_t> answers_;
    std::vector<uint32_t> answer_start_;
    std::vector<uint32_t> answer_clues_;
};

#endif
