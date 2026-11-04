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
  yes_.clear();
  index_.reset();

  auto const add = [&](std::vector<std::shared_ptr<DfsPairSet const>>& to,
                       int sentence, std::string_view kind, bool missing_ok) {
    std::filesystem::path const root = require_workflow_root(program);
    if (root.empty()) return false;
    std::shared_ptr<DfsPairSet const> pairs =
        cache.get(program, root, sentence, kind, missing_ok);
    if (pairs == nullptr) return false;
    to.push_back(std::move(pairs));
    return true;
  };
  bool const sentence = options_.sentence != CLASSIFIED_NO_SENTENCE;
  if (options_.drop_no &&
      !add(rejected_, CLASSIFIED_NO_SENTENCE, "no", false))
    return false;
  if (options_.drop_no && sentence &&
      !add(rejected_, options_.sentence, "no", false))
    return false;
  if (options_.drop_yes &&
      !add(rejected_, CLASSIFIED_NO_SENTENCE, "yes", false))
    return false;
  if (options_.drop_yes && sentence &&
      !add(rejected_, options_.sentence, "yes", false))
    return false;
  bool const keep_yes = options_.keep_yes && options_.old_bag;
  if (keep_yes && !add(yes_, CLASSIFIED_NO_SENTENCE, "yes", false))
    return false;
  if (keep_yes && sentence && !add(yes_, options_.sentence, "yes", true))
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
      (!options_.required_words ||
          options_.required_words->get().contains(row.left) ||
          options_.required_words->get().contains(row.right)) &&
      (index_ == nullptr || in_index(*index_, row));
}

bool Pfilter::is_new(DfsPairRow const& row) const {
  return !options_.old_bag ||
      !fits_letter_bag(*options_.old_bag, row.left + row.right) ||
      std::ranges::any_of(yes_, [entry = row.entry()](auto const& pairs) {
        return pairs->contains(entry);
      });
}
