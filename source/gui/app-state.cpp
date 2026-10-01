#include "app-state.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "dfs-cli-args.h"
#include "input-source.h"
#include "workflow-paths.h"

namespace {

// The pieces of `text` between each `separator`.
std::vector<std::string_view> split(std::string_view text, char separator) {
  std::vector<std::string_view> pieces;
  for (;;) {
    size_t const end = text.find(separator);
    pieces.push_back(text.substr(0, end));
    if (end == std::string_view::npos) return pieces;
    text.remove_prefix(end + 1);
  }
}

// The seed map key for the seed file `name`: "seed" and its cutoff for
// seed.S.M.all.CUTOFF[...].pairs ("seed85" for seed.s9.m3.all.85.15.pairs),
// "all" for all.S.M, or "seed" otherwise.
std::string get_seed_key(std::string const& name) {
  std::vector<std::string_view> const pieces = split(name, '.');
  if (pieces[0] == "seed" && pieces.size() > 4)
    return "seed" + std::string(pieces[4]);
  if (pieces[0] == "all") return "all";
  return "seed";
}

// The name of the `type` seed file sharing the S.M of the seed file `name`:
// seed.S.M.all.CUTOFF.pairs for "seed", all.S.M for "all", or none when
// `name` has no S.M, or `type` is "seed" with no `cutoff`, or is neither.
std::optional<std::string> build_seed_name(std::string const& name,
                                           std::string_view type,
                                           std::string_view cutoff = {}) {
  std::vector<std::string_view> const pieces = split(name, '.');
  if (pieces.size() < 3) return std::nullopt;
  std::string const sm = std::string(pieces[1]) + "." + std::string(pieces[2]);
  if (type == "seed") {
    if (cutoff.empty()) return std::nullopt;
    return "seed." + sm + ".all." + std::string(cutoff) + ".pairs";
  }
  if (type == "all") return "all." + sm;
  return std::nullopt;
}

// Adds `name` to `map` under `key`, unless `key` is already there, or `name`
// is none or names no file in `directory`.
void add_seed_if_exists(SeedMap& map, std::string const& directory,
                        std::string const& key,
                        std::optional<std::string> const& name) {
  if (!name || map.contains(key)) return;
  if (!std::filesystem::exists(std::filesystem::path(directory) / *name))
    return;
  map.emplace(key, *name);
}

// Adds to `map` the seed50, seed85, and all seed files in `directory` sharing
// the S.M of the seed file `name`.
void add_other_seed_files(SeedMap& map, std::string const& directory,
                          std::string const& name) {
  add_seed_if_exists(map, directory, "seed50",
                     build_seed_name(name, "seed", "50"));
  add_seed_if_exists(map, directory, "seed85",
                     build_seed_name(name, "seed", "85.15"));
  add_seed_if_exists(map, directory, "all", build_seed_name(name, "all"));
}

// The judged-bad file in `directory`.
std::string judged_bad_path(std::string const& directory) {
  return (std::filesystem::path(directory) / "judged-bad").string();
}

// The keys of `map`, sorted.
template <typename Map>
std::vector<std::string> sorted_keys(Map const& map) {
  std::vector<std::string> keys;
  for (auto const& [key, value] : map) keys.push_back(key);
  std::ranges::sort(keys);
  return keys;
}

}  // namespace

bool load_app_state(std::string const& seed_path) {
  char const* const root = require_workflow_root("pgui");
  if (root == NULL) return false;
  std::string const path =
      (std::filesystem::path(root) / WORKFLOW_DICT_PATH).string();
  AppState& state = app_state();
  state.set_letters("$S2");
  state.used_letters.clear();
  state.dictionaries.clear();
  state.dictionary_words.clear();
  state.dictionaries["sml_dict"].path = "/usr/share/dict/words";
  AppState::Dictionary& big = state.dictionaries["big_dict"];
  big.path = path;
  if (!load_dictionary(path.c_str(), &big.words)) return false;
  big.loaded = true;
  state.dictionary_keys = sorted_keys(state.dictionaries);
  std::filesystem::path const seed(seed_path);
  state.seed_directory = seed.parent_path().string();
  state.load_seed(seed.filename().string());
  state.judged_bad.clear();
  std::string const judged = judged_bad_path(state.seed_directory);
  Lines lines;
  if (std::filesystem::exists(judged) && read_file_lines(judged, &lines)) {
    for (std::string& line : lines) {
      if (!line.empty()) state.judged_bad.insert(std::move(line));
    }
  }
  ++state.judged_bad_version;
  return true;
}

void AppState::load_seed(std::string name) {
  seed_name = std::move(name);
  seed_map.clear();
  seed_key = get_seed_key(seed_name);
  seed_map.emplace(seed_key, seed_name);
  add_other_seed_files(seed_map, seed_directory, seed_name);
  seed_keys = sorted_keys(seed_map);
  auto lines = std::make_shared<Lines>();
  seed_readable = read_file_lines(
      (std::filesystem::path(seed_directory) / seed_name).string(),
      lines.get());
  if (!seed_readable) lines->clear();
  seeds.clear();
  seeds.emplace(seed_key, std::move(lines));
  ++generation;
}

SharedLines AppState::seed_lines(std::string const& key) {
  if (auto const found = seeds.find(key); found != seeds.end())
    return found->second;
  auto lines = std::make_shared<Lines>();
  auto const file = seed_map.find(key);
  if (file == seed_map.end() ||
      !read_file_lines(
          (std::filesystem::path(seed_directory) / file->second).string(),
          lines.get()))
    lines->clear();
  return seeds.emplace(key, std::move(lines)).first->second;
}

SharedLines AppState::dictionary_lines(std::string const& key) {
  if (auto const found = dictionary_words.find(key);
      found != dictionary_words.end())
    return found->second;
  auto lines = std::make_shared<Lines>();
  if (auto const found = dictionaries.find(key); found != dictionaries.end()) {
    Dictionary& dictionary = found->second;
    if (!dictionary.loaded &&
        !load_dictionary(dictionary.path.c_str(), &dictionary.words))
      dictionary.words.clear();
    dictionary.loaded = true;
    lines->assign(dictionary.words.begin(), dictionary.words.end());
    std::ranges::sort(*lines);
  }
  return dictionary_words.emplace(key, std::move(lines)).first->second;
}

void AppState::add_judged_bad(std::string letters) {
  if (!judged_bad.insert(letters).second) return;
  ++judged_bad_version;
  std::string const path = judged_bad_path(seed_directory);
  std::ofstream file(path, std::ios::app);
  file << letters << '\n';
  file.close();
  if (!file) std::fprintf(stderr, "pgui: can't append to %s\n", path.c_str());
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
