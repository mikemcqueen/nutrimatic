#include "app-state.h"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "bad-bag-bitmap.h"
#include "dfs-cli-args.h"
#include "hotspots.h"
#include "input-source.h"
#include "letter-bag.h"
#include "log.h"
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

// The forced pair file of the seed file `name` in `directory`; none when
// its sN component isn't valid.
std::filesystem::path seed_forced_pairs_path(
    std::filesystem::path const& directory, std::string_view name) {
  return forced_pairs_path(directory, get_seed_sentence(name));
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
      warn("pgui", "skipping missing %s", path.c_str());
    else
      report("pgui", "can't open %s: %s", path.c_str(), std::strerror(errno));
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
    report("pgui", "can't read %s", path.c_str());
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

// Whether the file at `path` was modified after `mtime`, the modification time
// it was last read with.
bool modified_since(std::filesystem::path const& path,
                    std::filesystem::file_time_type mtime) {
  return modification_time(path) > mtime;
}

// The pair file at `path`, read now, with the forced pairs of `forced`,
// none when empty, looking each pair up in `p1`, with version `version`.
AppState::SeedPairs read_seed_pairs_file(std::filesystem::path const& path,
                                         std::filesystem::path const& forced,
                                         P1Index const* p1,
                                         unsigned version) {
  AppState::SeedPairs file;
  file.mtime = modification_time(path);
  file.forced = forced;
  file.forced_mtime = modification_time(forced);
  auto pairs = std::make_shared<std::vector<SeedPair>>();
  std::string error;
  file.readable =
      read_seed_pairs(path, p1, std::thread::hardware_concurrency(),
                      pairs.get(), &error);
  if (!file.readable) report("pgui", "%s", error.c_str());
  if (file.readable && !forced.empty() &&
      !add_forced_pairs(forced, p1, pairs.get(), &error))
    report("pgui", "%s", error.c_str());
  file.pairs = std::move(pairs);
  file.version = version;
  return file;
}

// Opens the p1 index at `path` into `*index`, even when it is out of date,
// which is diagnosed, setting `*mtime` to its modification time, and returns
// true; leaves both as they were when it can't be opened, which is
// diagnosed, and returns false.
bool open_p1_index(std::filesystem::path const& path, P1Index* index,
                   std::filesystem::file_time_type* mtime) {
  std::filesystem::file_time_type const modified = modification_time(path);
  P1Index opened;
  std::string error;
  std::string stale;
  if (!opened.open(path, &error, &stale)) {
    report("pgui", "%s", error.c_str());
    return false;
  }
  if (!stale.empty()) alert("pgui", "%s", stale.c_str());
  *index = std::move(opened);
  *mtime = modified;
  return true;
}

// Loads `dictionary`'s words from its path, empty when it can't be read,
// which is diagnosed, with version `version` and modification time `mtime`.
// Returns whether it could be.
bool read_dictionary(AppState::Dictionary* dictionary, unsigned version,
                     std::filesystem::file_time_type mtime) {
  dictionary->mtime = mtime;
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

// Reloads `dictionary` from its path, with version `++*version`, when its
// file's modification time isn't `dictionary->mtime`, leaving it empty when the
// file is missing or can't be read, which is diagnosed. Returns false when it
// reloaded and the file couldn't be read.
bool refresh_dictionary(AppState::Dictionary* dictionary, unsigned* version) {
  std::filesystem::file_time_type const mtime =
      modification_time(dictionary->path);
  if (mtime == dictionary->mtime) return true;
  return read_dictionary(dictionary, ++*version, mtime);
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
  if (!read_dictionary(&big, ++state.dictionary_version,
                       modification_time(big.path)))
    return false;
  std::filesystem::path const seed(seed_path);
  state.seed_directory = seed.parent_path().string();
  state.p1_path = seed.parent_path() / "p1.idx";
  state.p1_open = open_p1_index(state.p1_path, &state.p1, &state.p1_mtime);
  state.bad_bags_sentence.reset();
  state.load_seed_and_bad_bags(seed.filename().string());
  state.refresh_seed_lists();
  state.refresh_required_words();
  state.refresh_hotspots();
  state.start_seed_answered();
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
  bool const p1_reopened = modified_since(p1_path, p1_mtime) &&
                           open_p1_index(p1_path, &p1, &p1_mtime);
  if (p1_reopened) {
    p1_open = true;
    ++p1_version;
  }
  if (seed_pairs_file &&
      (p1_reopened || modified_since(path, seed_pairs_file->mtime) ||
       modification_time(seed_pairs_file->forced) !=
           seed_pairs_file->forced_mtime))
    seed_pairs_file = read_seed_pairs_file(
        path, seed_forced_pairs_path(seed_directory, seed_name),
        p1_open ? &p1 : nullptr, ++seed_file_version);
}

void AppState::load_seed_and_bad_bags(std::string name) {
  load_seed(std::move(name));
  std::string const sentence(get_seed_sentence(seed_name));
  bool const valid = valid_seed_sentence(sentence);
  if (sentence.empty()) {
    warn("pgui", "missing sentence in seed filename: %s", seed_name.c_str());
  } else if (!valid) {
    warn("pgui", "invalid sentence '%s' in seed filename %s",
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
        seed_forced_pairs_path(seed_directory, seed_name),
        p1_open ? &p1 : nullptr, ++seed_file_version);
  }
  return *seed_pairs_file;
}

void AppState::start_seed_answered() {
  if (!seed_answers) return;
  SeedPairs const& pairs = seed_pairs();
  std::pair<unsigned, unsigned> const key = {pairs.version,
                                             seed_answers_version};
  if (seed_answered_key == key) return;
  std::erase_if(seed_answered_jobs, [](auto const& job) {
    return job.first.wait_for(std::chrono::seconds(0)) ==
           std::future_status::ready;
  });
  if (!clues.valid()) {
    std::promise<std::shared_ptr<PairAnswers const>> opened;
    clues = opened.get_future().share();
    clues_opener = std::jthread([opened = std::move(opened)]() mutable {
      auto index = std::make_shared<PairAnswers>();
      opened.set_value(index->open("pgui", CLUEDATA_PATH)
                           ? std::move(index)
                           : nullptr);
    });
  }
  std::promise<std::shared_ptr<std::vector<char> const>> found;
  seed_answered_flags = found.get_future().share();
  seed_answered_key = key;
  std::jthread job([found = std::move(found), clues = clues,
                    pairs = pairs.pairs, match = *seed_answers]() mutable {
    std::shared_ptr<PairAnswers const> const index = clues.get();
    std::shared_ptr<std::vector<char> const> flags;
    if (index) {
      std::vector<std::pair<std::string_view, std::string_view>> words;
      words.reserve(pairs->size());
      for (SeedPair const& pair : *pairs) {
        std::string_view const line = pair.line;
        words.emplace_back(line.substr(0, pair.comma),
                           line.substr(pair.comma + 1));
      }
      flags = std::make_shared<std::vector<char> const>(index->have_answers(
          words, match, std::thread::hardware_concurrency()));
    }
    found.set_value(std::move(flags));
    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    SDL_PushEvent(&event);
  });
  seed_answered_jobs.emplace_back(seed_answered_flags, std::move(job));
}

std::shared_ptr<std::vector<char> const> AppState::seed_answered() {
  start_seed_answered();
  if (!seed_answers || seed_answered_pending()) return nullptr;
  return seed_answered_flags.get();
}

bool AppState::seed_answered_pending() const {
  return seed_answers && seed_answered_flags.valid() &&
         seed_answered_flags.wait_for(std::chrono::seconds(0)) !=
             std::future_status::ready;
}

bool AppState::seed_answered_running() const {
  auto const running = [](auto const& future) {
    return future.valid() && future.wait_for(std::chrono::seconds(0)) !=
                                 std::future_status::ready;
  };
  return running(clues) ||
         std::ranges::any_of(seed_answered_jobs, [&](auto const& job) {
           return running(job.first);
         });
}

void AppState::stop_seed_answered() {
  seed_answered_jobs.clear();
  if (clues_opener.joinable()) clues_opener.join();
}

bool AppState::clues_failed() const {
  return clues.valid() &&
         clues.wait_for(std::chrono::seconds(0)) ==
             std::future_status::ready &&
         !clues.get();
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
      view.max_length_version == seed_max_length_version &&
      view.answers_version == seed_answers_version)
    return view;
  start_seed_answered();
  if (seed_answered_pending()) return view.lines ? view : none;
  std::shared_ptr<std::vector<char> const> const answered = seed_answered();
  SeedFilter const filter = {found->min_length, seed_max_length,
                             found->min_yes, &words.words->words};
  auto lines = std::make_shared<Lines>();
  for (size_t i = 0; i < pairs.pairs->size(); ++i) {
    SeedPair const& pair = (*pairs.pairs)[i];
    if (seed_pair_passes(pair, filter) &&
        (!answered || pair.forced || (*answered)[i]))
      lines->push_back(pair.line);
  }
  if (forced_warned.emplace(pairs.version, words.version).second)
    warn_forced_words_missing("pgui", *pairs.pairs, pairs.forced,
                              words.words->words, words.path);
  view = {std::move(lines), ++seed_file_version, pairs.version, words.version,
          seed_max_length_version, seed_answers_version};
  return view;
}

void AppState::refresh_seed_lists() {
  std::string_view const sentence = get_seed_sentence(seed_name);
  if (!valid_seed_sentence(sentence)) {
    seed_lists.clear();
    return;
  }
  std::string const prefix = "seed." + std::string(sentence) + ".";
  std::map<std::string, SeedListFile> lists;
  std::error_code error;
  for (std::filesystem::directory_iterator it(seed_directory, error), end;
       !error && it != end; it.increment(error)) {
    std::string const name = it->path().filename().string();
    if (name.size() <= prefix.size() || !name.starts_with(prefix)) continue;
    std::string pattern = name.substr(prefix.size());
    auto const found = seed_lists.find(pattern);
    SeedListFile list =
        found != seed_lists.end() && found->second.path == it->path() &&
                (!found->second.lines ||
                 modification_time(it->path()) == found->second.mtime)
            ? std::move(found->second)
            : SeedListFile{it->path(), {}, nullptr, 0};
    lists.emplace(std::move(pattern), std::move(list));
  }
  seed_lists = std::move(lists);
}

AppState::SeedListFile const& AppState::seed_list(std::string const& pattern) {
  static SeedListFile const none = {{}, {}, std::make_shared<Lines const>(), 0};
  auto const found = seed_lists.find(pattern);
  if (found == seed_lists.end()) return none;
  SeedListFile& list = found->second;
  if (!list.lines) {
    list.mtime = modification_time(list.path);
    auto lines = std::make_shared<Lines>();
    if (!read_file_lines(list.path.string(), lines.get())) lines->clear();
    list.lines = std::move(lines);
    list.version = ++seed_file_version;
  }
  return list;
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
  if (!dictionary.words)
    read_dictionary(&dictionary, ++dictionary_version,
                    modification_time(dictionary.path));
  return dictionary;
}

void AppState::refresh_dictionaries() {
  for (auto& [key, dictionary] : dictionaries)
    if (dictionary.words) refresh_dictionary(&dictionary, &dictionary_version);
}

void AppState::refresh_required_words() {
  std::filesystem::path path;
  if (sentence != CLASSIFIED_NO_SENTENCE)
    path = std::filesystem::path(seed_directory) /
           ("s" + std::to_string(sentence) + ".required");
  if (path != required_words.path)
    required_words = {std::move(path), nullptr,
                      std::filesystem::file_time_type::min(), 0};
  bool const readable =
      refresh_dictionary(&required_words, &dictionary_version);
  if (!required_words.words || !required_words.words->words.empty()) return;
  if (readable)
    warn("pgui", "\"%s\" has no words", required_words.path.c_str());
  required_words.words = nullptr;
}

void AppState::refresh_hotspots() {
  std::filesystem::path path;
  if (sentence != CLASSIFIED_NO_SENTENCE)
    path = std::filesystem::path(seed_directory) /
           ("s" + std::to_string(sentence) + ".hotspots");
  if (path != hotspots.path)
    hotspots = {std::move(path), std::make_shared<Hotspots::Sections const>(),
                {}, 0};
  if (hotspots.path.empty()) return;
  bool changed = false;
  for (size_t i = 0; i < hotspots.files.size(); ++i) {
    Hotspots::File& file = hotspots.files[i];
    std::filesystem::path const file_path =
        hotspots.path.string() + "." + std::to_string(i + 1);
    std::filesystem::file_time_type const mtime =
        modification_time(file_path);
    if (mtime == file.mtime) continue;
    file = {mtime, mtime == std::filesystem::file_time_type::min()
                       ? nullptr
                       : read_hotspots_file(file_path, i + 1)};
    changed = true;
  }
  if (!changed) return;
  hotspots.version = ++dictionary_version;
  auto sections = std::make_shared<Hotspots::Sections>();
  for (Hotspots::File const& file : hotspots.files)
    if (file.sections)
      sections->insert(sections->end(), file.sections->begin(),
                       file.sections->end());
  hotspots.sections = std::move(sections);
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
  if (!file) report("pgui", "can't append to %s", path.c_str());
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
    report("pgui", "only one letters $ENV_VAR is allowed");
    actual_letters.clear();
    letters_error = true;
  } else {
    std::string const variable(name);
    char const* const value = std::getenv(variable.c_str());
    if (!value || !*value) {
      report("pgui", "letters $%s is unset", variable.c_str());
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
      report("pgui", "letters don't fit within S%d's letters", sentence);
  }
  used_valid = true;
  new_used_valid = true;
  std::string const letters = bag_letters(actual_letters);
  if (letters.empty()) return;
  LetterBag const bag(letters);
  auto const check = [&](std::string const& used, std::string_view field) {
    std::string const text = bag_letters(used);
    if (!fits_letter_bag(bag, text)) {
      report("pgui", "%.*s letters don't fit within l:",
             static_cast<int>(field.size()), field.data());
      return false;
    }
    if (text.size() == bag.size) {
      report("pgui", "%.*s uses all of l:", static_cast<int>(field.size()),
             field.data());
      return false;
    }
    return true;
  };
  used_valid = check(used_letters, "u:");
  new_used_valid = check(new_used_letters, "u_new:");
}

AppState::KeepYesPairs const& AppState::keep_yes_pairs() {
  std::array<unsigned, 3> const key = {generation, baseline_version,
                                       classified_version};
  if (keep_yes_pairs_key_ == key) return keep_yes_pairs_;
  keep_yes_pairs_key_ = key;
  keep_yes_pairs_ = {};
  std::filesystem::path const root = require_workflow_root("pgui");
  if (root.empty()) return keep_yes_pairs_;
  std::shared_ptr<DfsPairSet const> global =
      classified_pairs.get("pgui", root, CLASSIFIED_NO_SENTENCE, "yes");
  std::shared_ptr<DfsPairSet const> sentence_pairs =
      sentence == CLASSIFIED_NO_SENTENCE
          ? std::make_shared<DfsPairSet const>()
          : classified_pairs.get("pgui", root, sentence, "yes", true);
  if (global != nullptr && sentence_pairs != nullptr)
    keep_yes_pairs_ = {std::move(global), std::move(sentence_pairs)};
  return keep_yes_pairs_;
}

AppState& app_state() {
  static AppState state;
  return state;
}
