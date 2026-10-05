#include "widgets.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>

bool clearable_input(char const* id, char* text, size_t size,
                     float reserve) {
  ImGui::PushID(id);
  float const side = ImGui::GetFrameHeight();
  float const spacing = ImGui::GetStyle().ItemInnerSpacing.x;
  ImGui::SetNextItemWidth(-(side + spacing + reserve));
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

bool toggle_button(char const* label, bool on) {
  if (on) {
    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32_WHITE);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
  }
  float const side = ImGui::GetFrameHeight();
  ImGui::PushID(label);
  bool const clicked = ImGui::Button("##toggle", ImVec2(side, side));
  ImGui::PopID();
  ImFontBaked* const font = ImGui::GetFontBaked();
  float left = FLT_MAX, right = -FLT_MAX, top = FLT_MAX, bottom = -FLT_MAX;
  float x = 0;
  for (char const* c = label; *c; ++c) {
    ImFontGlyph const* const glyph = font->FindGlyph(static_cast<ImWchar>(*c));
    left = std::min(left, x + glyph->X0);
    right = std::max(right, x + glyph->X1);
    top = std::min(top, glyph->Y0);
    bottom = std::max(bottom, glyph->Y1);
    x += glyph->AdvanceX;
  }
  ImVec2 const min = ImGui::GetItemRectMin();
  ImVec2 const pos(std::floor(min.x + (side - left - right) / 2),
                   std::floor(min.y + (side - top - bottom) / 2));
  ImGui::GetWindowDrawList()->AddText(pos, ImGui::GetColorU32(ImGuiCol_Text),
                                      label);
  if (on) {
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
  }
  return clicked;
}
