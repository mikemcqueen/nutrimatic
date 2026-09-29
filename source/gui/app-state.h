#ifndef NUTRIMATIC_GUI_APP_STATE_H
#define NUTRIMATIC_GUI_APP_STATE_H

#include <string>
#include <unordered_map>
#include <vector>

#include "classified.h"
#include "dfs-class-list.h"
#include "input-source.h"

// Seed file names in AppState::seed_directory, by key: "seed50", "seed85",
// "all", or the loaded seed's AppState::seed_key.
using SeedMap = std::unordered_map<std::string, std::string>;

// pgui's global state, loaded once at startup by load_app_state(), and the
// options every command runs with. visual_letters is the letters as typed and
// actual_letters what commands use; set_letters() sets both. sentence is 1
// through 9, or CLASSIFIED_NO_SENTENCE for none. generation goes up whenever
// letters, used_letters, sentence, or seed change. seed_name is a word or
// word,word pair file in seed_directory. seed_map holds seed_name under
// seed_key, plus the other seed files in seed_directory named like it (see
// load_seed()).
struct AppState {
  // Sets visual_letters to `letters`, and actual_letters to the value of the
  // environment variable it names when it is $NAME, empty when that variable
  // is unset, or to `letters` itself otherwise.
  void set_letters(std::string letters);

  // Sets seed_name to `name`, loads that file's lines, or none when it can't
  // be read, and sets seed_readable to whether it could; sets seed_key to
  // name's key, "seed" and its cutoff for seed.S.M.all.CUTOFF[...].pairs,
  // "all" for all.S.M, or "seed" otherwise; sets seed_map to seed_name under
  // seed_key plus whichever of seed.S.M.all.50.pairs (seed50),
  // seed.S.M.all.85.15.pairs (seed85), and all.S.M (all) exist in
  // seed_directory under keys not already taken; and bumps generation. An
  // unreadable seed is diagnosed.
  void load_seed(std::string name);

  // The lines of the seed file under `key` in seed_map, read the first time
  // they're asked for since load_seed(); none when `key` isn't in seed_map
  // or the file can't be read, which is diagnosed.
  SharedLines seed_lines(std::string const& key);

  // The words of the dictionary under `key` in dictionaries, loaded from its
  // path if it isn't yet, sorted, made the first time they're asked for; none
  // when `key` isn't in dictionaries or its file can't be read, which is
  // diagnosed.
  SharedLines dictionary_lines(std::string const& key);

  // A word list file, and its words once loaded.
  struct Dictionary {
    std::string path;
    bool loaded = false;
    DfsDictionary words;
  };

  // Dictionaries by key, set only by load_app_state(): "big_dict", $WFROOT's
  // filtered dictionary, loaded there, and "sml_dict", /usr/share/dict/words,
  // loaded by dictionary_lines(). pfilter uses "big_dict".
  std::unordered_map<std::string, Dictionary> dictionaries;
  // dictionaries' keys, sorted.
  std::vector<std::string> dictionary_keys;
  // The lines made by dictionary_lines(), by key.
  std::unordered_map<std::string, SharedLines> dictionary_words;
  std::string visual_letters;
  std::string actual_letters;
  std::string used_letters;
  int sentence = 9;
  std::string seed_directory;
  std::string seed_name;
  bool seed_readable = false;
  std::string seed_key;
  SeedMap seed_map;
  // seed_map's keys, sorted.
  std::vector<std::string> seed_keys;
  // The lines read by seed_lines(), by key.
  std::unordered_map<std::string, SharedLines> seeds;
  unsigned generation = 0;
};

// A copy of app_state()'s actual letters, used letters, and sentence, taken
// when a command starts so it runs unaffected by later changes.
struct GlobalSettings {
  std::string letters;
  std::string used_letters;
  int sentence = CLASSIFIED_NO_SENTENCE;
};

// Loads $WFROOT's filtered dictionary into app_state() as "big_dict", beside
// "sml_dict", not loaded yet, with empty letters and used letters, and the
// seed at `seed_path` (see AppState::load_seed()). Returns false, with the
// error diagnosed, when the dictionary can't be read; a seed that can't be
// read is left empty.
bool load_app_state(std::string const& seed_path);

AppState& app_state();

#endif
