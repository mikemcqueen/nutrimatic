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
#include <thread>
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

// The modification time of the file at `path`, or the earliest time when it
// can't be stat'ed.
std::filesystem::file_time_type modification_time(
    std::filesystem::path const& path) {
  std::error_code error;
  std::filesystem::file_time_type const mtime =
      std::filesystem::last_write_time(path, error);
  return error ? std::filesystem::file_time_type::min() : mtime;
}

// The pair file at `path`, read now, looking each pair up in `p1`, with
// version `version`.
AppState::SeedPairs read_seed_pairs_file(std::filesystem::path const& path,
                                         P1Index const* p1,
                                         unsigned version) {
  AppState::SeedPairs file;
  file.mtime = modification_time(path);
  auto pairs = std::make_shared<std::vector<SeedPair>>();
  std::string error;
  file.readable =
      read_seed_pairs(path, p1, std::thread::hardware_concurrency(),
                      pairs.get(), &error);
  if (!file.readable) std::fprintf(stderr, "pgui: %s\n", error.c_str());
  file.pairs = std::move(pairs);
  file.version = version;
  return file;
}

// Opens the p1 index at `path` into `*index`, setting `*mtime` to its
// modification time, and returns true; leaves both as they were when it
// can't be opened, which is diagnosed, and returns false.
bool open_p1_index(std::filesystem::path const& path, P1Index* index,
                   std::filesystem::file_time_type* mtime) {
  std::filesystem::file_time_type const modified = modification_time(path);
  P1Index opened;
  std::string error;
  if (!opened.open(path, &error)) {
    std::fprintf(stderr, "pgui: %s\n", error.c_str());
    return false;
  }
  *index = std::move(opened);
  *mtime = modified;
  return true;
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

}  // namespace

bool load_app_state(std::string const& seed_path) {
  std::filesystem::path const root = require_workflow_root("pgui");
  if (root.empty()) return false;
  AppState& state = app_state();
  state.set_letters("$S1");
  state.used_letters.clear();
  state.load_sentence_letters();
  state.dictionaries.clear();
  state.dictionaries["sml_dict"].path = root / WORKFLOW_SMALL_DICT_PATH;
  AppState::Dictionary& big = state.dictionaries["big_dict"];
  big.path = root / WORKFLOW_DICT_PATH;
  if (!read_dictionary(&big, ++state.dictionary_version)) return false;
  std::filesystem::path const seed(seed_path);
  state.seed_directory = seed.parent_path().string();
  state.p1_path = seed.parent_path() / "p1.idx";
  state.p1_open = open_p1_index(state.p1_path, &state.p1, &state.p1_mtime);
  state.bad_bags_sentence.reset();
  state.load_seed_and_bad_bags(seed.filename().string());
  return true;
}

void AppState::load_seed(std::string name) {
  if (name != seed_name) seed_pairs_file.reset();
  seed_name = std::move(name);
}

void AppState::refresh_seed_files(std::vector<SeedViewKey> const& keys) {
  std::erase_if(seed_views, [&](auto const& view) {
    return std::ranges::find(keys, view.first) == keys.end();
  });
  std::filesystem::path const path =
      std::filesystem::path(seed_directory) / seed_name;
  bool const p1_reopened = modification_time(p1_path) > p1_mtime &&
                           open_p1_index(p1_path, &p1, &p1_mtime);
  if (p1_reopened) p1_open = true;
  if (seed_pairs_file &&
      (p1_reopened || modification_time(path) > seed_pairs_file->mtime))
    seed_pairs_file = read_seed_pairs_file(path, p1_open ? &p1 : nullptr,
                                           ++seed_file_version);
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

AppState::SeedPairs const& AppState::seed_pairs() {
  if (!seed_pairs_file) {
    seed_pairs_file = read_seed_pairs_file(
        std::filesystem::path(seed_directory) / seed_name,
        p1_open ? &p1 : nullptr, ++seed_file_version);
  }
  return *seed_pairs_file;
}

AppState::SeedView const& AppState::seed_view(std::string const& band,
                                              std::string const& dictionary) {
  static SeedView const none = {std::make_shared<Lines const>()};
  SeedBand const* const found = seed_band(band);
  if (!found) return none;
  SeedPairs const& pairs = seed_pairs();
  Dictionary const& words = this->dictionary(dictionary);
  SeedView& view = seed_views[{band, dictionary}];
  if (view.lines && view.pairs_version == pairs.version &&
      view.dictionary_version == words.version &&
      view.max_length_version == seed_max_length_version)
    return view;
  SeedFilter const filter = {found->min_length, seed_max_length,
                             found->min_yes, &words.words->words};
  auto lines = std::make_shared<Lines>();
  for (SeedPair const& pair : *pairs.pairs)
    if (seed_pair_passes(pair, filter)) lines->push_back(pair.line);
  view = {std::move(lines), ++seed_file_version, pairs.version, words.version,
          seed_max_length_version};
  return view;
}

SeedBand const* AppState::seed_band(std::string_view key) {
  auto const found = std::ranges::find(seed_bands, key, &SeedBand::key);
  return found == seed_bands.end() ? nullptr : &*found;
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
  std::filesystem::path const root = require_workflow_root("pgui");
  if (sentence != CLASSIFIED_NO_SENTENCE && !root.empty()) {
    std::filesystem::path const path =
        root / WORKFLOW_BEST_PATH /
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
