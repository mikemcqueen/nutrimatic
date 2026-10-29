#ifndef NUTRIMATIC_WORKFLOW_PATHS_H
#define NUTRIMATIC_WORKFLOW_PATHS_H

#include <stdio.h>
#include <stdlib.h>

#include <filesystem>
#include <optional>
#include <string_view>
#include <system_error>

#include "log.h"

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
inline constexpr char WORKFLOW_SMALL_DICT_PATH[] = ".wf/dict/dict.filtered";
inline constexpr char WORKFLOW_REVIEWED_WORDS_PATH[] =
    ".wf/dict/done/reviewed.words";
inline constexpr char WORKFLOW_P1_OUT_PATH[] = ".wf/p1/done/out";

// Paths relative to the working directory: the directory of the sentence
// pair files and p1.idx, and the cluer clue data.
inline constexpr char IDX_PATH[] = "idx";
inline constexpr char P1_INDEX_PATH[] = "idx/p1.idx";
inline constexpr char CLUEDATA_PATH[] = "cluer/data/cluedata";

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
inline bool check_workflow_root(std::string_view program,
                                std::filesystem::path const& root) {
  std::error_code error;
  if (std::filesystem::is_directory(root / WORKFLOW_DIR_PATH, error))
    return true;
  report(program, "workflow root \"%s\" has no .wf directory", root.c_str());
  return false;
}

// The nonempty WFROOT, checked by check_workflow_root(); empty, diagnosed
// prefixed by `program`, when it is unset, empty or fails the check.
inline std::filesystem::path require_workflow_root(std::string_view program) {
  char const* const root = getenv("WFROOT");
  if (root == NULL || root[0] == '\0') {
    report(program, "WFROOT must be set and nonempty");
    return {};
  }
  return check_workflow_root(program, root) ? root : std::filesystem::path();
}

// Sets an unset `*path` to `relative` under require_workflow_root(). False
// when the root is needed and unusable, diagnosed prefixed by `program`.
inline bool default_workflow_path(std::string_view program,
                                  std::optional<std::filesystem::path>* path,
                                  std::string_view relative) {
  if (*path) return true;
  std::filesystem::path const root = require_workflow_root(program);
  if (root.empty()) return false;
  *path = root / relative;
  return true;
}

#endif
