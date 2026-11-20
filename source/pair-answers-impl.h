#ifndef NUTRIMATIC_PAIR_ANSWERS_IMPL_H
#define NUTRIMATIC_PAIR_ANSWERS_IMPL_H

#include "clue-index.h"
#include "dfs-class-list.h"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The dictionary pair-answers marks answers by: its words, and each spelled
// forwards and backwards, sorted.
struct AnswerDictionary {
    DfsDictionary words;
    std::vector<std::string> forward;
    std::vector<std::string> reversed;
};

// Loads `*out` from the word list at `path`. False, diagnosed and prefixed by
// `program`, when it can't be read or has no words.
bool load_answer_dictionary(std::string_view program,
                            std::filesystem::path const& path,
                            AnswerDictionary* out);

// pair-answers' FLAGS for `answer`: M when it is a `dict` word, P when it is
// a prefix of a `dict` word whose rest is a `dict` word, S when it is such a
// suffix, or X for none of them.
std::string answer_flags(AnswerDictionary const& dict,
                         std::string_view answer);

// The crossword answers of pairs' clues, as pair-answers finds them.
class PairAnswers {
  public:
    // Opens the clue index beside `cluedata`. False, diagnosed and prefixed
    // by `program`, when it can't be.
    bool open(std::string_view program,
              std::filesystem::path const& cluedata);

    // Sets `*out` to the distinct answers, upper-cased and sorted, of the
    // clues where `left` and `right` match as `match` says.
    void answers(std::string_view left, std::string_view right,
                 ClueMatch match, std::vector<std::string_view>* out) const;

    // Whether answers() of `pairs[i]` isn't empty, for each i, found on
    // `threads` threads.
    std::vector<char> have_answers(
        std::span<std::pair<std::string_view, std::string_view> const> pairs,
        ClueMatch match, size_t threads) const;

  private:
    ClueIndex index_;
};

#endif
