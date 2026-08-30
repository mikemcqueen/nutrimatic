#include "pair-exclusions.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <string>
#include <vector>

#include "dfs-cli-args.h"

namespace {

std::string workflow_path(char const* root, char const* relative) {
  std::string path(root);
  if (!path.empty() && path.back() != '/') path.push_back('/');
  path.append(relative);
  return path;
}

bool load_workflow_pair_file(
    std::string const& path, char const* program, DfsPairSet* excluded) {
  struct stat status;
  if (stat(path.c_str(), &status) != 0 &&
      (errno == ENOENT || errno == ENOTDIR)) {
    fprintf(stderr,
        "%s: WARNING: classified pair file \"%s\" is not present\n",
        program, path.c_str());
    return true;
  }
  return load_pair_file(
      path.c_str(), "exclude list", excluded, true, true);
}

}  // namespace

PairExclusionOptionResult parse_pair_exclusion_option(
    int argc, char* const argv[], int* index, char const* program,
    PairExclusionOptions* out) {
  char const* const option = argv[*index];
  if (strcmp(option, "--wf") == 0) {
    out->workflow = true;
    return PAIR_EXCLUSION_OPTION_HANDLED;
  }
  if (strcmp(option, "-x") != 0 && strcmp(option, "--exclude") != 0)
    return PAIR_EXCLUSION_OPTION_OTHER;

  if (++*index == argc) {
    fprintf(stderr, "%s: %s requires a file\n", program, option);
    return PAIR_EXCLUSION_OPTION_ERROR;
  }
  out->paths.push_back(argv[*index]);
  return PAIR_EXCLUSION_OPTION_HANDLED;
}

bool load_pair_exclusions(
    PairExclusionOptions const& options, char const* program,
    DfsPairSet* excluded) {
  char const* wfroot = NULL;
  if (options.workflow) {
    wfroot = getenv("WFROOT");
    if (wfroot == NULL || wfroot[0] == '\0') {
      fprintf(stderr,
          "%s: --wf requires WFROOT to be set and nonempty\n", program);
      return false;
    }
  }

  for (size_t i = 0; i < options.paths.size(); ++i) {
    if (!load_pair_file(
            options.paths[i].c_str(), "exclude list", excluded, true, true))
      return false;
  }
  if (!options.workflow) return true;

  std::vector<std::string> const workflow_paths = {
    workflow_path(wfroot, ".wf/classified/yes/yes.pairs"),
    workflow_path(wfroot, ".wf/classified/no/no.pairs"),
  };
  for (size_t i = 0; i < workflow_paths.size(); ++i) {
    if (!load_workflow_pair_file(workflow_paths[i], program, excluded))
      return false;
  }
  return true;
}
