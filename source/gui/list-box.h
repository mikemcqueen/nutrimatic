#ifndef NUTRIMATIC_GUI_LIST_BOX_H
#define NUTRIMATIC_GUI_LIST_BOX_H

#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "input-source.h"

// A single-selection list filling the remaining space of its container, where
// clicking the selected item deselects it. Holds its items by sharing, never
// copying them, and drawing only those scrolled into view. A filter hides the items it doesn't match without changing
// the items or the selection.
class ListBox {
 public:
  explicit ListBox(SharedLines items);

  void render();

  SharedLines const& items() const { return items_; }

  // Replaces the items and clears the selection.
  void set_items(SharedLines items);

  // Shows only the items `pattern` (ECMAScript) matches part of; empty shows
  // every item. Returns false, and shows every item, when `pattern` is
  // invalid.
  bool set_filter(std::string const& pattern);

  // How many items the filter shows.
  size_t shown_count() const;

  // Index into the items, or -1 when nothing is selected.
  int selected() const { return selected_; }

 private:
  void apply_filter();

  SharedLines items_;
  std::optional<std::regex> filter_;
  std::vector<int> shown_;
  int selected_ = -1;
};

#endif
