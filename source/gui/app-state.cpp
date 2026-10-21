#include "app-state.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "bad-bag-bitmap.h"
#include "dfs-cli-args.h"
#include "input-source.h"
#include "letter-bag.h"
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

// The mN component of the seed file `name`, <prefix>.sN.mN.<suffix>; empty
// when `name` has no mN.
std::string_view get_seed_m(std::string_view name) {
  std::vector<std::string_view> const pieces = split(name, '.');
  return pieces.size() > 2 ? pieces[2] : std::string_view();
}

// The seed map key for the seed file `name`: "seed", its cutoff, and its mN
// for seed.S.M.all.CUTOFF[...].pairs ("seed85m3" for
// seed.s9.m3.all.85.15.pairs), "all" and its mN for all.S.M, or "seed"
// otherwise.
std::string get_seed_key(std::string const& name) {
  std::vector<std::string_view> const pieces = split(name, '.');
  if (pieces[0] == "seed" && pieces.size() > 4)
    return "seed" + std::string(pieces[4]) + std::string(pieces[2]);
  if (pieces[0] == "all" && pieces.size() > 2)
    return "all" + std::string(pieces[2]);
  return "seed";
}

// The universe component of the seed file `name`: U for seed.S.M.U.CUTOFF[...]
// .pairs, "all" otherwise.
std::string_view get_seed_universe(std::string_view name) {
  std::vector<std::string_view> const pieces = split(name, '.');
  return pieces[0] == "seed" && pieces.size() > 4 ? pieces[3] : "all";
}

// The name of the `type` seed file with the sN and universe of the seed file
// `name` and the mN `m`: seed.S.M.U.CUTOFF.pairs for "seed", all.S.M for
// "all", or none when `name` has no sN, or `type` is "seed" with no
// `cutoff`, or is neither.
std::optional<std::string> build_seed_name(std::string const& name,
                                           std::string_view type,
                                           std::string_view m,
                                           std::string_view cutoff = {}) {
  std::vector<std::string_view> const pieces = split(name, '.');
  if (pieces.size() < 2) return std::nullopt;
  std::string const sm = std::string(pieces[1]) + "." + std::string(m);
  if (type == "seed") {
    if (cutoff.empty()) return std::nullopt;
    return "seed." + sm + "." + std::string(get_seed_universe(name)) + "." +
           std::string(cutoff) + ".pairs";
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

// Adds to `map` the seed50, seed85, and all seed files in `directory` with
// the sN (and, for seed50 and seed85, the universe) of the seed file `name`, for each of m3 and m4, under keys suffixed
// with m3 or m4.
void add_other_seed_files(SeedMap& map, std::string const& directory,
                          std::string const& name) {
  for (std::string const m : {"m3", "m4"}) {
    add_seed_if_exists(map, directory, "seed50" + m,
                       build_seed_name(name, "seed", m, "50"));
    add_seed_if_exists(map, directory, "seed85" + m,
                       build_seed_name(name, "seed", m, "85.15"));
    add_seed_if_exists(map, directory, "all" + m,
                       build_seed_name(name, "all", m));
  }
}

// The judged-bad file in `directory`.
std::string judged_bad_path(std::string const& directory) {
  return (std::filesystem::path(directory) / "judged-bad").string();
}

// The sN component of the seed file `name`, <prefix>.sN.<suffix>; empty
// when `name` has no '.'.
std::string_view get_seed_sentence(std::string_view name) {
  std::vector<std::string_view> const pieces = split(name, '.');
  return pieces.size() > 1 ? pieces[1] : std::string_view();
}

// Whether `sentence` is s followed by one digit.
bool valid_seed_sentence(std::string_view sentence) {
  return sentence.size() == 2 && sentence[0] == 's' &&
         sentence[1] >= '0' && sentence[1] <= '9';
}

// Adds each nonblank line of the file at `path` to `bags`, or, given
// `bitmap` and a file starting with BAD_BAG_BITMAP_MAGIC's line, sets
// `*bitmap` to the bad-bag bitmap in it. A missing file is diagnosed and adds
// nothing.
void load_bags(std::string const& path, std::unordered_set<std::string>& bags,
               std::optional<BadBagBitmap>* bitmap = NULL) {
  std::unique_ptr<FILE, int (*)(FILE*)> const in(
      std::fopen(path.c_str(), "rb"), std::fclose);
  if (!in) {
    if (errno == ENOENT)
      std::fprintf(stderr, "pgui: skipping missing %s\n", path.c_str());
    else
      std::fprintf(stderr, "pgui: can't open %s: %s\n", path.c_str(),
                   std::strerror(errno));
    return;
  }
  std::string line;
  if (!read_line(in.get(), &line)) return;
  if (bitmap && line == BAD_BAG_BITMAP_MAGIC) {
    *bitmap = read_bad_bag_bitmap(in.get(), path);
    return;
  }
  do {
    if (!line.empty()) bags.insert(line);
  } while (read_line(in.get(), &line));
  if (std::ferror(in.get()))
    std::fprintf(stderr, "pgui: can't read %s\n", path.c_str());
}

// The path of the seed file `name` in `directory`.
std::string seed_path(std::string const& directory, std::string const& name) {
  return (std::filesystem::path(directory) / name).string();
}

// The modification time of the file at `path`, or the earliest time when it
// can't be stat'ed.
std::filesystem::file_time_type modification_time(std::string const& path) {
  std::error_code error;
  std::filesystem::file_time_type const mtime =
      std::filesystem::last_write_time(path, error);
  return error ? std::filesystem::file_time_type::min() : mtime;
}

// The seed file at `path`, read now, with version `version`.
AppState::SeedFile read_seed_file(std::string const& path, unsigned version) {
  AppState::SeedFile file;
  file.mtime = modification_time(path);
  auto lines = std::make_shared<Lines>();
  file.readable = read_file_lines(path, lines.get());
  if (!file.readable) lines->clear();
  file.lines = std::move(lines);
  file.version = version;
  return file;
}

// Loads `dictionary`'s words from its path, empty when it can't be read,
// which is diagnosed, with version `version`. Returns whether it could be.
bool read_dictionary(AppState::Dictionary* dictionary, unsigned version) {
  dictionary->mtime = modification_time(dictionary->path);
  auto words = std::make_shared<LoadedDictionary>();
  bool const ok = load_dictionary(dictionary->path, &words->words);
  if (!ok) words->words.clear();
  Lines lines(words->words.begin(), words->words.end());
  std::ranges::sort(lines);
  words->lines = std::make_shared<Lines const>(std::move(lines));
  dictionary->words = std::move(words);
  dictionary->version = version;
  return ok;
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
  std::string const small_path =
      (std::filesystem::path(root) / WORKFLOW_SMALL_DICT_PATH).string();
  AppState& state = app_state();
  state.set_letters("$S1");
  state.used_letters.clear();
  state.load_sentence_letters();
  state.dictionaries.clear();
  state.dictionaries["sml_dict"].path = small_path;
  AppState::Dictionary& big = state.dictionaries["big_dict"];
  big.path = path;
  if (!read_dictionary(&big, ++state.dictionary_version)) return false;
  std::filesystem::path const seed(seed_path);
  state.seed_directory = seed.parent_path().string();
  state.bad_bags_sentence.reset();
  state.load_seed_and_bad_bags(seed.filename().string());
  return true;
}

void AppState::load_seed(std::string name) {
  seed_name = std::move(name);
  seed_map.clear();
  seed_key = get_seed_key(seed_name);
  seed_m = get_seed_m(seed_name);
  seed_map.emplace(seed_key, seed_name);
  add_other_seed_files(seed_map, seed_directory, seed_name);
  seed_keys = sorted_keys(seed_map);
}

void AppState::refresh_seed_files(std::vector<std::string> const& keys) {
  std::unordered_set<std::string> used = {seed_name};
  for (std::string const& key : keys) {
    if (auto const file = seed_map.find(key); file != seed_map.end())
      used.insert(file->second);
  }
  std::erase_if(seed_files,
                [&](auto const& loaded) { return !used.contains(loaded.first); });
  for (auto& [file_name, file] : seed_files) {
    std::string const path = seed_path(seed_directory, file_name);
    if (modification_time(path) > file.mtime)
      file = read_seed_file(path, ++seed_file_version);
  }
}

void AppState::load_seed_and_bad_bags(std::string name) {
  load_seed(std::move(name));
  std::string const sentence(get_seed_sentence(seed_name));
  bool const valid = valid_seed_sentence(sentence);
  if (sentence.empty()) {
    std::fprintf(stderr, "missing sentence in seed filename: %s\n",
                 seed_name.c_str());
  } else if (!valid) {
    std::fprintf(stderr, "invalid sentence '%s' in seed filename %s\n",
                 sentence.c_str(), seed_name.c_str());
  }
  if (sentence == bad_bags_sentence) return;
  bad_bags_sentence = sentence;
  judged_bad.clear();
  bad_bags_bitmap.reset();
  load_bags(judged_bad_path(seed_directory), judged_bad);
  if (valid) {
    load_bags(
        (std::filesystem::path(seed_directory) / ("bad." + sentence)).string(),
        judged_bad, &bad_bags_bitmap);
  }
  ++judged_bad_version;
}

AppState::SeedFile const& AppState::seed_file(std::string const& key) {
  static SeedFile const none = {{}, std::make_shared<Lines const>(), false, 0};
  auto const file = seed_map.find(key);
  if (file == seed_map.end()) return none;
  if (auto const found = seed_files.find(file->second);
      found != seed_files.end())
    return found->second;
  return seed_files
      .emplace(file->second,
               read_seed_file(seed_path(seed_directory, file->second),
                              ++seed_file_version))
      .first->second;
}

AppState::Dictionary const& AppState::dictionary(std::string const& key) {
  auto const found = dictionaries.find(key);
  if (found == dictionaries.end()) {
    static Dictionary const empty = {
        {},
        std::make_shared<LoadedDictionary>(
            LoadedDictionary{{}, std::make_shared<Lines const>()}),
        {},
        0};
    return empty;
  }
  Dictionary& dictionary = found->second;
  if (!dictionary.words) read_dictionary(&dictionary, ++dictionary_version);
  return dictionary;
}

void AppState::refresh_dictionaries() {
  for (auto& [key, dictionary] : dictionaries) {
    if (!dictionary.words ||
        modification_time(dictionary.path) <= dictionary.mtime)
      continue;
    Dictionary reloaded = dictionary;
    if (read_dictionary(&reloaded, ++dictionary_version))
      dictionary = std::move(reloaded);
  }
}

bool AppState::is_judged_bad(std::string const& letters,
                             LetterCounts const& letter_counts) const {
  return judged_bad.contains(letters) ||
         (bad_bags_bitmap && bad_bags_bitmap->is_bad(letter_counts));
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
  std::string_view name;
  std::string added;
  int names = 0;
  for (std::string_view const piece : split(letters, ' ')) {
    if (piece.starts_with('$')) {
      name = piece.substr(1);
      ++names;
    } else if (!piece.empty()) {
      added += ' ';
      added += piece;
    }
  }
  letters_error = false;
  if (names == 0) {
    actual_letters = letters;
  } else if (names > 1) {
    std::fputs("pgui: only one letters $ENV_VAR is allowed\n", stderr);
    actual_letters.clear();
    letters_error = true;
  } else {
    std::string const variable(name);
    char const* const value = std::getenv(variable.c_str());
    if (!value || !*value) {
      std::fprintf(stderr, "pgui: letters $%s is unset\n", variable.c_str());
      actual_letters.clear();
      letters_error = true;
    } else {
      actual_letters = value + added;
    }
  }
  visual_letters = std::move(letters);
}

void AppState::load_sentence_letters() {
  sentence_letters.reset();
  char const* const root = require_workflow_root("pgui");
  if (sentence != CLASSIFIED_NO_SENTENCE && root) {
    std::filesystem::path const path =
        std::filesystem::path(root) / WORKFLOW_BEST_PATH /
        ("s" + std::to_string(sentence)) / "letters";
    std::string letters;
    if (load_letter_bag(path, "pgui", &letters))
      sentence_letters = std::move(letters);
  }
  validate_letters();
}

void AppState::validate_letters() {
  if (letters_error) {
    letters_valid = false;
  } else if (sentence == CLASSIFIED_NO_SENTENCE) {
    letters_valid = true;
  } else if (!sentence_letters) {
    letters_valid = false;
  } else {
    letters_valid = fits_letter_bag(LetterBag(*sentence_letters),
                                    bag_letters(actual_letters));
    if (!letters_valid)
      std::fprintf(stderr, "pgui: letters don't fit within S%d's letters\n",
                   sentence);
  }
}

AppState& app_state() {
  static AppState state;
  return state;
}
