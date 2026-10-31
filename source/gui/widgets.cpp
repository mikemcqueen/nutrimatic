#include "widgets.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <string_view>

bool clearable_input(std::string_view id, char* text, size_t size,
                     float reserve) {
  ImGui::PushID(id.data(), id.data() + id.size());
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

bool toggle_button(std::string_view label, bool on) {
  if (on) {
    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32_WHITE);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
  }
  float const side = ImGui::GetFrameHeight();
  ImGui::PushID(label.data(), label.data() + label.size());
  bool const clicked = ImGui::Button("##toggle", ImVec2(side, side));
  ImGui::PopID();
  ImFontBaked* const font = ImGui::GetFontBaked();
  float left = FLT_MAX, right = -FLT_MAX, top = FLT_MAX, bottom = -FLT_MAX;
  float x = 0;
  for (char const c : label) {
    ImFontGlyph const* const glyph = font->FindGlyph(static_cast<ImWchar>(c));
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
                                      label.data(),
                                      label.data() + label.size());
  if (on) {
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
  }
  return clicked;
}

bool refresh_button(std::string_view id) {
  float const side = ImGui::GetFrameHeight();
  ImGui::PushID(id.data(), id.data() + id.size());
  bool const clicked = ImGui::Button("##refresh", ImVec2(side, side));
  ImGui::PopID();
  ImVec2 const min = ImGui::GetItemRectMin();
  ImVec2 const center(min.x + side / 2, min.y + side / 2);
  float const radius = side * 0.28f;
  float const start = -std::numbers::pi_v<float> / 2 + 0.35f;
  float const end = start + 2 * std::numbers::pi_v<float> - 0.9f;
  ImU32 const color = ImGui::GetColorU32(ImGuiCol_Text);
  ImDrawList* const draw = ImGui::GetWindowDrawList();
  draw->PathArcTo(center, radius, start, end);
  draw->PathStroke(color, 0, std::max(1.0f, side / 12));
  ImVec2 const normal(std::cos(end), std::sin(end));
  ImVec2 const tangent(-normal.y, normal.x);
  ImVec2 const tip(center.x + normal.x * radius + tangent.x * side * 0.18f,
                   center.y + normal.y * radius + tangent.y * side * 0.18f);
  float const half = side * 0.16f;
  draw->AddTriangleFilled(
      tip,
      ImVec2(center.x + normal.x * (radius + half),
             center.y + normal.y * (radius + half)),
      ImVec2(center.x + normal.x * (radius - half),
             center.y + normal.y * (radius - half)),
      color);
  return clicked;
}

void cv_range_input(char* min_text, char* max_text, size_t size,
                    std::string_view default_min,
                    std::string_view default_max, bool enabled, bool* entered) {
  ImGui::PushID("cv_range");
  float const pad = ImGui::GetStyle().FramePadding.x;
  float const width = ImGui::CalcTextSize("0.00").x + 2 * pad;
  auto const field = [&](std::string_view label, char const* id, char* text) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label.data(), label.data() + label.size());
    ImGui::SameLine(0, 0);
    ImGui::SetNextItemWidth(width);
    *entered |= ImGui::InputText(id, text, size,
                                 ImGuiInputTextFlags_EnterReturnsTrue);
  };
  ImGui::BeginDisabled(!enabled);
  field("m", "##min", min_text);
  ImGui::SameLine(0, pad);
  field("x", "##max", max_text);
  ImGui::SameLine(0, pad);
  float const side = ImGui::GetFrameHeight();
  if (ImGui::Button("X", ImVec2(side, side))) {
    std::snprintf(min_text, size, "%.*s", static_cast<int>(default_min.size()),
                  default_min.data());
    std::snprintf(max_text, size, "%.*s", static_cast<int>(default_max.size()),
                  default_max.data());
    *entered = true;
  }
  ImGui::EndDisabled();
  ImGui::PopID();
}
