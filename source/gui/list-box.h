#ifndef NUTRIMATIC_GUI_LIST_BOX_H
#define NUTRIMATIC_GUI_LIST_BOX_H

#include <string>
#include <vector>

#include "renderable.h"

// A single-selection list filling the remaining space of its container.
class ListBox : public Renderable {
 public:
  explicit ListBox(std::vector<std::string> items);

  void render() override;

  // Index into the items, or -1 when nothing is selected.
  int selected() const { return selected_; }

 private:
  std::vector<std::string> items_;
  int selected_ = -1;
};

#endif
