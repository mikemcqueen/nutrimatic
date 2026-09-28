#include "column.h"

#include <imgui.h>

#include <utility>

Column::Column(std::vector<std::string> choices, std::vector<std::string> items)
    : choices_(std::move(choices)), list_(std::move(items)) {}

void Column::render() {
  ImGui::PushID(this);
  ImGui::SetNextItemWidth(-FLT_MIN);
  ImGui::InputText("##text", text_, sizeof text_);

  ImGui::SetNextItemWidth(-FLT_MIN);
  char const* preview = choices_.empty() ? "" : choices_[choice_].c_str();
  if (ImGui::BeginCombo("##choice", preview)) {
    for (int i = 0; i < static_cast<int>(choices_.size()); ++i) {
      if (ImGui::Selectable(choices_[i].c_str(), choice_ == i)) choice_ = i;
    }
    ImGui::EndCombo();
  }

  list_.render();
  ImGui::PopID();
}
