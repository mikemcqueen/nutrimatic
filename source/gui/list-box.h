#ifndef NUTRIMATIC_GUI_LIST_BOX_H
#define NUTRIMATIC_GUI_LIST_BOX_H

#include <imgui.h>

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
// Right don't move ImGui navigation; sideways_pressed() reports them
// instead, and review_pressed() reports Shift+R. Items can
// also be hidden by set_hidden(), apart from the filter. Its items are drawn in
// light gray on a muted blue background, both lighter while it has keyboard
// focus.
class ListBox {
 public:
  ListBox(SharedLines items, SharedLines values);

  void render();

  SharedLines const& items() const { return items_; }

  SharedLines const& values() const { return values_; }

  // Replaces the items and their values. The selected item stays selected
  // when it is among the new items, at its first occurrence; otherwise the
  // selection is cleared. `values` is either empty or holds one entry,
  // possibly empty, per item.
  void set_items(SharedLines items, SharedLines values);

  // Shows only the items `pattern` (ECMAScript) matches part of; empty shows
  // every item. Returns false, and shows every item, when `pattern` is
  // invalid.
  bool set_filter(std::string const& pattern);

  // Hides the items whose entry in `hidden` is nonzero, alongside the
  // filter, without changing the selection. `hidden` is either empty, hiding
  // none, or holds one entry per item. set_items() clears it.
  void set_hidden(std::vector<char> hidden);

  // Whether the selected item is hidden by set_hidden().
  bool selected_hidden() const;

  // When selected_hidden(), selects the next shown item, else the last one
  // shown, else none, and moves keyboard focus to it (see focus()) when the
  // list has keyboard focus.
  void step_off_hidden();

  // When selected_hidden(), clears the selection.
  void deselect_hidden();

  // How many items the filter and set_hidden() show.
  size_t shown_count() const;

  // The indexes of the items the filter and set_hidden() show, in order.
  std::vector<int> shown_items() const;

  // Index into the items, or -1 when nothing is selected.
  int selected() const { return selected_; }

  // -1 when Left, or 1 when Right, was pressed while the list had keyboard
  // focus during the last render(), else 0.
  int sideways_pressed() const { return sideways_pressed_; }

  // Whether Shift+R was pressed while the list had keyboard focus during the
  // last render().
  bool review_pressed() const { return review_pressed_; }

  // The screen rect of the list's frame during the last render().
  ImVec2 rect_min() const { return rect_min_; }
  ImVec2 rect_max() const { return rect_max_; }

  // Moves keyboard focus, at the next render(), to the selected item when it
  // is shown, else to the top item in view, else to the list itself, without
  // changing the selection.
  void focus() { focus_requested_ = true; }

 private:
  // Whether shown_ lists the items shown, rather than every item being shown.
  bool filtering() const { return filter_ || !hidden_.empty(); }
  void apply_filter();
  // The row showing items_[item], or -1 when `item` is -1 or not shown.
  int shown_row(int item) const;

  SharedLines items_;
  SharedLines values_;
  std::optional<std::regex> filter_;
  // Empty when set_hidden() hides no item.
  std::vector<char> hidden_;
  std::vector<int> shown_;
  int selected_ = -1;
  int sideways_pressed_ = 0;
  bool review_pressed_ = false;
  ImVec2 rect_min_;
  ImVec2 rect_max_;
  // Whether the list had keyboard focus during the last render().
  bool focused_ = false;
  bool focus_requested_ = false;
};

#endif
