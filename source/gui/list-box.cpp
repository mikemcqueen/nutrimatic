#include "list-box.h"

#include <imgui.h>

#include <utility>

ListBox::ListBox(std::vector<std::string> items) : items_(std::move(items)) {}

void ListBox::render() {
  if (!ImGui::BeginListBox("##list", ImVec2(-FLT_MIN, -FLT_MIN))) return;
  for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
    if (ImGui::Selectable(items_[i].c_str(), selected_ == i)) selected_ = i;
  }
  ImGui::EndListBox();
}
