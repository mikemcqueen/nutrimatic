#include "pfilter-impl.h"

#include <stdio.h>

#include "workflow-paths.h"

namespace {

bool in_index(IndexReader const& reader, DfsPairRow const& row) {
  IndexReader::EntryPosition position;
  return reader.aggregate_entry_position(row.entry(), &position) ||
      reader.aggregate_entry_position(row.entry(/*reverse=*/true), &position);
}

}  // namespace

bool Pfilter::load(char const* program, PfilterOptions const& options) {
  options_ = options;
  rejected_.clear();
  index_.reset();

  char const* const root = require_workflow_root(program);
  if (root == NULL) return false;

  if (!load_global_no_pairs(program, root, &rejected_)) return false;
  if (options_.sentence != CLASSIFIED_NO_SENTENCE &&
      !load_sentence_no_pairs(program, root, options_.sentence, &rejected_))
    return false;
  if (options_.drop_yes &&
      !load_classified_pairs(program, root, CLASSIFIED_NO_SENTENCE, "yes",
          &rejected_))
    return false;
  if (options_.drop_yes && options_.sentence != CLASSIFIED_NO_SENTENCE &&
      !load_classified_pairs(program, root, options_.sentence, "yes",
          &rejected_))
    return false;

  if (!options_.index_file.empty()) {
    FILE* fp = fopen(options_.index_file.c_str(), "rb");
    if (fp == NULL) {
      fprintf(stderr, "%s: can't open \"%s\"\n", program,
          options_.index_file.c_str());
      return false;
    }
    index_.reset(new IndexReader(fp));
  }
  return true;
}

bool Pfilter::keep(DfsPairRow const& row) const {
  return (options_.max_letters == 0 ||
             row.left.size() + row.right.size() <=
                 size_t(options_.max_letters)) &&
      (!options_.dictionary ||
          (options_.dictionary->get().contains(row.left) &&
              options_.dictionary->get().contains(row.right))) &&
      !rejected_.contains(row.entry()) &&
      (!options_.bag || fits_letter_bag(*options_.bag, row.left + row.right)) &&
      (index_ == nullptr || in_index(*index_, row));
}
