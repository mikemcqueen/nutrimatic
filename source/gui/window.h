#ifndef NUTRIMATIC_GUI_WINDOW_H
#define NUTRIMATIC_GUI_WINDOW_H

#include <vector>

#include "column.h"
#include "input-source.h"

// The top-level pane filling the viewport: a line of seed, letters (l), and
// used-letters (u) fields splitting its width evenly, followed by sentence
// buttons S1-S9 (s), then the columns side by side. The seed field starts
// from app_state()'s seed_name, and Enter in it reloads the seed from the
// name typed. The letters fields start from app_state(), and Enter in either
// stores both there. The buttons start from app_state()'s sentence; clicking
// one selects it alone, or deselects it when already selected, and stores the
// result there. Left and Right, while a column's ListBox has keyboard focus,
// move it to the ListBox of the nearest column that shows one on that side
// (see ListBox::focus()).
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

 private:
  std::vector<Column> columns_;
  char seed_[256] = {};
  char letters_[256] = {};
  char used_letters_[256] = {};
  int sentence_;
};

// pgui's one Window, constructed on first use, which must follow
// load_app_state().
Window& main_window();

#endif
