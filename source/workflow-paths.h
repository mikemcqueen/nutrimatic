#ifndef NUTRIMATIC_WORKFLOW_PATHS_H
#define NUTRIMATIC_WORKFLOW_PATHS_H

#include <stdio.h>
#include <stdlib.h>

#include <filesystem>
#include <system_error>

// Return the configured workflow root, or NULL when it is unset or empty.
inline char const* workflow_root_from_env() {
  char const* const root = getenv("WFROOT");
  return root != NULL && root[0] != '\0' ? root : NULL;
}

// Workflow-root-relative paths and target-local artifact names shared by the
// DFS/query and segment-selection tools. Keep these aligned with the workflow
// producer rather than spelling paths independently in each consumer.
inline constexpr char WORKFLOW_DIR_PATH[] = ".wf";
inline constexpr char WORKFLOW_BEST_PATH[] = ".wf/best";
inline constexpr char WORKFLOW_INDEX_PATH[] =
    ".wf/best/idx/wiki-merged.2.index";
inline constexpr char WORKFLOW_CLASSIFIED_PATH[] = ".wf/classified";
inline constexpr char WORKFLOW_NO_PAIRS_PATH[] =
    ".wf/classified/no/no.pairs";
inline constexpr char WORKFLOW_YES_PAIRS_PATH[] =
    ".wf/classified/yes/yes.pairs";
inline constexpr char WORKFLOW_DICT_PATH[] = ".wf/dict/words.filtered";

inline constexpr char WORKFLOW_TARGET_NO_PAIRS_NAME[] = "no.pairs";
inline constexpr char WORKFLOW_TARGET_BEST_PAIRS_NAME[] = "best.pairs";
inline constexpr char WORKFLOW_TARGET_PATH[] =
    ".wf/best/SENTENCE/LETTERS/mN/gN";
inline constexpr char WORKFLOW_TARGET_NO_PAIRS_PATH[] =
    ".wf/best/SENTENCE/LETTERS/mN/gN/no.pairs";

inline constexpr char WORKFLOW_DEFAULT_TARGET[] = "current";
inline constexpr char WORKFLOW_RESULTS_NAME[] =
    "dfs.SENTENCE[.SEED].mN.x2.gN[.best].LIMIT.LETTERS";

// Whether `root` holds a .wf directory. Diagnoses a failure prefixed by
// `program`.
inline bool check_workflow_root(char const* program, char const* root) {
  std::error_code error;
  if (std::filesystem::is_directory(
          std::filesystem::path(root) / WORKFLOW_DIR_PATH, error))
    return true;
  fprintf(stderr, "%s: workflow root \"%s\" has no .wf directory\n",
      program, root);
  return false;
}

// As workflow_root_from_env(), but also requires check_workflow_root(), and
// diagnoses either failure prefixed by `program`.
inline char const* require_workflow_root(char const* program) {
  char const* const root = workflow_root_from_env();
  if (root == NULL) {
    fprintf(stderr, "%s: WFROOT must be set and nonempty\n", program);
    return NULL;
  }
  return check_workflow_root(program, root) ? root : NULL;
}

#endif
