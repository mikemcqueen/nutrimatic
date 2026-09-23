#include "classified.h"

#include <stdio.h>

#include <string>
#include <system_error>

#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "workflow-paths.h"

void classified_help_sentence() {
  dfs_help_option("-s, --sentence N",
      "drop pairs listed in $WFROOT/%s/sN/no/no.pairs",
      WORKFLOW_CLASSIFIED_PATH);
}

bool parse_classified_sentence(char const* arg, int* sentence) {
  return parse_count(arg, "--sentence", sentence);
}

std::filesystem::path classified_sentence_dir(char const* root, int sentence) {
  return std::filesystem::path(root) / WORKFLOW_CLASSIFIED_PATH /
      ("s" + std::to_string(sentence));
}

bool load_classified_no_pairs(
    char const* program, char const* root, int sentence, DfsPairSet* pairs) {
  std::filesystem::path const path =
      classified_sentence_dir(root, sentence) / "no" / "no.pairs";
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) {
    fprintf(stderr, "%s: \"%s\" is not a file\n", program, path.c_str());
    return false;
  }
  return load_pair_file(path.c_str(), "reject list", pairs, true, true);
}
