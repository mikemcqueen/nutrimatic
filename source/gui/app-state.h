#ifndef NUTRIMATIC_GUI_APP_STATE_H
#define NUTRIMATIC_GUI_APP_STATE_H

#include <string>

#include "classified.h"
#include "dfs-class-list.h"

// pgui's global state, loaded once at startup by load_app_state(), and the
// options every command runs with. visual_letters is the letters as typed and
// actual_letters what commands use; set_letters() sets both. sentence is 1
// through 9, or CLASSIFIED_NO_SENTENCE for none. generation goes up whenever
// letters, used_letters, or sentence change.
struct AppState {
  // Sets visual_letters to `letters`, and actual_letters to the value of the
  // environment variable it names when it is $NAME, empty when that variable
  // is unset, or to `letters` itself otherwise.
  void set_letters(std::string letters);

  DfsDictionary dictionary;
  std::string visual_letters;
  std::string actual_letters;
  std::string used_letters;
  int sentence = 9;
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
// used letters. Returns false, with the error diagnosed, when it can't be read.
bool load_app_state();

AppState& app_state();

#endif
