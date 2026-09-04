#include "pair-exclusions.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <string>
#include "dfs-cli-args.h"

char const* const WORKFLOW_NO_PAIRS_PATH = ".wf/classified/no/no.pairs";
char const* const WORKFLOW_YES_PAIRS_PATH = ".wf/classified/yes/yes.pairs";
char const* const WORKFLOW_DICT_PATH = ".wf/best/dict/words.big";

namespace {

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
