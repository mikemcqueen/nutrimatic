#include "pair-exclusions.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <string>
#include <vector>

#include "dfs-cli-args.h"
#include "log.h"
#include "option-value.h"

namespace {

// The two halves of WORKFLOW_TARGET_NO_PAIRS_PATH that are matched rather
// than displayed: everything above a target, and the file inside one.
std::string workflow_path(char const* root, char const* relative) {
  std::string path(root);
  if (!path.empty() && path.back() != '/') path.push_back('/');
  path.append(relative);
  return path;
}

bool workflow_file_missing(
    std::string const& path, char const* program, char const* description) {
  struct stat status;
  if (stat(path.c_str(), &status) == 0 ||
      (errno != ENOENT && errno != ENOTDIR))
    return false;
  warn(program, "%s \"%s\" is not present", description, path.c_str());
  return true;
}

// The directory part of a path, without consulting the filesystem.
std::string lexical_dirname(std::string const& path) {
  size_t const slash = path.find_last_of('/');
  if (slash == std::string::npos) return ".";
  if (slash == 0) return "/";
  return path.substr(0, slash);
}

// The final component of a path, without consulting the filesystem.
std::string lexical_basename(std::string const& path) {
  size_t const slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool resolve(std::string const& path, std::string* out) {
  char* const real = realpath(path.c_str(), NULL);
  if (real == NULL) return false;
  out->assign(real);
  free(real);
  return true;
}

std::vector<std::string> split(std::string const& text, char delimiter) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (true) {
    size_t const end = text.find(delimiter, start);
    parts.push_back(end == std::string::npos
        ? text.substr(start) : text.substr(start, end - start));
    if (end == std::string::npos) return parts;
    start = end + 1;
  }
}

bool is_directory(std::string const& path) {
  struct stat status;
  return stat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode);
}

// Whether `name` is a counted component: one letter and then digits, as mN
// and gN are spelled.
bool is_counted(std::string const& name, char letter) {
  if (name.size() < 2 || name[0] != letter) return false;
  for (size_t i = 1; i < name.size(); ++i)
    if (name[i] < '0' || name[i] > '9') return false;
  return true;
}

// The name of the target `dir` is, when it is one: its path below `best`,
// which is a target's shape when it has four components and the last two
// are counted. Both paths are resolved already, so the name that comes out
// is the canonical one, and the same target reached two ways gets the same
// name.
bool target_name_below_best(
    std::string const& dir, std::string const& best, std::string* out) {
  std::string prefix(best);
  if (prefix.empty() || prefix.back() != '/') prefix.push_back('/');
  if (dir.compare(0, prefix.size(), prefix) != 0) return false;

  std::string const name = dir.substr(prefix.size());
  std::vector<std::string> const parts = split(name, '/');
  if (parts.size() != 4) return false;
  if (!is_counted(parts[2], 'm') || !is_counted(parts[3], 'g')) return false;
  *out = name;
  return true;
}

// The target `input` is an artifact of, if it is one.
//
// A workflow artifact sits directly in a target directory, so the candidate
// is just the file's own directory and the question is whether that
// directory has a target's shape below the selected root. The dirname is
// taken lexically and only then resolved, because an artifact may be a
// symlink to results kept elsewhere -- what names the target is where the
// link sits, not where it points. Both sides are resolved so a relative
// input and a relative root compare, and so a symlinked .wf resolves the
// same way on each.
bool workflow_target_dir(
    std::string const& input, char const* wfroot, std::string* out) {
  std::string dir, best;
  if (!resolve(lexical_dirname(input), &dir)) return false;
  if (!resolve(workflow_path(wfroot, WORKFLOW_BEST_PATH), &best)) return false;
  return target_name_below_best(dir, best, out);
}

// Whether `name` is a letter-set label: the bag form, a dash, its letters.
bool is_letter_set(std::string const& name) {
  return name.size() > 2 && (name[0] == 'o' || name[0] == 'u') &&
      name[1] == '-';
}

bool is_number(std::string const& name) {
  if (name.empty()) return false;
  for (size_t i = 0; i < name.size(); ++i)
    if (name[i] < '0' || name[i] > '9') return false;
  return true;
}

// The target `input` names, for a dfs-anagrams results file kept outside
// the workflow tree.
//
// The workflow renders those names as
// dfs.SENTENCE[.SEED].mN.x2.gN[.best].LIMIT.LETTERS, so every component of
// a target address is in the name -- but the seed annotation between the
// sentence and mN is opaque and may be any number of components, so the
// address is read inward from both ends and never straight through. The
// directory the file is in says nothing here: results are kept wherever the
// search was told to put them, so the target is rebuilt under the selected
// root instead. A name that is there is canonicalised, so that it compares
// equal to the selected target however either side was spelled; one that is
// not is left as written, to be reported as the disagreement it is.
bool workflow_target_name(
    std::string const& input, char const* wfroot, std::string* out) {
  std::vector<std::string> const parts =
      split(lexical_basename(input), '.');
  if (parts.size() < 7 || parts[0] != "dfs") return false;

  size_t last = parts.size() - 1;
  if (!is_letter_set(parts[last])) return false;
  std::string const& letter_set = parts[last];
  if (!is_number(parts[--last])) return false;
  if (parts[last - 1] == "best") --last;
  if (!is_counted(parts[--last], 'g')) return false;
  std::string const& segments = parts[last];
  if (!is_counted(parts[--last], 'x')) return false;
  if (!is_counted(parts[--last], 'm')) return false;
  std::string const& universe = parts[last];
  // The sentence is the one component the name pins from the left, so what
  // is left over between it and mN is the seed annotation, whatever it is.
  if (last < 2) return false;

  std::string name =
      parts[1] + "/" + letter_set + "/" + universe + "/" + segments;
  std::string best, dir, canonical;
  if (resolve(workflow_path(wfroot, WORKFLOW_BEST_PATH), &best) &&
      resolve(best + "/" + name, &dir) && is_directory(dir) &&
      target_name_below_best(dir, best, &canonical))
    name = canonical;
  *out = name;
  return true;
}

// The target ROOT/.wf/best/NAME addresses, as the name of the directory it
// resolves to. A symlink is the ordinary spelling -- "current" is one -- so
// what the link points at is what the run is about, and that is the name
// every later message and comparison uses.
//
// Nothing here is recoverable: the option asks for one particular target,
// so any answer but a target directory is a run that would filter against
// something other than what was asked for.
bool resolve_target_name(
    char const* wfroot, std::string const& name, char const* program,
    std::string* out) {
  std::string best;
  if (!resolve(workflow_path(wfroot, WORKFLOW_BEST_PATH), &best)) {
    fprintf(stderr, "%s: no \"%s\" below \"%s\"\n",
        program, WORKFLOW_BEST_PATH, wfroot);
    return false;
  }

  std::string dir;
  if (!resolve(best + "/" + name, &dir) || !is_directory(dir)) {
    fprintf(stderr, "%s: target \"%s\" is not a directory in \"%s\"\n",
        program, name.c_str(), best.c_str());
    return false;
  }
  if (!target_name_below_best(dir, best, out)) {
    fprintf(stderr, "%s: target \"%s\" leads to \"%s\", which is not "
        "ROOT/%s\n", program, name.c_str(), dir.c_str(),
        WORKFLOW_TARGET_PATH);
    return false;
  }
  return true;
}

// Whether any component of `path` is the workflow directory.
bool under_workflow_dir(std::string const& path) {
  std::vector<std::string> const parts = split(path, '/');
  for (size_t i = 0; i < parts.size(); ++i)
    if (parts[i] == WORKFLOW_DIR_PATH) return true;
  return false;
}

// The target `input` belongs to, by whichever of the two spellings applies.
//
// A .wf component decides: inside the tree the file sits in its target and
// the directory answers, outside it the file was rendered into results and
// only its name does. Choosing rather than trying both means a path in the
// tree that does not resolve is reported as the anomaly it is, instead of
// falling through to a name that was never going to parse.
bool workflow_target(
    std::string const& input, char const* wfroot, std::string* out) {
  return under_workflow_dir(input)
      ? workflow_target_dir(input, wfroot, out)
      : workflow_target_name(input, wfroot, out);
}

// The target to resolve when -t/--target was not given: the one the single
// named input belongs to, when it names one, and otherwise the default
// link. What comes out is still only a name -- a results file kept outside
// the tree can name a target that has no directory -- so it goes through
// resolve_target_name() like any other.
std::string default_target_name(
    PairFilterOptions const& options, char const* wfroot) {
  std::string named;
  if (!options.input_path.empty() &&
      workflow_target(options.input_path, wfroot, &named))
    return named;
  return WORKFLOW_DEFAULT_TARGET;
}

// Whether the file the tool will read agrees with the selected target.
//
// Only a file that names a target of its own has anything to say, and when
// it does the two names have to be the same one: neither is authoritative
// enough to silently win, and a results file filtered against another
// target's classification is what this is here to catch. Files that name no
// target -- a listing, standard input -- say nothing and are not asked to.
bool target_agrees_with_input(
    PairFilterOptions const& options, char const* wfroot,
    std::string const& target, char const* program) {
  std::string named;
  if (options.input_path.empty() ||
      !workflow_target(options.input_path, wfroot, &named) ||
      named == target)
    return true;
  fprintf(stderr,
      "%s: \"%s\" belongs to target \"%s\", not the selected \"%s\"\n",
      program, options.input_path.c_str(), named.c_str(), target.c_str());
  return false;
}

// Rejects the selected target's own no.pairs.
//
// Never fatal on absence: a target with no exclusions is the ordinary case
// and says nothing, leaving `rejected` holding just the root-level set. A
// file that is there but unreadable or malformed is an error like any other
// reject list.
bool load_target_pair_file(
    char const* wfroot, std::string const& target, char const* program,
    DfsPairSet* rejected, DfsPairSet* source) {
  std::string const path = workflow_path(wfroot, WORKFLOW_BEST_PATH) + "/" +
      target + "/" + WORKFLOW_TARGET_NO_PAIRS_NAME;
  struct stat status;
  if (stat(path.c_str(), &status) == 0) {
    if (!load_pair_file(path.c_str(), "reject list", source, true, true))
      return false;
    rejected->insert(source->begin(), source->end());
    return true;
  }
  // A link with nothing under it is a tree to fix: targets keep this file as
  // a link into the results it was written beside, and a stale one would
  // read as the ordinary case and quietly filter nothing.
  if (lstat(path.c_str(), &status) == 0)
    warn(program, "\"%s\" leads nowhere; it was not applied",
        path.c_str());
  return true;
}

bool load_workflow_pair_file(
    std::string const& path, char const* program, char const* description,
    DfsPairSet* pairs, DfsPairSet* source) {
  if (workflow_file_missing(path, program, "classified pair file"))
    return true;
  if (!load_pair_file(path.c_str(), description, source, true, true))
    return false;
  pairs->insert(source->begin(), source->end());
  return true;
}

}  // namespace

void print_reject_option_help(FILE* fp, int description_column) {
  static char const* const lines[] = {
    "discard rows matching an entry listed in FILE;",
    "entries are words or comma-separated pairs;",
    "words match anywhere as whole words;",
    "pairs match complete segments in either order;",
    "may be repeated",
  };
  fprintf(fp, "%-*s%s\n", description_column, "  -r, --reject FILE", lines[0]);
  for (size_t i = 1; i < sizeof(lines) / sizeof(lines[0]); ++i)
    fprintf(fp, "%*s%s\n", description_column, "", lines[i]);
}

void print_allow_pairs_option_help(FILE* fp, int description_column) {
  static char const* const lines[] = {
    "keep rows only when every multi-word segment is",
    "listed in FILE; comma-separated pairs are loaded;",
    "standalone entries are ignored and counted;",
    "pairs match complete segments in either order;",
    "solo-word segments remain unrestricted; may be repeated",
  };
  fputs("  -a, --allow-pairs FILE\n", fp);
  for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); ++i)
    fprintf(fp, "%*s%s\n", description_column, "", lines[i]);
}

PairFilterOptionResult parse_pair_filter_option(
    int argc, char* const argv[], int* index, char const* program,
    PairFilterSupport support, PairFilterOptions* out) {
  char const* const option = argv[*index];
  if (strcmp(option, "--wf") == 0) {
    if (!out->workflow_root.empty()) {
      fprintf(stderr, "%s: --wf and --wfroot are mutually exclusive\n",
          program);
      return PAIR_FILTER_OPTION_ERROR;
    }
    out->workflow = true;
    return PAIR_FILTER_OPTION_HANDLED;
  }
  char const* value;
  if (match_option_value(argc, argv, index, NULL, "--wfroot", &value)) {
    if (out->workflow) {
      fprintf(stderr, "%s: --wf and --wfroot are mutually exclusive\n",
          program);
      return PAIR_FILTER_OPTION_ERROR;
    }
    if (value == NULL || value[0] == '\0') {
      fprintf(stderr, "%s: --wfroot requires a nonempty directory\n",
          program);
      return PAIR_FILTER_OPTION_ERROR;
    }
    out->workflow_root = value;
    return PAIR_FILTER_OPTION_HANDLED;
  }
  if (match_option_value(argc, argv, index, "-d", "--dict", &value)) {
    if (value == NULL || value[0] == '\0') {
      fprintf(stderr, "%s: %s requires a nonempty path\n", program, option);
      return PAIR_FILTER_OPTION_ERROR;
    }
    out->dictionary_path = value;
    return PAIR_FILTER_OPTION_HANDLED;
  }
  if (match_option_value(argc, argv, index, "-t", "--target", &value)) {
    if (value == NULL || value[0] == '\0') {
      fprintf(stderr, "%s: %s requires a nonempty target\n", program, option);
      return PAIR_FILTER_OPTION_ERROR;
    }
    out->target = value;
    return PAIR_FILTER_OPTION_HANDLED;
  }
  if (support.workflow_yes &&
      (strcmp(option, "-y") == 0 || strcmp(option, "--yes") == 0)) {
    out->workflow_yes = true;
    return PAIR_FILTER_OPTION_HANDLED;
  }

  std::vector<std::string>* paths;
  if (support.allow &&
      match_option_value(argc, argv, index, "-a", "--allow-pairs", &value)) {
    paths = &out->allow_paths;
  } else if (support.ignore &&
      match_option_value(argc, argv, index, "-i", "--ignore", &value)) {
    paths = &out->ignore_paths;
  } else if (match_option_value(argc, argv, index, "-r", "--reject", &value)) {
    paths = &out->reject_paths;
  } else {
    return PAIR_FILTER_OPTION_OTHER;
  }

  if (value == NULL) {
    fprintf(stderr, "%s: %s requires a file\n", program, option);
    return PAIR_FILTER_OPTION_ERROR;
  }
  paths->push_back(value);
  return PAIR_FILTER_OPTION_HANDLED;
}

bool check_pair_filter_options(
    PairFilterOptions const& options, char const* program) {
  if (options.workflow || !options.workflow_root.empty()) return true;
  if (options.workflow_yes) {
    fprintf(stderr, "%s: --yes requires --wf or --wfroot\n", program);
    return false;
  }
  if (!options.target.empty()) {
    fprintf(stderr, "%s: --target requires --wf or --wfroot\n", program);
    return false;
  }
  return true;
}

bool load_pair_filters(
    PairFilterOptions const& options, char const* program,
    PairFilterSupport support, PairFilters* out) {
  assert(support.allow || options.allow_paths.empty());
  assert(support.ignore || options.ignore_paths.empty());
  assert(support.workflow_yes || !options.workflow_yes);
  (void) support;

  char const* wfroot = NULL;
  if (options.workflow) {
    wfroot = getenv("WFROOT");
    if (wfroot == NULL || wfroot[0] == '\0') {
      fprintf(stderr,
          "%s: --wf requires WFROOT to be set and nonempty\n", program);
      return false;
    }
  } else if (!options.workflow_root.empty()) {
    wfroot = options.workflow_root.c_str();
  }

  std::string target;
  if (wfroot != NULL) {
    std::string const name = options.target.empty()
        ? default_target_name(options, wfroot) : options.target;
    if (!resolve_target_name(wfroot, name, program, &target)) return false;
    if (!target_agrees_with_input(options, wfroot, target, program))
      return false;
    success(program, "TARGET resolved to %s", target.c_str());
    out->sources.target = target;
  }

  out->allowed.reset();
  if (!options.allow_paths.empty()) {
    size_t ignored_non_pairs = 0;
    out->allowed.emplace();
    for (size_t i = 0; i < options.allow_paths.size(); ++i) {
      if (!load_pair_file_ignoring_single_words(
              options.allow_paths[i].c_str(), "allow list",
              &out->allowed.value(), &ignored_non_pairs))
        return false;
    }
    if (ignored_non_pairs != 0)
      fprintf(stderr,
          "%s: ignored %zu non-pairs in --allow-pairs file(s)\n",
          program, ignored_non_pairs);
  }
  for (size_t i = 0; i < options.ignore_paths.size(); ++i) {
    if (!load_pair_file(
            options.ignore_paths[i].c_str(), "ignore list", &out->ignored,
            true, true))
      return false;
  }
  for (size_t i = 0; i < options.reject_paths.size(); ++i) {
    if (!load_pair_file(
            options.reject_paths[i].c_str(), "reject list", &out->rejected,
            true, true, true))
      return false;
  }
  if (wfroot != NULL) {
    if (!load_workflow_pair_file(
            workflow_path(wfroot, WORKFLOW_NO_PAIRS_PATH), program,
            "reject list", &out->rejected, &out->sources.classified_no))
      return false;
    if (!load_target_pair_file(wfroot, target, program, &out->rejected,
            &out->sources.target_no))
      return false;
    if (options.workflow_yes) {
      if (!load_workflow_pair_file(
              workflow_path(wfroot, WORKFLOW_YES_PAIRS_PATH), program,
              "ignore list", &out->ignored, &out->sources.classified_yes))
        return false;
    }
  }

  if (!options.dictionary_path.empty())
    return load_dictionary(options.dictionary_path.c_str(), &out->dictionary);
  if (wfroot == NULL) {
    warn(program, "NO DICTIONARY SUPPLIED");
    return true;
  }
  std::string const dict_path = workflow_path(wfroot, WORKFLOW_DICT_PATH);
  if (workflow_file_missing(dict_path, program, "dictionary")) return true;
  return load_dictionary(dict_path.c_str(), &out->dictionary);
}

bool is_rejected_segment(
    DfsPairSet const& rejected, std::string const& segment) {
  if (rejected.find(segment) != rejected.end()) return true;
  size_t start = 0;
  while (true) {
    size_t const end = segment.find(' ', start);
    std::string const word = end == std::string::npos
        ? segment.substr(start) : segment.substr(start, end - start);
    if (rejected.find(word) != rejected.end()) return true;
    if (end == std::string::npos) return false;
    start = end + 1;
  }
}

bool is_allowed_segment(
    std::optional<DfsPairSet> const& allowed, std::string const& segment) {
  if (segment.find(' ') == std::string::npos || !allowed.has_value())
    return true;
  return allowed->find(segment) != allowed->end();
}

bool all_words_in_dict(
    DfsDictionary const& dictionary, std::string const& segment) {
  if (dictionary.empty()) return true;
  size_t start = 0;
  while (true) {
    size_t const end = segment.find(' ', start);
    if (end == std::string::npos) {
      return start == 0
          ? dictionary.find(segment) != dictionary.end()
          : dictionary.find(segment.substr(start)) != dictionary.end();
    }
    if (dictionary.find(segment.substr(start, end - start)) ==
        dictionary.end())
      return false;
    start = end + 1;
  }
}

PairFilterLayer PairFilters::first_rejecting_layer(
    std::string const& segment) const {
  if (sources.classified_no.find(segment) != sources.classified_no.end())
    return PAIR_FILTER_CLASSIFIED_NO;
  if (sources.target_no.find(segment) != sources.target_no.end())
    return PAIR_FILTER_TARGET_NO;
  if (is_rejected_segment(rejected, segment))
    return PAIR_FILTER_EXPLICIT_REJECT;
  if (!is_allowed_segment(allowed, segment)) return PAIR_FILTER_ALLOWLIST;
  if (!all_words_in_dict(dictionary, segment)) return PAIR_FILTER_DICTIONARY;
  return PAIR_FILTER_NONE;
}
