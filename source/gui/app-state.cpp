#include "app-state.h"

#include <cstdlib>
#include <filesystem>
#include <utility>

#include "dfs-cli-args.h"
#include "input-source.h"
#include "workflow-paths.h"

bool load_app_state(std::string const& seed_path) {
  char const* const root = require_workflow_root("pgui");
  if (root == NULL) return false;
  std::string const path =
      (std::filesystem::path(root) / WORKFLOW_DICT_PATH).string();
  AppState& state = app_state();
  state.set_letters("");
  state.used_letters.clear();
  if (!load_dictionary(path.c_str(), &state.dictionary)) return false;
  std::filesystem::path const seed(seed_path);
  state.seed_directory = seed.parent_path().string();
  state.load_seed(seed.filename().string());
  return true;
}

void AppState::load_seed(std::string name) {
  seed_name = std::move(name);
  auto lines = std::make_shared<Lines>();
  seed_readable = read_file_lines(
      (std::filesystem::path(seed_directory) / seed_name).string(),
      lines.get());
  if (!seed_readable) lines->clear();
  seed = std::move(lines);
  ++generation;
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
