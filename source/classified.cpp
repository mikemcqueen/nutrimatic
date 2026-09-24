#include "classified.h"

#include <stdio.h>

#include <charconv>
#include <string>
#include <system_error>

#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "log.h"
#include "workflow-paths.h"

void classified_help_sentence_no() {
  dfs_help_option("-s, --sentence N",
      "drop pairs listed in $WFROOT/%s/sN/no/no.pairs",
      WORKFLOW_CLASSIFIED_PATH);
}

void classified_help_sentence_pairs() {
  dfs_help_option("-s, --sentence N",
      "load $WFROOT/%s/sN/yes/yes.pairs as YES pairs and exclude "
      "sN/no/no.pairs as if it were a pairs-only --reject file; both must "
      "exist; --wfroot DIR replaces $WFROOT; does not require --wf; a "
      "--target must be in sentence N",
      WORKFLOW_CLASSIFIED_PATH);
}

bool parse_classified_sentence(char const* arg, int* sentence) {
  return parse_count(arg, "--sentence", sentence);
}

bool classified_pair_file(
    char const* program, char const* root, int sentence, char const* kind,
    std::string* path) {
  std::filesystem::path dir =
      std::filesystem::path(root) / WORKFLOW_CLASSIFIED_PATH;
  if (sentence != CLASSIFIED_NO_SENTENCE)
    dir /= "s" + std::to_string(sentence);
  std::filesystem::path const file =
      dir / kind / (std::string(kind) + ".pairs");
  path->clear();
  std::error_code error;
  if (sentence == CLASSIFIED_NO_SENTENCE) {
    if (!std::filesystem::exists(file, error) && !error) {
      warn(program, "classified pair file \"%s\" is not present",
          file.c_str());
      return true;
    }
  } else if (!std::filesystem::is_regular_file(file, error)) {
    fprintf(stderr, "%s: \"%s\" is not a file\n", program, file.c_str());
    return false;
  }
  *path = file.string();
  return true;
}

bool load_classified_pairs(
    char const* program, char const* root, int sentence, char const* kind,
    DfsPairSet* pairs) {
  std::string path;
  if (!classified_pair_file(program, root, sentence, kind, &path))
    return false;
  return path.empty() ||
      load_pair_file(path.c_str(), "classified pair list", pairs, true, true);
}

bool load_global_no_pairs(
    char const* program, char const* root, DfsPairSet* pairs) {
  return load_classified_pairs(
      program, root, CLASSIFIED_NO_SENTENCE, "no", pairs);
}

bool load_sentence_no_pairs(
    char const* program, char const* root, int sentence, DfsPairSet* pairs) {
  return load_classified_pairs(program, root, sentence, "no", pairs);
}

bool add_classified_sentence_pairs(
    char const* program, int sentence, DfsCommonArgs* args,
    std::vector<std::string>* reject_files) {
  if (args->pair_file != NULL) {
    fprintf(stderr, "%s: --pairs cannot be combined with --sentence\n",
            program);
    return false;
  }
  char const* root;
  if (args->workflow_root.empty()) {
    root = require_workflow_root(program);
    if (root == NULL) return false;
  } else {
    root = args->workflow_root.c_str();
    if (!check_workflow_root(program, root)) return false;
  }
  std::string yes;
  std::string no;
  if (!classified_pair_file(program, root, sentence, "yes", &yes) ||
      !classified_pair_file(program, root, sentence, "no", &no))
    return false;
  args->yes_pair_files.push_back(yes);
  reject_files->push_back(no);
  return true;
}

bool check_classified_sentence_target(
    char const* program, int sentence, std::string const& target) {
  if (sentence == CLASSIFIED_NO_SENTENCE || target.empty()) return true;
  int target_sentence = -1;
  if (target[0] == 's')
    std::from_chars(
        target.data() + 1, target.data() + target.size(), target_sentence);
  if (target_sentence == sentence) return true;
  fprintf(stderr, "%s: --sentence %d does not match target \"%s\"\n",
          program, sentence, target.c_str());
  return false;
}
