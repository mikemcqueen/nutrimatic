#include "pfilter-impl.h"

#include <stdio.h>

#include <algorithm>
#include <string_view>

#include "log.h"
#include "workflow-paths.h"

namespace {

bool in_index(IndexReader const& reader, DfsPairRow const& row) {
  IndexReader::EntryPosition position;
  return reader.aggregate_entry_position(row.entry(), &position) ||
      reader.aggregate_entry_position(row.entry(/*reverse=*/true), &position);
}

}  // namespace

bool Pfilter::load(std::string_view program, PfilterOptions const& options,
                   ClassifiedPairCache& cache) {
  options_ = options;
  rejected_.clear();
  index_.reset();

  auto const add = [&](int sentence, std::string_view kind) {
    char const* const root = require_workflow_root(program);
    if (root == NULL) return false;
    std::shared_ptr<DfsPairSet const> pairs =
        cache.get(program, root, sentence, kind);
    if (pairs == nullptr) return false;
    rejected_.push_back(std::move(pairs));
    return true;
  };
  if (options_.drop_no && !add(CLASSIFIED_NO_SENTENCE, "no")) return false;
  if (options_.drop_no && options_.sentence != CLASSIFIED_NO_SENTENCE &&
      !add(options_.sentence, "no"))
    return false;
  if (options_.drop_yes && !add(CLASSIFIED_NO_SENTENCE, "yes")) return false;
  if (options_.drop_yes && options_.sentence != CLASSIFIED_NO_SENTENCE &&
      !add(options_.sentence, "yes"))
    return false;

  if (!options_.index_file.empty()) {
    FILE* fp = fopen(options_.index_file.c_str(), "rb");
    if (fp == NULL) {
      report(program, "can't open \"%s\"", options_.index_file.c_str());
      return false;
    }
    index_.reset(new IndexReader(fp));
  }
  return true;
}

bool Pfilter::keep(DfsPairRow const& row) const {
  size_t const letters = row.left.size() + row.right.size();
  return letters >= size_t(options_.min_letters) &&
      (options_.max_letters == 0 ||
          letters <= size_t(options_.max_letters)) &&
      (!options_.dictionary ||
          (options_.dictionary->get().contains(row.left) &&
              options_.dictionary->get().contains(row.right))) &&
      std::ranges::none_of(rejected_,
          [entry = row.entry()](auto const& pairs) {
            return pairs->contains(entry);
          }) &&
      (!options_.bag ||
          ((!options_.exact || letters == options_.bag->size) &&
              fits_letter_bag(*options_.bag, row.left + row.right))) &&
      (index_ == nullptr || in_index(*index_, row));
}
