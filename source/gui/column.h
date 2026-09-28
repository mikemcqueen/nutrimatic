#ifndef NUTRIMATIC_GUI_COLUMN_H
#define NUTRIMATIC_GUI_COLUMN_H

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "input-source.h"
#include "list-box.h"

// One top-level pane: a text field, a dropdown of `choices` naming the
// command (see make_command()), a status line, and a ListBox of that
// command's output on its input source, omitted when the source is
// std::monostate. render() reruns the command, on a worker thread, when the
// choice, app_state(), or the source column's output or selection has changed
// since it last ran; it waits for a pending source column to finish first.
// The letters of the source column's selected item are added to the used
// letters the command runs with. Widget IDs
// are scoped to the Column, so any number can be rendered side by side in a
// Window.
class Column {
 public:
  Column(InputSource source, std::vector<std::string> choices);

  void render();

  InputSource const& input_source() const { return source_; }

  // The command's output, shared rather than copied by readers: empty before
  // the first run completes, when the choice names no command, and when the
  // command fails.
  SharedLines const& output() const { return list_.items(); }

  // The selected item of output(), or empty when nothing is selected.
  std::string selected_item() const;

  // Goes up each time output() is replaced or the selection changes.
  unsigned version() const { return version_; }

  // Whether output() is out of date: a run is in flight, or one is due or
  // waiting on the source column.
  bool pending() const;

 private:
  // What an output is made from.
  struct Key {
    int choice;
    unsigned generation;
    unsigned source_version;
    bool operator==(Key const&) const = default;
  };

  // A run on the worker thread; done is set once ok and output are.
  struct Job {
    Key key;
    std::atomic<bool> done = false;
    bool ok = false;
    SharedLines output;
  };

  // The source column, or null when the source isn't one.
  Column const* source_column() const;
  Key wanted_key() const;
  void start(Key key);
  void finish();
  void publish(Key key, bool ok, SharedLines output);

  InputSource source_;
  char text_[256] = {};
  std::vector<std::string> choices_;
  int choice_ = 0;
  Key made_key_ = {-1, 0, 0};
  unsigned version_ = 0;
  bool failed_ = false;
  std::shared_ptr<Job> job_;
  std::jthread worker_;
  ListBox list_;
};

#endif
