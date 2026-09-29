#ifndef NUTRIMATIC_GUI_APP_STATE_H
#define NUTRIMATIC_GUI_APP_STATE_H

#include <string>

#include "classified.h"
#include "dfs-class-list.h"
#include "input-source.h"

// pgui's global state, loaded once at startup by load_app_state(), and the
// options every command runs with. visual_letters is the letters as typed and
// actual_letters what commands use; set_letters() sets both. sentence is 1
// through 9, or CLASSIFIED_NO_SENTENCE for none. generation goes up whenever
// letters, used_letters, sentence, or seed change. seed is the lines of the
// file seed_name in seed_directory, a word or word,word pair file.
struct AppState {
  // Sets visual_letters to `letters`, and actual_letters to the value of the
  // environment variable it names when it is $NAME, empty when that variable
  // is unset, or to `letters` itself otherwise.
  void set_letters(std::string letters);

  // Sets seed_name to `name`, seed to that file's lines, or to none when it
  // can't be read, and seed_readable to whether it could, and bumps
  // generation. An unreadable seed is diagnosed.
  void load_seed(std::string name);

  DfsDictionary dictionary;
  std::string visual_letters;
  std::string actual_letters;
  std::string used_letters;
  int sentence = 9;
  std::string seed_directory;
  std::string seed_name;
  SharedLines seed = std::make_shared<Lines const>();
  bool seed_readable = false;
  unsigned generation = 0;
};

// A copy of app_state()'s actual letters, used letters, and sentence, taken
// when a command starts so it runs unaffected by later changes.
struct GlobalSettings {
  std::string letters;
  std::string used_letters;
  int sentence = CLASSIFIED_NO_SENTENCE;
};

// Loads $WFROOT's filtered dictionary into app_state(), with empty letters and
// used letters, and the seed at `seed_path` (see AppState::load_seed()).
// Returns false, with the error diagnosed, when the dictionary can't be read;
// a seed that can't be read is left empty.
bool load_app_state(std::string const& seed_path);

AppState& app_state();

#endif
