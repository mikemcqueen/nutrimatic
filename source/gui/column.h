#ifndef NUTRIMATIC_GUI_COLUMN_H
#define NUTRIMATIC_GUI_COLUMN_H

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "command.h"
#include "input-source.h"
#include "list-box.h"

// Loads the font Column's lc tooltips use, Cascadia Mono, after the default
// font. The tooltips use the current font when it can't be loaded, which is
// diagnosed.
void load_letter_count_font();

// One top-level pane: a line of a source dropdown, offering app_state()'s
// seed keys, "dict" when the command reads dictionaries (see
// Command::reads_dictionaries()), and the number, counting from 1, of each
// column before this one, and a letter
// source dropdown, offering "all" and those same numbers; a line of an "f:"
// filter field and a "D:" dropdown of DICT ("sml_dict") and BIG ("big_dict"),
// starting at DICT, naming the dictionary in app_state()'s dictionaries the
// command runs with, and that "dict" names; then a
// dropdown of
// command_names naming the command (see make_command()), sized to the longest
// name, beside that command's option widgets, a status line, and a ListBox of
// that command's output on its input source, omitted when the source is
// std::monostate. Each choice keeps its own options. render() reruns the
// command, on a worker thread, when the source, the choice, its options, the
// dictionary, app_state(), or the source or letters column's output or selection has
// changed since it last ran; it waits for a pending source or letters column
// to finish first. The letters column is the one letter_source names, none
// for "all". The command runs with the used letters passed on by the letters
// column (see output_used_letters()), or app_state()'s (the window's u:
// field) for "all", plus its own (see Command::used_letters()). A status line
// shows the letters left once those used letters are removed from
// app_state()'s letters (the window's l: field); while the mouse is held on
// it, a tooltip below it shows lc's report on them (see
// letter_count_lines()). Items that would leave letters judged bad (see
// AppState::is_judged_bad()), were they taken from the letters left, are,
// while app_state()'s judged_bad_filter is on, hidden from the ListBox (see
// ListBox::set_hidden()), once output() is up to date. A selected item
// hidden this way is deselected
// when it was kept across a rerun, or stepped off (see
// ListBox::step_off_hidden()) when judged_bad_version went up. Pressing Enter
// in the filter field applies it as a regex to the ListBox (see
// ListBox::set_filter()), and the X button beside it clears and applies it;
// the filter changes only what is shown, never output(), selected_item(), or
// version(). Widget IDs are scoped to the Column, so any number can be
// rendered side by side in a Window. Both dropdowns offer only columns before
// this one, so sources never form a cycle.
class Column {
 public:
  Column(InputSource source, LetterSource letter_source);

  void render();

  InputSource const& input_source() const { return source_; }

  // The command's output, shared rather than copied by readers: empty before
  // the first run completes, when the choice names no command, and when the
  // command fails. When any line the command printed has a space, each is cut
  // at its first space, and the rest, trimmed, becomes its value, shown
  // beside it in the ListBox but left out of output().
  SharedLines const& output() const { return list_.items(); }

  // The selected item of output(), or empty when nothing is selected.
  std::string selected_item() const;

  // The used letters passed on to the columns reading this one: those the
  // command runs with, plus the letters of selected_item().
  std::string output_used_letters() const;

  // Goes up each time output() is replaced or the selection changes.
  unsigned version() const { return version_; }

  // Whether output() is out of date: a run is in flight, or one is due or
  // waiting on the source or letters column.
  bool pending() const;

 private:
  // What an output is made from.
  struct Key {
    int choice;
    unsigned options_version;
    unsigned generation;
    unsigned source_version;
    InputSource source;
    unsigned letters_version;
    LetterSource letter_source;
    int dictionary;
    bool operator==(Key const&) const = default;
  };

  // A run on the worker thread; done is set once ok, output, and values are.
  struct Job {
    Key key;
    std::atomic<bool> done = false;
    bool ok = false;
    SharedLines output;
    SharedLines values;
  };

  // "src:" and the source dropdown, which sets source_, then "ltrs:" and
  // the letter source dropdown, which sets letter_source_.
  void render_sources();
  // letter_count_ in a tooltip whose top left is at `x`, `y`.
  void render_letter_count(float x, float y) const;
  // Whether render() shows the ListBox: the source isn't std::monostate.
  bool shows_list() const {
    return !std::holds_alternative<std::monostate>(source_);
  }
  // The source column, or null when the source isn't one.
  Column const* source_column() const;
  // The letters column, or null when there isn't one.
  Column const* letters_column() const;
  Key wanted_key() const;
  // The used letters the command runs with: the letters column's
  // output_used_letters(), or app_state()'s when there is none, plus the
  // command's own.
  std::string used_letters() const;
  // Hides the ListBox items leaving letters judged bad, and deselects or
  // steps off a hidden selection; see the class comment.
  void update_hidden();
  void start(Key key);
  void finish();
  void publish(Key key, bool ok, SharedLines output, SharedLines values);

  friend class Window;

  // This column's identifier in main_window(), set by Window::add_column().
  ColumnIdentifier id_ = 0;
  InputSource source_;
  LetterSource letter_source_;
  char filter_[256] = {};
  // Index of the D: dropdown's choice in dictionary_choices.
  int dictionary_ = 0;
  bool bad_filter_ = false;
  std::vector<std::optional<Command>> commands_;
  int choice_ = 0;
  unsigned options_version_ = 0;
  Key made_key_ = {-1, 0, 0, 0, {}, 0, {}, 0};
  // The remaining letters status line, as of remaining_key_.
  std::string remaining_;
  Key remaining_key_ = {-1, 0, 0, 0, {}, 0, {}, 0};
  // The consonant/vowel ratio of the remaining letters, as of remaining_key_.
  double remaining_cv_ = 0;
  // The remaining a-z letters, lowercase and sorted, as of remaining_key_, or
  // empty when some used letters weren't in app_state()'s letters.
  std::string remaining_sorted_;
  // lc's output, as of the last press on the remaining letters status line.
  std::vector<std::string> letter_count_;
  // The made_key_ and app_state()'s judged_bad_version as of the last
  // update_hidden().
  Key hidden_key_ = {-1, 0, 0, 0, {}, 0, {}, 0};
  unsigned hidden_judged_version_ = 0;
  unsigned version_ = 0;
  bool failed_ = false;
  std::shared_ptr<Job> job_;
  std::jthread worker_;
  ListBox list_;
};

#endif
