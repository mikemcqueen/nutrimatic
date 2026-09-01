#include "pair-exclusions.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <string>
#include "dfs-cli-args.h"

namespace {

char const* const workflow_yes_path =
    ".wf/classified/yes/yes.pairs";
char const* const workflow_no_path =
    ".wf/classified/no/no.pairs";

std::string workflow_path(char const* root, char const* relative) {
  std::string path(root);
  if (!path.empty() && path.back() != '/') path.push_back('/');
  path.append(relative);
  return path;
}

bool load_workflow_pair_file(
    std::string const& path, char const* program, char const* description,
    DfsPairSet* pairs) {
  struct stat status;
  if (stat(path.c_str(), &status) != 0 &&
      (errno == ENOENT || errno == ENOTDIR)) {
    fprintf(stderr,
        "%s: WARNING: classified pair file \"%s\" is not present\n",
        program, path.c_str());
    return true;
  }
  return load_pair_file(path.c_str(), description, pairs, true, true);
}

}  // namespace

PairFilterOptionResult parse_pair_filter_option(
    int argc, char* const argv[], int* index, char const* program,
    bool support_ignore, bool support_workflow_yes, PairFilterOptions* out) {
  char const* const option = argv[*index];
  if (strcmp(option, "--wf") == 0) {
    out->workflow = true;
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
    DfsPairSet* ignored, DfsPairSet* rejected) {
  char const* wfroot = NULL;
  if (options.workflow) {
    wfroot = getenv("WFROOT");
    if (wfroot == NULL || wfroot[0] == '\0') {
      fprintf(stderr,
          "%s: --wf requires WFROOT to be set and nonempty\n", program);
      return false;
    }
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
  if (!options.workflow) return true;

  if (!load_workflow_pair_file(
          workflow_path(wfroot, workflow_no_path), program, "reject list",
          rejected))
    return false;
  if (options.workflow_yes) {
    if (!load_workflow_pair_file(
            workflow_path(wfroot, workflow_yes_path), program, "ignore list",
            ignored))
      return false;
  }
  return true;
}
