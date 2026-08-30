#ifndef NUTRIMATIC_PAIR_EXCLUSIONS_H
#define NUTRIMATIC_PAIR_EXCLUSIONS_H

#include <string>
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

// Loads explicit exclusions and, when requested, the classified YES and NO
// pair files below $WFROOT. Missing workflow files warn and are skipped;
// explicit missing files and all malformed or unreadable files are errors.
bool load_pair_exclusions(
    PairExclusionOptions const& options, char const* program,
    DfsPairSet* excluded);

#endif
