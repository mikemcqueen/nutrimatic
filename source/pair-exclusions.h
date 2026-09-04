#ifndef NUTRIMATIC_PAIR_EXCLUSIONS_H
#define NUTRIMATIC_PAIR_EXCLUSIONS_H

#include <string>
#include <unordered_set>
#include <vector>

#include "dfs-class-list.h"

// Workflow-root-relative paths --wf and --wfroot resolve against. Shared so
// the loader and every tool's help text name the same files.
extern char const* const WORKFLOW_NO_PAIRS_PATH;
extern char const* const WORKFLOW_YES_PAIRS_PATH;
extern char const* const WORKFLOW_DICT_PATH;

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
// pairs below the selected workflow root are rejected, the root's
// .wf/best/dict/words.big is loaded into `dictionary`, and with -y/--yes,
// classified YES pairs are ignored. Missing workflow files warn and are
// skipped, leaving `dictionary` empty. Without a workflow root there is no
// dictionary to load at all, which warns. Explicit missing files and all
// malformed or unreadable files are errors.
bool load_pair_filters(
    PairFilterOptions const& options, char const* program,
    DfsPairSet* ignored, DfsPairSet* rejected, DfsDictionary* dictionary);

inline bool is_rejected_segment(
    DfsPairSet const& rejected, std::string const& segment) {
  return rejected.find(segment) != rejected.end();
}

// Returns true when `dictionary` is empty, or when every space-delimited word
// of `segment` is in it. An empty dictionary is the no-workflow case, where
// there is nothing to check against.
bool all_words_in_dict(
    DfsDictionary const& dictionary, std::string const& segment);

#endif
