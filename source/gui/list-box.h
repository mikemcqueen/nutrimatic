#ifndef NUTRIMATIC_GUI_LIST_BOX_H
#define NUTRIMATIC_GUI_LIST_BOX_H

#include "input-source.h"

// A single-selection list filling the remaining space of its container, where
// clicking the selected item deselects it. Holds its items by sharing, never
// copying them.
class ListBox {
 public:
  explicit ListBox(SharedLines items);

  void render();

  SharedLines const& items() const { return items_; }

  // Replaces the items and clears the selection.
  void set_items(SharedLines items);

  // Index into the items, or -1 when nothing is selected.
  int selected() const { return selected_; }

 private:
  SharedLines items_;
  int selected_ = -1;
};

#endif
