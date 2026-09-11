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

// Where a target's own exclusions sit, as help text and warnings spell it.
// The capitalised components stand for one target's sentence, letter set,
// minimum word length and segment count: this is a shape to match, not a
// path to open.
extern char const* const WORKFLOW_TARGET_NO_PAIRS_PATH;

// The target selected when -t/--target is not given and the input names no
// target of its own.
extern char const* const WORKFLOW_DEFAULT_TARGET;

// How the workflow renders a dfs-anagrams results file kept outside the
// tree. The bracketed parts are the ones that may be absent. Like the path
// above this is a shape, shown when a name could not be read as one.
extern char const* const WORKFLOW_RESULTS_NAME;

struct PairFilterOptions {
  std::vector<std::string> ignore_paths;
  std::vector<std::string> reject_paths;
  bool workflow = false;
  std::string workflow_root;
  bool workflow_yes = false;
  // The -t/--target value, or empty to take the target from `input_path`.
  // Named rather than resolved here: what it addresses is known only once a
  // workflow root is selected.
  std::string target;
  // The one input file the tool is about to read, or empty when it will read
  // standard input or more than one file. Set by the tool once its arguments
  // are parsed, because only a single named file identifies a single target.
  std::string input_path;
};

// The workflow-owned portions of the aggregate ignore and reject sets.
// Callers that need to explain which layer acted may request these from
// load_pair_filters(); ordinary filtering can continue to use the aggregate
// sets alone.
struct PairFilterSources {
  DfsPairSet classified_yes;
  DfsPairSet classified_no;
  DfsPairSet target_no;
  std::string target;
};

enum PairFilterOptionResult {
  PAIR_FILTER_OPTION_OTHER,
  PAIR_FILTER_OPTION_HANDLED,
  PAIR_FILTER_OPTION_ERROR,
};

// Parses -i FILE, --ignore FILE, -r FILE, --reject FILE, --wf,
// --wfroot DIR, -t/--target TARGET, and -y/--yes. Ignore and -y/--yes
// options are returned as OTHER when their support flags are false. `index`
// points to the current argument and advances over a consumed FILE, DIR or
// TARGET. Errors are diagnosed already.
PairFilterOptionResult parse_pair_filter_option(
    int argc, char* const argv[], int* index, char const* program,
    bool support_ignore, bool support_workflow_yes, PairFilterOptions* out);

// Diagnoses the options that need a workflow root without one: -y/--yes and
// -t/--target. Tools call this once their arguments are parsed, before
// load_pair_filters().
bool check_pair_filter_options(
    PairFilterOptions const& options, char const* program);

// Loads explicit ignore/reject files. With --wf or --wfroot, classified NO
// pairs below the selected workflow root are rejected, the root's
// .wf/dict/words.filtered is loaded into `dictionary`, and with -y/--yes,
// classified YES pairs are ignored. Missing workflow files warn and are
// skipped, leaving `dictionary` empty. Without a workflow root there is no
// dictionary to load at all, which warns. Explicit missing files and all
// malformed or unreadable files are errors.
//
// A workflow root also selects a target, .wf/best/ plus a target name, and
// rejects that target's own no.pairs, so that asking for the workflow's
// filtering gets all of it rather than the root-level half. The name is the
// -t/--target value; without one it is the target `options.input_path`
// belongs to, by its directory or by its name, and WORKFLOW_DEFAULT_TARGET
// only when the input names none. However it was arrived at, the selection
// is announced and has to resolve to a target directory, and a named input
// that disagrees with an explicit -t/--target is an error. A target with no
// no.pairs is silent, because having none is the ordinary case.
bool load_pair_filters(
    PairFilterOptions const& options, char const* program,
    DfsPairSet* ignored, DfsPairSet* rejected, DfsDictionary* dictionary,
    PairFilterSources* sources = NULL);

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
