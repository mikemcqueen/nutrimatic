#include "pair-exclusions.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <string>
#include <vector>

#include "dfs-cli-args.h"

char const* const WORKFLOW_NO_PAIRS_PATH = ".wf/classified/no/no.pairs";
char const* const WORKFLOW_YES_PAIRS_PATH = ".wf/classified/yes/yes.pairs";
char const* const WORKFLOW_DICT_PATH = ".wf/best/dict/words.big";
char const* const WORKFLOW_TARGET_NO_PAIRS_PATH =
    ".wf/best/SENTENCE/LETTERS/mN/gN/no.pairs";
char const* const WORKFLOW_RESULTS_NAME =
    "dfs.SENTENCE[.SEED].mN.x2.gN[.best].LIMIT.LETTERS";

namespace {

// The two halves of WORKFLOW_TARGET_NO_PAIRS_PATH that are matched rather
// than displayed: everything above a target, and the file inside one.
char const* const WORKFLOW_DIR_NAME = ".wf";
char const* const WORKFLOW_BEST_PATH = ".wf/best";
char const* const WORKFLOW_TARGET_PATH = ".wf/best/SENTENCE/LETTERS/mN/gN";
char const* const TARGET_NO_PAIRS_NAME = "no.pairs";

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
  fprintf(stderr, "%s: WARNING: %s \"%s\" is not present\n",
      program, description, path.c_str());
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

// The target directory `input` is an artifact of, if it is one.
//
// A workflow artifact sits directly in a target directory, so the candidate
// is just the file's own directory and the question is whether that
// directory has a target's shape below the selected root: four components,
// the last two counted. The dirname is taken lexically and only then
// resolved, because an artifact may be a symlink to results kept elsewhere
// -- what names the target is where the link sits, not where it points.
// Both sides are resolved so a relative input and a relative root compare,
// and so a symlinked .wf resolves the same way on each.
bool workflow_target_dir(
    std::string const& input, char const* wfroot, std::string* out) {
  std::string dir, best;
  if (!resolve(lexical_dirname(input), &dir)) return false;
  if (!resolve(workflow_path(wfroot, WORKFLOW_BEST_PATH), &best)) return false;
  if (best.empty() || best.back() != '/') best.push_back('/');
  if (dir.compare(0, best.size(), best) != 0) return false;

  std::vector<std::string> const parts = split(dir.substr(best.size()), '/');
  if (parts.size() != 4) return false;
  if (!is_counted(parts[2], 'm') || !is_counted(parts[3], 'g')) return false;
  *out = dir;
  return true;
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

// The target directory `input` names, for a dfs-anagrams results file kept
// outside the workflow tree.
//
// The workflow renders those names as
// dfs.SENTENCE[.SEED].mN.x2.gN[.best].LIMIT.LETTERS, so every component of
// a target address is in the name -- but the seed annotation between the
// sentence and mN is opaque and may be any number of components, so the
// address is read inward from both ends and never straight through. The
// directory the file is in says nothing here: results are kept wherever the
// search was told to put them, so the target is rebuilt under the selected
// root instead, and having to exist there is what makes a misread name a
// warning rather than silently no exclusions.
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

  std::string const target = workflow_path(wfroot, WORKFLOW_BEST_PATH) +
      "/" + parts[1] + "/" + letter_set + "/" + universe + "/" + segments;
  if (!is_directory(target)) return false;
  *out = target;
  return true;
}

// Whether any component of `path` is the workflow directory.
bool under_workflow_dir(std::string const& path) {
  std::vector<std::string> const parts = split(path, '/');
  for (size_t i = 0; i < parts.size(); ++i)
    if (parts[i] == WORKFLOW_DIR_NAME) return true;
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

// Rejects the target no.pairs belonging to the file the tool will read.
//
// Never fatal on absence in either sense: a target that cannot be identified
// warns and a target with no exclusions is silent, and both leave `rejected`
// holding just the root-level set. A file that is there but unreadable or
// malformed is an error like any other reject list.
bool load_target_pair_file(
    PairFilterOptions const& options, char const* wfroot, char const* program,
    DfsPairSet* rejected) {
  if (options.input_path.empty()) {
    fprintf(stderr,
        "%s: WARNING: no single input file to name a workflow target; "
        "TARGET/%s was not applied\n", program, TARGET_NO_PAIRS_NAME);
    return true;
  }
  std::string target;
  if (!workflow_target(options.input_path, wfroot, &target)) {
    fprintf(stderr,
        "%s: WARNING: no workflow target for \"%s\": not in "
        "ROOT/%s, and not named %s; TARGET/%s was not applied\n",
        program, options.input_path.c_str(), WORKFLOW_TARGET_PATH,
        WORKFLOW_RESULTS_NAME, TARGET_NO_PAIRS_NAME);
    return true;
  }

  std::string const path = target + "/" + TARGET_NO_PAIRS_NAME;
  struct stat status;
  if (stat(path.c_str(), &status) == 0)
    return load_pair_file(path.c_str(), "reject list", rejected, true, true);
  // Having no exclusions is the ordinary case and says nothing, but a link
  // with nothing under it is a tree to fix: targets keep this file as a link
  // into the results it was written beside, and a stale one would read as
  // the ordinary case and quietly filter nothing.
  if (lstat(path.c_str(), &status) == 0)
    fprintf(stderr,
        "%s: WARNING: \"%s\" leads nowhere; it was not applied\n",
        program, path.c_str());
  return true;
}

bool load_workflow_pair_file(
    std::string const& path, char const* program, char const* description,
    DfsPairSet* pairs) {
  if (workflow_file_missing(path, program, "classified pair file"))
    return true;
  return load_pair_file(path.c_str(), description, pairs, true, true);
}

}  // namespace

PairFilterOptionResult parse_pair_filter_option(
    int argc, char* const argv[], int* index, char const* program,
    bool support_ignore, bool support_workflow_yes, PairFilterOptions* out) {
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
  if (strcmp(option, "--wfroot") == 0) {
    if (out->workflow) {
      fprintf(stderr, "%s: --wf and --wfroot are mutually exclusive\n",
          program);
      return PAIR_FILTER_OPTION_ERROR;
    }
    if (++*index == argc || argv[*index][0] == '\0') {
      fprintf(stderr, "%s: --wfroot requires a nonempty directory\n",
          program);
      return PAIR_FILTER_OPTION_ERROR;
    }
    out->workflow_root = argv[*index];
    return PAIR_FILTER_OPTION_HANDLED;
  }
  if (support_workflow_yes &&
      (strcmp(option, "-y") == 0 || strcmp(option, "--yes") == 0)) {
    out->workflow_yes = true;
    return PAIR_FILTER_OPTION_HANDLED;
  }

  std::vector<std::string>* paths;
  if (support_ignore &&
      (strcmp(option, "-i") == 0 || strcmp(option, "--ignore") == 0)) {
    paths = &out->ignore_paths;
  } else if (strcmp(option, "-r") == 0 ||
             strcmp(option, "--reject") == 0) {
    paths = &out->reject_paths;
  } else {
    return PAIR_FILTER_OPTION_OTHER;
  }

  if (++*index == argc) {
    fprintf(stderr, "%s: %s requires a file\n", program, option);
    return PAIR_FILTER_OPTION_ERROR;
  }
  paths->push_back(argv[*index]);
  return PAIR_FILTER_OPTION_HANDLED;
}

bool load_pair_filters(
    PairFilterOptions const& options, char const* program,
    DfsPairSet* ignored, DfsPairSet* rejected, DfsDictionary* dictionary) {
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

  for (size_t i = 0; i < options.ignore_paths.size(); ++i) {
    if (!load_pair_file(
            options.ignore_paths[i].c_str(), "ignore list", ignored,
            true, true))
      return false;
  }
  for (size_t i = 0; i < options.reject_paths.size(); ++i) {
    if (!load_pair_file(
            options.reject_paths[i].c_str(), "reject list", rejected,
            true, true))
      return false;
  }
  if (wfroot == NULL) {
    fprintf(stderr, "%s: WARNING: NO DICTIONARY SUPPLIED\n", program);
    return true;
  }

  if (!load_workflow_pair_file(
          workflow_path(wfroot, WORKFLOW_NO_PAIRS_PATH), program,
          "reject list", rejected))
    return false;
  if (!load_target_pair_file(options, wfroot, program, rejected))
    return false;
  if (options.workflow_yes) {
    if (!load_workflow_pair_file(
            workflow_path(wfroot, WORKFLOW_YES_PAIRS_PATH), program,
            "ignore list", ignored))
      return false;
  }

  std::string const dict_path = workflow_path(wfroot, WORKFLOW_DICT_PATH);
  if (workflow_file_missing(dict_path, program, "dictionary")) return true;
  return load_dictionary(dict_path.c_str(), dictionary);
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
