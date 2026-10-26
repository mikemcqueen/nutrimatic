#ifndef NUTRIMATIC_GUI_APP_STATE_H
#define NUTRIMATIC_GUI_APP_STATE_H

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bad-bag-bitmap.h"
#include "classified.h"
#include "dfs-class-list.h"
#include "input-source.h"
#include "letter-bag.h"
#include "p1-index.h"
#include "seed-view.h"

// A word list's words, as a set and as sorted lines.
struct LoadedDictionary {
  DfsDictionary words;
  SharedLines lines;
};
using SharedDictionary = std::shared_ptr<LoadedDictionary const>;

// A seed view's band: its source key, the phase 1 YES its pairs need, none
// keeping every pair, and its per-word min length.
struct SeedBand {
  char const* key;
  std::optional<float> min_yes;
  int min_length;
};

// A seed view's band key, one of AppState::seed_bands, and dictionary key,
// "sml_dict" or "big_dict".
using SeedViewKey = std::pair<std::string, std::string>;

// pgui's global state, loaded once at startup by load_app_state(), and the
// options every command runs with. visual_letters is the letters as typed and
// actual_letters what commands use; set_letters() sets both. sentence is 1
// through 9, or CLASSIFIED_NO_SENTENCE for none. generation goes up whenever
// letters, used_letters, or sentence change. seed_name is a word,word pair
// file in seed_directory, pairs.sN, whose seeds are views of it (see
// seed_view()).
struct AppState {
  // Sets visual_letters to `letters`, and actual_letters to `letters` itself
  // unless one of its space-separated pieces is $NAME. Then it is the value of
  // the environment variable NAME followed by the other pieces, each after a
  // space. Sets letters_error, with the error diagnosed, when NAME is unset or
  // empty or there is more than one $NAME piece, which also leaves
  // actual_letters empty.
  void set_letters(std::string letters);

  // Sets sentence_letters to the bag_letters() of sentence's
  // $WFROOT/.wf/best/sN/letters file, none when sentence is
  // CLASSIFIED_NO_SENTENCE or that file can't be read, with the error
  // diagnosed, then validate_letters().
  void load_sentence_letters();

  // Sets letters_valid to whether actual_letters fit within sentence_letters,
  // diagnosing letters that don't. Without a sentence they always do; with a
  // sentence whose letters weren't loaded, or after letters_error, they never
  // do.
  void validate_letters();

  // Sets seed_name to `name`, dropping the pair file read for another name.
  // Reads no pair file; see seed_pairs() and refresh_seed_files().
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

  // The pair file's pairs, each with its phase 1 YES from p1, none when it
  // can't be read, which is diagnosed, with its modification time when read,
  // the earliest time when it can't be stat'ed, and a version unique to this
  // load.
  struct SeedPairs {
    std::filesystem::file_time_type mtime;
    std::shared_ptr<std::vector<SeedPair> const> pairs;
    bool readable = false;
    unsigned version = 0;
  };

  // The lines of the pairs passing a view's filter, with a version unique
  // to this build, and the pair file, dictionary and max length versions it
  // was built from.
  struct SeedView {
    SharedLines lines;
    unsigned version = 0;
    unsigned pairs_version = 0;
    unsigned dictionary_version = 0;
    unsigned max_length_version = 0;
  };

  // Drops from seed_views those none of `keys` name, reopens p1 when p1_path
  // is newer than when it was opened, keeping it as it was when it can't be
  // reopened, which is diagnosed, and reads the pair file again when it's
  // newer than when it was read or p1 was reopened.
  void refresh_seed_files(std::vector<SeedViewKey> const& keys);

  // seed_name's pairs, read the first time they're asked for.
  SeedPairs const& seed_pairs();

  // The view under `band` and `dictionary`: seed_pairs() passing a
  // SeedFilter of the band's min length, seed_max_length, the band's YES,
  // and the dictionary under `dictionary` in dictionaries. Built the first
  // time it's asked for, and again, under a new seed_file_version, when the
  // pair file, that dictionary or seed_max_length have changed since. No
  // lines and version 0 when no band in seed_bands has the key `band`.
  SeedView const& seed_view(std::string const& band,
                            std::string const& dictionary);

  // A word list file, and, once loaded, its words, its modification time
  // when read, the earliest time when it can't be stat'ed, and a version
  // unique to that load.
  struct Dictionary {
    std::filesystem::path path;
    SharedDictionary words;
    std::filesystem::file_time_type mtime;
    unsigned version = 0;
  };

  // The dictionary under `key` in dictionaries, loaded from its path if it
  // isn't yet; empty, with version 0, when `key` isn't in dictionaries, and
  // empty when its file can't be read, which is diagnosed. Only called on
  // the main thread.
  Dictionary const& dictionary(std::string const& key);

  // Reloads the loaded dictionaries whose files' modification times changed
  // since they were loaded, leaving a dictionary empty when its file is
  // missing or can't be read, which is diagnosed.
  void refresh_dictionaries();

  // Points required_words at sentence's sN.required in seed_directory, with
  // no words and version 0 when that path changed, and reloads it as
  // refresh_dictionaries() does. Its words are null while sentence is
  // CLASSIFIED_NO_SENTENCE or that file has been missing since the path
  // changed, which isn't diagnosed, and null when the file can't be read or
  // has no words, which is diagnosed.
  void refresh_required_words();

  // Adds `letters`, lowercase a-z and sorted, to judged_bad when it isn't
  // there already, bumps judged_bad_version, and appends it as a line to the
  // judged-bad file in seed_directory. A failed append is diagnosed.
  void add_judged_bad(std::string letters);

  // Whether `letters`, lowercase a-z and sorted, are in judged_bad or are
  // bad by bad_bags_bitmap; `letter_counts` are their counts.
  bool is_judged_bad(std::string const& letters,
                     LetterCounts const& letter_counts) const;

  // Dictionaries by key, set only by load_app_state(): "big_dict", $WFROOT's
  // filtered dictionary, loaded there, and "sml_dict", $WFROOT's filtered
  // small dictionary, loaded by dictionary().
  std::unordered_map<std::string, Dictionary> dictionaries;
  // The last Dictionary version given out.
  unsigned dictionary_version = 0;
  std::string visual_letters;
  std::string actual_letters;
  std::string used_letters;
  int sentence = 1;
  std::optional<std::string> sentence_letters;
  // Whether set_letters() failed; see set_letters().
  bool letters_error = false;
  // Whether commands may run; see validate_letters().
  bool letters_valid = true;
  std::string seed_directory;
  std::string seed_name;
  // The bands of seed_view(), in source dropdown order.
  static constexpr std::array<SeedBand, 5> seed_bands = {{
      {"all", std::nullopt, SeedFilter{}.min_len},
      {"seed50m3", 0.5f, 3},
      {"seed50m4", 0.5f, 4},
      {"seed85m3", 0.85f, 3},
      {"seed85m4", 0.85f, 4},
  }};
  // The band in seed_bands with the key `key`; null when there is none.
  static SeedBand const* seed_band(std::string_view key);
  // The first column's source, and what a seed column falls back to.
  static constexpr char const* seed_key = "seed85m4";
  // seed_directory's p1.idx, opened from p1_path by load_app_state() and
  // reopened by refresh_seed_files(), with its modification time when
  // opened, the earliest time before; every pair has no verdict while it
  // hasn't been.
  std::filesystem::path p1_path;
  P1Index p1;
  std::filesystem::file_time_type p1_mtime =
      std::filesystem::file_time_type::min();
  bool p1_open = false;
  // Each view's per-word max length, inclusive, and a version that goes up
  // when it changes.
  int seed_max_length = SeedFilter{}.max_len;
  unsigned seed_max_length_version = 0;
  // seed_name's pairs, once read by seed_pairs().
  std::optional<SeedPairs> seed_pairs_file;
  // The views built by seed_view().
  std::map<SeedViewKey, SeedView> seed_views;
  // The last SeedPairs or SeedView version given out.
  unsigned seed_file_version = 0;
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
  // Goes up whenever a Review of pairs records verdicts.
  unsigned classified_version = 0;
  // The required words pfilter runs share, set by refresh_required_words().
  Dictionary required_words;
};

// What a command runs with, taken when it starts so it runs unaffected by
// later changes: app_state()'s actual letters and sentence, its Column's used
// letters, the dictionary its Column's D: dropdown names and that
// dictionary's lines, and the lines of the dictionary or the output of the
// column its wb: dropdown names, empty when it has none; the "big_dict"
// dictionary, whatever D: names; app_state()'s required words; and the
// token a command may check to quit early, its output then discarded.
struct LetterToolsParams {
  std::string letters;
  std::string used_letters;
  int sentence = CLASSIFIED_NO_SENTENCE;
  SharedDictionary dictionary;
  SharedLines dictionary_lines;
  SharedLines word_bag_lines;
  SharedDictionary big_dictionary;
  SharedDictionary required_words;
  std::stop_token stop;
};

// Loads $WFROOT's filtered dictionary into app_state() as "big_dict", beside
// "sml_dict", not loaded yet, with letters "$S1", empty used letters,
// sentence 1's letters (see AppState::load_sentence_letters()) and required
// words (see AppState::refresh_required_words()), the pair
// file at `seed_path`, the p1.idx beside it, and judged_bad and
// bad_bags_bitmap from the judged-bad and bad.sN files beside it (see
// AppState::load_seed_and_bad_bags()). Returns false, with the error
// diagnosed, when the dictionary can't be read; a pair file that can't be
// read is left empty, and a p1.idx that can't be opened, which is
// diagnosed, leaves every pair without a verdict.
bool load_app_state(std::string const& seed_path);

AppState& app_state();

#endif
