#ifndef NUTRIMATIC_P1_INDEX_H
#define NUTRIMATIC_P1_INDEX_H

#include "mapped-file.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

// The file format version make-p1-index writes and P1Index reads.
inline constexpr uint32_t P1_INDEX_VERSION = 1;

// A pair's phase 1 YES probabilities, one per word order.
struct P1Yes {
    // "a b" and "b a", for the words as asked.
    double ab = 0;
    double ba = 0;
};

// Builds the index of every *.jsonl file in `dir` (phase 1 classifier
// verdicts, one `{"pair": "a,b", ... "fwd": [...], "rvs": [...]}` line per
// pair) at `out`, reading files on up to `threads` threads. A pair on more
// than one line keeps each order's largest YES; a line without one counts
// as 0. False, with `*error` set, on a read or write failure.
bool build_p1_index(std::filesystem::path const& dir,
                    std::filesystem::path const& out, size_t threads,
                    std::string* error);

// A make-p1-index file, mapped.
class P1Index {
  public:
    // Opens `path`. False, with `*error` set, when it isn't a version
    // P1_INDEX_VERSION index or a source file it names is missing or has
    // changed since.
    bool open(std::filesystem::path const& path, std::string* error);

    // The pair's YES probabilities; false when it has no verdict.
    bool yes(std::string_view a, std::string_view b, P1Yes* out) const;

  private:
    // The word's id, or -1.
    int64_t word(std::string_view text) const;

    MappedFile file_;
    NpyArray<uint32_t> word_offsets_;
    char const* word_text_ = nullptr;
    size_t words_ = 0;
    NpyArray<uint64_t> keys_;
    uint16_t const* values_ = nullptr;
};

#endif
