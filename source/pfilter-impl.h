#ifndef NUTRIMATIC_PFILTER_IMPL_H
#define NUTRIMATIC_PFILTER_IMPL_H

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "classified.h"
#include "dfs-class-list.h"
#include "dfs-cli-args.h"
#include "index.h"
#include "letter-bag.h"

using DictionaryRef = std::reference_wrapper<DfsDictionary const>;

inline constexpr int PFILTER_DEFAULT_MAX_LETTERS = 20;

// Which pairs pfilter keeps: those of at most max_letters letters (0 for no
// limit), whose words are both in dictionary when given, that fit within bag
// when given, and that are in index_file when not empty. Pairs classified NO
// globally, or for sentence when given, are dropped, as are pairs classified
// YES the same way when drop_yes.
struct PfilterOptions {
  std::optional<LetterBag> bag;
  int max_letters = PFILTER_DEFAULT_MAX_LETTERS;
  int sentence = CLASSIFIED_NO_SENTENCE;
  bool drop_yes = false;
  std::optional<DictionaryRef> dictionary;
  std::string index_file;
};

// PfilterOptions with its classified pairs and index loaded.
class Pfilter {
 public:
  // Loads what `options` names from the workflow root in $WFROOT, taking its
  // classified pairs from `cache`. Errors are diagnosed, prefixed by
  // `program`.
  bool load(char const* program, PfilterOptions const& options,
            ClassifiedPairCache& cache);

  // Whether `row` is kept. Only valid after load() succeeds.
  bool keep(DfsPairRow const& row) const;

 private:
  PfilterOptions options_;
  std::vector<std::shared_ptr<DfsPairSet const>> rejected_;
  std::unique_ptr<IndexReader> index_;
};

#endif
