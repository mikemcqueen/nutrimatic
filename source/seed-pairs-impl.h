#ifndef NUTRIMATIC_SEED_PAIRS_IMPL_H
#define NUTRIMATIC_SEED_PAIRS_IMPL_H

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "p1-index.h"
#include "seed-view.h"
#include "workflow-paths.h"

// Where seed-pairs reads its word,word lines: a pair file, mapped, or lines
// already in memory, such as a pgui Column's items.
using SeedPairsInput =
    std::variant<std::filesystem::path, std::span<std::string const>>;

// How seed-pairs reads and filters pairs: looked up in the p1 index at p1,
// with the forced pairs of the pair file at forced, none when empty, kept
// when they pass filter, whose dictionary, when given, is named
// dictionary_path in warnings. Reads on threads threads, 0 for all of them.
struct SeedPairsOptions {
  std::filesystem::path p1 = P1_INDEX_PATH;
  SeedFilter filter = {.min_yes = .85f};
  std::filesystem::path dictionary_path;
  std::filesystem::path forced;
  int threads = 0;
};

// SeedPairsOptions with its p1 index opened.
class SeedPairs {
 public:
  // Opens what `options` names. Errors are diagnosed, prefixed by `program`,
  // as is a stale p1 index, which opens anyway.
  bool load(std::string_view program, SeedPairsOptions const& options);

  // Sets `*out` to the pairs of `input`, in its order, then the forced pairs
  // it lacks, in theirs, that pass the filter, each oriented to its larger
  // YES. Errors are diagnosed. Only valid after load() succeeds; may run on
  // several threads at once.
  bool run(SeedPairsInput const& input, std::vector<SeedPair>* out) const;

 private:
  std::string program_;
  SeedPairsOptions options_;
  P1Index p1_;
};

#endif
