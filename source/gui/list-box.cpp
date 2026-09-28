#include "list-box.h"

#include <imgui.h>

#include <utility>

ListBox::ListBox(SharedLines items) : items_(std::move(items)) {}

void ListBox::render() {
  if (!ImGui::BeginListBox("##list", ImVec2(-FLT_MIN, -FLT_MIN))) return;
  Lines const& items = *items_;
  for (int i = 0; i < static_cast<int>(items.size()); ++i) {
    if (ImGui::Selectable(items[i].c_str(), selected_ == i))
      selected_ = selected_ == i ? -1 : i;
  }
  ImGui::EndListBox();
}

void ListBox::set_items(SharedLines items) {
  items_ = std::move(items);
  selected_ = -1;
}
