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
// from app_state()'s seed_name, and Enter in it reloads the seed from the
// name typed, with its bad bags (see AppState::load_seed_and_bad_bags()).
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
// when the Review closes.
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
};

// pgui's one Window, constructed on first use, which must follow
// load_app_state().
Window& main_window();

#endif
