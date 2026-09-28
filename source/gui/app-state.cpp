#include "app-state.h"

#include <cstdlib>
#include <filesystem>
#include <utility>

#include "dfs-cli-args.h"
#include "workflow-paths.h"

bool load_app_state() {
  char const* const root = require_workflow_root("pgui");
  if (root == NULL) return false;
  std::string const path =
      (std::filesystem::path(root) / WORKFLOW_DICT_PATH).string();
  AppState& state = app_state();
  state.set_letters("");
  state.used_letters.clear();
  return load_dictionary(path.c_str(), &state.dictionary);
}

void AppState::set_letters(std::string letters) {
  if (letters.starts_with('$')) {
    char const* const value = std::getenv(letters.c_str() + 1);
    actual_letters = value ? value : "";
  } else {
    actual_letters = letters;
  }
  visual_letters = std::move(letters);
}

AppState& app_state() {
  static AppState state;
  return state;
}
