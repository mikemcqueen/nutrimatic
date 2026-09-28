#ifndef NUTRIMATIC_GUI_COLUMN_H
#define NUTRIMATIC_GUI_COLUMN_H

#include <string>
#include <vector>

#include "list-box.h"
#include "renderable.h"

// One top-level pane: a text box and a dropdown above a ListBox. Widget IDs
// are scoped to the Column, so any number can be rendered side by side.
class Column : public Renderable {
 public:
  Column(std::vector<std::string> choices, std::vector<std::string> items);

  void render() override;

 private:
  char text_[256] = {};
  std::vector<std::string> choices_;
  int choice_ = 0;
  ListBox list_;
};

#endif
