#ifndef NUTRIMATIC_GUI_WINDOW_H
#define NUTRIMATIC_GUI_WINDOW_H

#include <memory>
#include <optional>
#include <vector>

#include "column.h"
#include "input-source.h"
#include "review.h"

// The top-level pane filling the viewport: a line of seed, letters (l), and
// used-letters (u) fields splitting its width evenly, followed by a BAD
// checkbox that toggles app_state()'s judged_bad_filter and sentence buttons
// S1-S9 (s), then the columns side by side. The seed field starts
// from app_state()'s seed_name, and Enter in it loads the seed from the
// name typed, with its bad bags (see AppState::load_seed_and_bad_bags()),
// points each column whose source is a seed key no longer in app_state()'s
// seed_map at its seed_key, then keeps and refreshes only the seed files the
// columns' seed keys and the seed name name (see
// AppState::refresh_seed_files()), and reloads the loaded dictionaries whose
// files are newer (see AppState::refresh_dictionaries()).
// The letters fields start from app_state(), and Enter in either stores both
// there. The buttons start from app_state()'s sentence; clicking
// one selects it alone, or deselects it when already selected, and stores the
// result there. Left and Right, while a column's ListBox has keyboard focus,
// move it to the ListBox of the nearest column that shows one on that side
// (see ListBox::focus()).
// Shift+R, while a column's ListBox has keyboard focus, opens a Review of the
// items it shows, when its command is reviewable (see Command::reviewable()),
// its output is up to date, and a sentence is selected, and otherwise says
// why on stderr; once the Review records verdicts, app_state()'s
// classified_version goes up. Keyboard focus returns to the column's ListBox
// when the Review closes. Tapping Alt doesn't move keyboard focus, so the
// Alt release that follows Alt-Tab back into the window leaves it in place.
// A request to close pgui goes through the open Review, if any (see
// Review::request_close()).
class Window {
 public:
  Window();

  void render();

  // Appends `column` and returns its identifier. References from
  // get_column() are invalidated.
  ColumnIdentifier add_column(Column column);

  // The number of columns; identifiers run from 0 to one less.
  int column_count() const { return static_cast<int>(columns_.size()); }

  // The column with identifier `id`, which must be less than column_count().
  Column& get_column(ColumnIdentifier id) { return columns_[id]; }

  // The judge column, of which there is at most one.
  std::optional<ColumnIdentifier> judge() const { return judge_; }

  // Makes column `id` the judge when there is none, or none when it is the
  // judge. When another column is the judge, leaves it so and flashes it
  // instead (see judge_flashing()).
  void toggle_judge(ColumnIdentifier id);

  // Whether the judge is flashing, which it does for a moment after a
  // refused toggle_judge().
  bool judge_flashing() const;

  // Asks to close pgui; closed() says once it may.
  void request_close();
  bool closed() const { return closed_; }

  // Whether a column or the Review is still running something.
  bool running() const;

  // Stops the columns' runs, waits for them and any SUBMIT, and closes the
  // Review.
  void stop();

 private:
  // Opens a Review of column `id`; see the class comment.
  void open_review(ColumnIdentifier id);

  std::vector<Column> columns_;
  char seed_[256] = {};
  char letters_[256] = {};
  char used_letters_[256] = {};
  int sentence_;
  std::optional<ColumnIdentifier> judge_;
  // When the judge stops flashing, in ImGui::GetTime() seconds.
  double judge_flash_until_ = 0;
  // The open Review, if any, and the column it reviews.
  std::unique_ptr<Review> review_;
  ColumnIdentifier reviewed_ = 0;
  bool closed_ = false;
};

// pgui's one Window, constructed on first use, which must follow
// load_app_state().
Window& main_window();

#endif
