#ifndef NUTRIMATIC_PAIR_EXCLUSIONS_H
#define NUTRIMATIC_PAIR_EXCLUSIONS_H

#include <string>
#include <unordered_set>
#include <vector>

#include "dfs-class-list.h"

struct PairFilterOptions {
  std::vector<std::string> ignore_paths;
  std::vector<std::string> reject_paths;
  bool workflow = false;
  std::string workflow_root;
  bool workflow_yes = false;
};

enum PairFilterOptionResult {
  PAIR_FILTER_OPTION_OTHER,
  PAIR_FILTER_OPTION_HANDLED,
  PAIR_FILTER_OPTION_ERROR,
};

// Parses -i FILE, --ignore FILE, -r FILE, --reject FILE, --wf,
// --wfroot DIR, and -y/--yes. Ignore and -y/--yes options are returned as
// OTHER when their support flags are false. `index` points to the current
// argument and advances over a consumed FILE or DIR. Errors are diagnosed
// already.
PairFilterOptionResult parse_pair_filter_option(
    int argc, char* const argv[], int* index, char const* program,
    bool support_ignore, bool support_workflow_yes, PairFilterOptions* out);

// Loads explicit ignore/reject files. With --wf or --wfroot, classified NO
// pairs below the selected workflow root are rejected; with -y/--yes,
// classified YES pairs are ignored. Missing workflow files warn and are
// skipped. Explicit missing files and all malformed or unreadable files are
// errors.
bool load_pair_filters(
    PairFilterOptions const& options, char const* program,
    DfsPairSet* ignored, DfsPairSet* rejected);

#endif
