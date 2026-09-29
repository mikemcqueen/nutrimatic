#include "widgets.h"

#include <imgui.h>

bool clearable_input(char const* id, char* text, size_t size) {
  ImGui::PushID(id);
  float const side = ImGui::GetFrameHeight();
  float const spacing = ImGui::GetStyle().ItemInnerSpacing.x;
  ImGui::SetNextItemWidth(-(side + spacing));
  bool changed = ImGui::InputText("##text", text, size,
                                  ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine(0, spacing);
  if (ImGui::Button("X", ImVec2(side, side))) {
    text[0] = '\0';
    changed = true;
  }
  ImGui::PopID();
  return changed;
}
