#ifndef NUTRIMATIC_SEED_VIEW_H
#define NUTRIMATIC_SEED_VIEW_H

#include "dfs-class-list.h"
#include "p1-index.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// One pair of a pair file with its phase 1 YES, oriented to the larger
// order.
struct SeedPair {
    std::string line;  // "a,b", in the order with the larger YES
    float yes;         // max(YES ab, YES ba); -1 when p1 has no verdict
    uint16_t comma;    // offset of the comma in `line`
    bool forced = false;  // listed in a forced pair file; see
                          // add_forced_pairs()
};

// Reads the word,word lines of `pairs`, a regular file, looking each up in
// `p1`, which may be null (every yes is then -1). Lines in their file
// order, read on `threads` threads. False, with `*error` set, when the file
// can't be read or a line isn't two comma-separated words.
bool read_seed_pairs(std::filesystem::path const& pairs,
                     P1Index const* p1, size_t threads,
                     std::vector<SeedPair>* out, std::string* error);

// Marks each pair of `*pairs` that `forced`, a pair file, lists, in either
// order, as forced, then appends, as forced, the pairs it lists that
// `*pairs` lacks, once each, in its file order, looking them up in `p1` as
// read_seed_pairs() does. True, doing nothing, when `forced` doesn't exist;
// false, with `*error` set, when it can't be read or a line isn't two
// comma-separated words.
bool add_forced_pairs(std::filesystem::path const& forced,
                      P1Index const* p1, std::vector<SeedPair>* pairs,
                      std::string* error);

// Whether `sentence` is s followed by a digit from 1 to 9.
bool valid_seed_sentence(std::string_view sentence);

// The forced pair file of `sentence`, sN, in `directory`: sN.pairs; none
// when `sentence` isn't valid_seed_sentence().
std::filesystem::path forced_pairs_path(std::filesystem::path const& directory,
                                        std::string_view sentence);

// Warns, prefixed by `program`, about each word of the forced pairs of
// `pairs`, from `forced`, that isn't in `dictionary`, read from
// `dictionary_path`, once each, sorted.
void warn_forced_words_missing(std::string_view program,
                               std::vector<SeedPair> const& pairs,
                               std::filesystem::path const& forced,
                               DfsDictionary const& dictionary,
                               std::filesystem::path const& dictionary_path);

struct SeedFilter {
    int min_len = 3;                            // each word, inclusive
    int max_len = 13;                           // each word, inclusive
    std::optional<float> min_yes;               // none keeps every pair
    DfsDictionary const* dictionary = nullptr;  // null keeps every word
};

// Whether `pair` passes `filter`. A forced pair skips `min_yes`; any other
// pair with no verdict passes only when `min_yes` is unset.
bool seed_pair_passes(SeedPair const& pair, SeedFilter const& filter);

#endif
