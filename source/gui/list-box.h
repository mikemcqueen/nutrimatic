#ifndef NUTRIMATIC_GUI_LIST_BOX_H
#define NUTRIMATIC_GUI_LIST_BOX_H

#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "input-source.h"

// A single-selection list filling the remaining space of its container, where
// clicking the selected item deselects it. Holds its items, and a value for
// each shown right-aligned on its row, by sharing, never copying them, and
// drawing only those scrolled into view. A filter hides the items it doesn't match without changing
// the items or the selection. While the list has keyboard focus, Left and
// Right don't move ImGui navigation; sideways() reports them instead.
class ListBox {
 public:
  ListBox(SharedLines items, SharedLines values);

  void render();

  SharedLines const& items() const { return items_; }

  // Replaces the items and their values and clears the selection. `values` is
  // either empty or holds one entry, possibly empty, per item.
  void set_items(SharedLines items, SharedLines values);

  // Shows only the items `pattern` (ECMAScript) matches part of; empty shows
  // every item. Returns false, and shows every item, when `pattern` is
  // invalid.
  bool set_filter(std::string const& pattern);

  // How many items the filter shows.
  size_t shown_count() const;

  // Index into the items, or -1 when nothing is selected.
  int selected() const { return selected_; }

  // -1 when Left, or 1 when Right, was pressed while the list had keyboard
  // focus during the last render(), else 0.
  int sideways() const { return sideways_; }

  // Moves keyboard focus, at the next render(), to the selected item when it
  // is shown, else to the top item in view, without changing the selection.
  void focus() { focus_requested_ = true; }

 private:
  void apply_filter();
  // The row showing items_[item], or -1 when `item` is -1 or filtered out.
  int shown_row(int item) const;

  SharedLines items_;
  SharedLines values_;
  std::optional<std::regex> filter_;
  std::vector<int> shown_;
  int selected_ = -1;
  int sideways_ = 0;
  bool focus_requested_ = false;
};

#endif
