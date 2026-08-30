#ifndef NUTRIMATIC_PAIR_EXCLUSIONS_H
#define NUTRIMATIC_PAIR_EXCLUSIONS_H

#include <string>
#include <unordered_set>
#include <vector>

#include "dfs-class-list.h"

struct PairExclusionOptions {
  std::vector<std::string> paths;
  bool workflow = false;
};

enum PairExclusionOptionResult {
  PAIR_EXCLUSION_OPTION_OTHER,
  PAIR_EXCLUSION_OPTION_HANDLED,
  PAIR_EXCLUSION_OPTION_ERROR,
};

// Parses -x FILE, --exclude FILE, and --wf. `index` points to the current
// argument and advances over a consumed FILE. Errors are diagnosed already.
PairExclusionOptionResult parse_pair_exclusion_option(
    int argc, char* const argv[], int* index, char const* program,
    PairExclusionOptions* out);

// Loads explicit exclusions and, when requested, the classified pair files
// selected by `workflow_selectors` below $WFROOT. Missing workflow files warn
// and are skipped; unknown selectors, explicit missing files, and all malformed
// or unreadable files are errors.
bool load_pair_exclusions(
    PairExclusionOptions const& options,
    std::unordered_set<std::string> const& workflow_selectors,
    char const* program,
    DfsPairSet* excluded);

#endif
