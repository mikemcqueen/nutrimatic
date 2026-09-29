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

// One top-level pane: a line of a source dropdown, offering "seed" and each
// column's number counting from 1, and a filter field; then a dropdown of
// command_names naming the command (see make_command()), sized to the longest
// name, beside that command's option widgets, a status line, and a ListBox of
// that command's output on its input source, omitted when the source is
// std::monostate. Each choice keeps its own options. render() reruns the
// command, on a worker thread, when the source, the choice, its options,
// app_state(), or the source column's output or selection has changed since
// it last ran; it waits for a pending source column to finish first.
// The letters of the source column's selected item, and the used letters of
// its command and every command upstream of it (see
// Command::used_letters()), are added to the used letters the command runs
// with. A status line shows the letters left once those used letters are
// removed from app_state()'s letters. Pressing Enter in the filter field
// applies it as a regex to the ListBox (see ListBox::set_filter()), and the X
// button beside it clears and applies it; the filter changes only what is
// shown, never output(), selected_item(), or version(). Widget IDs are scoped
// to the Column, so any number can be rendered side by side in a Window.
// Columns that read from this one, directly or through others, including this
// one itself, are disabled in the source dropdown, so sources never form a
// cycle.
class Column {
 public:
  explicit Column(InputSource source);

  void render();

  InputSource const& input_source() const { return source_; }

  // Whether `column` is this column or one upstream of it.
  bool reads_from(Column const* column) const;

  // The command's output, shared rather than copied by readers: empty before
  // the first run completes, when the choice names no command, and when the
  // command fails.
  SharedLines const& output() const { return list_.items(); }

  // The selected item of output(), or empty when nothing is selected.
  std::string selected_item() const;

  // The used letters of the commands that made output(): this column's and
  // every one upstream of it.
  std::string const& chain_used_letters() const { return chain_used_letters_; }

  // Goes up each time output() is replaced or the selection changes.
  unsigned version() const { return version_; }

  // Whether output() is out of date: a run is in flight, or one is due or
  // waiting on the source column.
  bool pending() const;

 private:
  // What an output is made from.
  struct Key {
    int choice;
    unsigned options_version;
    unsigned generation;
    unsigned source_version;
    InputSource source;
    bool operator==(Key const&) const = default;
  };

  // A run on the worker thread; done is set once ok and output are.
  struct Job {
    Key key;
    std::atomic<bool> done = false;
    bool ok = false;
    SharedLines output;
    std::string chain_used_letters;
  };

  // "src:" and the source dropdown, which sets source_.
  void render_source();
  // The source column, or null when the source isn't one.
  Column const* source_column() const;
  Key wanted_key() const;
  // The used letters of this column's command and every one upstream.
  std::string chain_letters() const;
  // app_state()'s used letters, chain_letters(), and the letters of the
  // source column's selected item: those the command runs with.
  std::string used_letters() const;
  void start(Key key);
  void finish();
  void publish(Key key, bool ok, SharedLines output,
               std::string chain_used_letters);

  InputSource source_;
  char filter_[256] = {};
  bool bad_filter_ = false;
  std::vector<std::optional<Command>> commands_;
  int choice_ = 0;
  unsigned options_version_ = 0;
  Key made_key_ = {-1, 0, 0, 0, {}};
  // The remaining letters status line, as of remaining_key_.
  std::string remaining_;
  Key remaining_key_ = {-1, 0, 0, 0, {}};
  std::string chain_used_letters_;
  unsigned version_ = 0;
  bool failed_ = false;
  std::shared_ptr<Job> job_;
  std::jthread worker_;
  ListBox list_;
};

#endif
