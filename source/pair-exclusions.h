#ifndef NUTRIMATIC_PAIR_EXCLUSIONS_H
#define NUTRIMATIC_PAIR_EXCLUSIONS_H

#include <stdio.h>

#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "dfs-class-list.h"
#include "workflow-paths.h"

struct PairFilterOptions {
  std::vector<std::string> allow_paths;
  std::vector<std::string> ignore_paths;
  std::vector<std::string> reject_paths;
  std::string dictionary_path;
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

// Which pair-filter options a tool accepts. Options a tool does not support
// are refused at parse time, so the loader never sees them.
struct PairFilterSupport {
  bool ignore = false;        // -i/--ignore
  bool allow = false;         // -a/--allow-pairs
  bool workflow_yes = false;  // -y/--yes
};

// The workflow-owned portions of the aggregate ignore and reject sets.
// Callers that need to explain which layer acted may consult these; ordinary
// filtering can continue to use the aggregate sets alone.
struct PairFilterSources {
  DfsPairSet classified_yes;
  DfsPairSet classified_no;
  DfsPairSet target_no;
  std::string target;
};

// Rejection layers in attribution precedence order. PAIR_FILTER_NONE is not
// part of that order; it means every layer accepted the segment.
enum PairFilterLayer {
  PAIR_FILTER_NONE,
  PAIR_FILTER_CLASSIFIED_NO,
  PAIR_FILTER_TARGET_NO,
  PAIR_FILTER_EXPLICIT_REJECT,
  PAIR_FILTER_ALLOWLIST,
  PAIR_FILTER_DICTIONARY,
};

// Every filtering layer a segment tool applies. Empty members accept
// everything, so a tool consults only the layers it cares about.
struct PairFilters {
  DfsPairSet ignored;
  DfsPairSet rejected;
  DfsDictionary dictionary;
  std::optional<DfsPairSet> allowed;  // nullopt: no allowlist policy active
  PairFilterSources sources;          // empty without a workflow root

  // Returns the first layer that rejects `segment`, in PairFilterLayer order.
  PairFilterLayer first_rejecting_layer(std::string const& segment) const;
};

enum PairFilterOptionResult {
  PAIR_FILTER_OPTION_OTHER,
  PAIR_FILTER_OPTION_HANDLED,
  PAIR_FILTER_OPTION_ERROR,
};

// Prints the shared -r/--reject detailed-help block. `description_column` is
// the zero-based column at which option descriptions begin in the caller's
// help text.
void print_reject_option_help(FILE* fp, int description_column);

// Prints the shared -a/--allow-pairs detailed-help block.
void print_allow_pairs_option_help(FILE* fp, int description_column);

// Parses -a/--allow-pairs FILE, -i/--ignore FILE, -r/--reject FILE,
// -d/--dict PATH, --wf, --wfroot DIR, -t/--target TARGET, and -y/--yes.
// Allow, ignore, and -y/--yes options are returned as OTHER when `support`
// does not list them. `index` points to the current argument and advances
// over a consumed FILE, DIR or TARGET. Errors are diagnosed already.
PairFilterOptionResult parse_pair_filter_option(
    int argc, char* const argv[], int* index, char const* program,
    PairFilterSupport support, PairFilterOptions* out);

// Diagnoses the options that need a workflow root without one: -y/--yes and
// -t/--target. Tools call this once their arguments are parsed, before
// load_pair_filters().
bool check_pair_filter_options(
    PairFilterOptions const& options, char const* program);

// Loads explicit allow/ignore/reject files. Allow files load pairs while
// ignoring and counting standalone entries; ignore files are pair-only;
// explicit reject files may also contain standalone words. When no allow file
// was supplied, `out->allowed` is nullopt; supplying one or more files
// containing no pairs leaves it as an active empty set. `support` states which
// options the calling tool accepts, and has to agree with `options`, which the
// tool's own parsing guarantees. With --wf or --wfroot, classified NO
// pairs below the selected workflow root are rejected, and with -y/--yes,
// classified YES pairs are ignored. -d/--dict selects the dictionary whether
// or not there is a workflow root; otherwise workflow mode defaults to the
// root's .wf/dict/words.filtered. A missing inferred workflow dictionary warns
// and leaves `dictionary` empty. Without either a workflow root or an explicit
// dictionary there is no dictionary to load, which warns. Explicit missing
// files and all malformed or unreadable files are errors.
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
    PairFilterSupport support, PairFilters* out);

// Returns true when `segment` is an exact rejected pair or contains a
// space-delimited word listed on its own in an explicit reject file.
bool is_rejected_segment(
    DfsPairSet const& rejected, std::string const& segment);

// Returns true for every solo-word segment. A multi-word segment is allowed
// only when no allowlist policy is active or its complete text is in the
// bidirectional pair set. In particular, active policies reject segments of
// three or more words because pair files cannot represent them.
bool is_allowed_segment(
    std::optional<DfsPairSet> const& allowed, std::string const& segment);

// Returns true when `dictionary` is empty, or when every space-delimited word
// of `segment` is in it. An empty dictionary means there is nothing to check
// against.
bool all_words_in_dict(
    DfsDictionary const& dictionary, std::string const& segment);

#endif
