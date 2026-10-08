#ifndef NUTRIMATIC_GUI_APP_STATE_H
#define NUTRIMATIC_GUI_APP_STATE_H

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bad-bag-bitmap.h"
#include "classified.h"
#include "dfs-class-list.h"
#include "input-source.h"
#include "letter-bag.h"

// A word list's words, as a set and as sorted lines.
struct LoadedDictionary {
  DfsDictionary words;
  SharedLines lines;
};
using SharedDictionary = std::shared_ptr<LoadedDictionary const>;

// Seed file names in AppState::seed_directory, by key: "seed50", "seed85",
// or "all", then "m3" or "m4", or the loaded seed's AppState::seed_key.
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
  // name's key, "seed", its cutoff, and its M for
  // seed.S.M.all.CUTOFF[...].pairs, "all" and its M for all.S.M, or "seed"
  // otherwise; sets seed_m to name's M; sets seed_map to seed_name under
  // seed_key plus, for each M of m3 and m4, whichever of
  // seed.S.M.all.50.pairs (seed50M), seed.S.M.all.85.15.pairs (seed85M),
  // and all.S.M (allM) exist in seed_directory under keys not already
  // taken, S being name's; and bumps generation. An unreadable seed is
  // diagnosed.
  void load_seed(std::string name);

  // load_seed()s `name`, diagnosing a name whose sN component,
  // <prefix>.sN.<suffix>, is missing or isn't s and one digit. When that
  // component differs from bad_bags_sentence, sets bad_bags_sentence to it,
  // empties judged_bad and bad_bags_bitmap, loads judged_bad from the
  // judged-bad file in seed_directory and, for a valid component, loads the
  // bad.sN file there too, into bad_bags_bitmap when it is a bad-bag bitmap
  // (see bad-bag-bitmap.h) or else into judged_bad, and bumps
  // judged_bad_version. A missing file is diagnosed and adds nothing.
  void load_seed_and_bad_bags(std::string name);

  // The lines of the seed file under `key` in seed_map, read the first time
  // they're asked for since load_seed(); none when `key` isn't in seed_map
  // or the file can't be read, which is diagnosed.
  SharedLines seed_lines(std::string const& key);

  // The dictionary under `key` in dictionaries, loaded from its path if it
  // isn't yet; empty when `key` isn't in dictionaries or its file can't be
  // read, which is diagnosed. Only called on the main thread.
  SharedDictionary dictionary(std::string const& key);

  // Adds `letters`, lowercase a-z and sorted, to judged_bad when it isn't
  // there already, bumps judged_bad_version, and appends it as a line to the
  // judged-bad file in seed_directory. A failed append is diagnosed.
  void add_judged_bad(std::string letters);

  // Whether `letters`, lowercase a-z and sorted, are in judged_bad or are
  // bad by bad_bags_bitmap; `letter_counts` are their counts.
  bool is_judged_bad(std::string const& letters,
                     LetterCounts const& letter_counts) const;

  // A word list file, and its words once loaded.
  struct Dictionary {
    std::string path;
    SharedDictionary words;
  };

  // Dictionaries by key, set only by load_app_state(): "big_dict", $WFROOT's
  // filtered dictionary, loaded there, and "sml_dict", $WFROOT's filtered
  // small dictionary, loaded by dictionary().
  std::unordered_map<std::string, Dictionary> dictionaries;
  std::string visual_letters;
  std::string actual_letters;
  std::string used_letters;
  int sentence = 1;
  std::string seed_directory;
  std::string seed_name;
  bool seed_readable = false;
  std::string seed_key;
  // The mN component of seed_name, <prefix>.sN.mN.<suffix>; empty when it
  // has none.
  std::string seed_m;
  SeedMap seed_map;
  // seed_map's keys, sorted.
  std::vector<std::string> seed_keys;
  // The lines read by seed_lines(), by key.
  std::unordered_map<std::string, SharedLines> seeds;
  unsigned generation = 0;
  // Remaining letters judged bad, each lowercase a-z and sorted, whatever
  // letters, used letters, seed, or sentence they were judged under; loaded
  // by load_seed_and_bad_bags() from the judged-bad and text bad.sN files
  // in seed_directory.
  std::unordered_set<std::string> judged_bad;
  // The bad.sN file loaded by load_seed_and_bad_bags(), when it is a bad-bag
  // bitmap.
  std::optional<BadBagBitmap> bad_bags_bitmap;
  // The sN component of the seed whose bad.sN judged_bad or bad_bags_bitmap
  // holds, set by load_seed_and_bad_bags(); none before the first.
  std::optional<std::string> bad_bags_sentence;
  // Whether columns hide items leaving letters judged bad (see
  // is_judged_bad()).
  bool judged_bad_filter = true;
  // Goes up whenever judged_bad, bad_bags_bitmap, or judged_bad_filter
  // changes.
  unsigned judged_bad_version = 0;
  // The classified pairs pfilter runs share, loaded from $WFROOT as they're
  // first asked for and again when their files change.
  ClassifiedPairCache classified_pairs;
};

// What a command runs with, taken when it starts so it runs unaffected by
// later changes: app_state()'s actual letters and sentence, its Column's used
// letters, and the dictionary its Column's D: dropdown names.
struct LetterToolsParams {
  std::string letters;
  std::string used_letters;
  int sentence = CLASSIFIED_NO_SENTENCE;
  SharedDictionary dictionary;
};

// Loads $WFROOT's filtered dictionary into app_state() as "big_dict", beside
// "sml_dict", not loaded yet, with letters "$S1", empty used letters, the
// seed at `seed_path`, and judged_bad and bad_bags_bitmap from the
// judged-bad and bad.sN files beside it (see
// AppState::load_seed_and_bad_bags()). Returns false, with the error
// diagnosed, when the dictionary can't be read; a seed that can't be read is
// left empty.
bool load_app_state(std::string const& seed_path);

AppState& app_state();

#endif
