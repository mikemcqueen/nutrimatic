#include "list-box.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <utility>

ListBox::ListBox(SharedLines items, SharedLines values)
    : items_(std::move(items)), values_(std::move(values)) {}

void ListBox::render() {
  judge_pressed_ = false;
  float const brightness = focused_ ? 1.45f : 1.25f;
  ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
  bg.x *= brightness;
  bg.y *= brightness;
  bg.z *= brightness;
  ImGui::PushStyleColor(ImGuiCol_FrameBg, bg);
  bool const open = ImGui::BeginListBox("##list", ImVec2(-FLT_MIN, -FLT_MIN));
  ImGui::PopStyleColor();
  focused_ = open && ImGui::IsWindowFocused();
  if (!open) return;
  sideways_pressed_ = 0;
  if (ImGui::Shortcut(ImGuiKey_LeftArrow, ImGuiInputFlags_Repeat))
    sideways_pressed_ = -1;
  if (ImGui::Shortcut(ImGuiKey_RightArrow, ImGuiInputFlags_Repeat))
    sideways_pressed_ = 1;
  judge_pressed_ = ImGui::Shortcut(ImGuiKey_J);
  Lines const& items = *items_;
  Lines const& values = *values_;
  float const right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
  int const count = static_cast<int>(shown_count());
  int const focus_row = focus_requested_ ? shown_row(selected_) : -1;
  bool focus_top = focus_requested_ && focus_row < 0;
  focus_requested_ = false;
  float const top = ImGui::GetCurrentWindow()->InnerRect.Min.y - 1;
  auto const row = [&](int i) {
    ImGui::PushID(i);
    if (ImGui::Selectable(items[i].c_str(), selected_ == i,
                          ImGuiSelectableFlags_SelectOnNav)) {
      bool const navigated = GImGui->NavJustMovedToId == ImGui::GetItemID();
      selected_ = selected_ == i && !navigated ? -1 : i;
    }
    ImGui::PopID();
  };
  auto const value = [&](int i) {
    if (values.empty() || values[i].empty()) return;
    ImGui::SameLine(right - ImGui::CalcTextSize(values[i].c_str()).x);
    ImGui::TextDisabled("%s", values[i].c_str());
  };
  ImGuiListClipper clipper;
  clipper.Begin(count);
  if (focus_row >= 0) clipper.IncludeItemByIndex(focus_row);
  while (clipper.Step()) {
    for (int n = clipper.DisplayStart; n < clipper.DisplayEnd; ++n) {
      int const i = filter_ ? shown_[n] : n;
      row(i);
      if (n == focus_row ||
          (focus_top && ImGui::GetItemRectMin().y >= top)) {
        ImGui::FocusItem();
        ImGui::SetNavCursorVisibleAfterMove();
        focus_top = false;
      }
      value(i);
    }
  }
  if (focus_top) ImGui::SetWindowFocus();
  ImGui::EndListBox();
}

void ListBox::set_items(SharedLines items, SharedLines values) {
  items_ = std::move(items);
  values_ = std::move(values);
  selected_ = -1;
  apply_filter();
}

bool ListBox::set_filter(std::string const& pattern) {
  filter_.reset();
  bool ok = true;
  if (!pattern.empty()) {
    try {
      filter_.emplace(pattern, std::regex::ECMAScript | std::regex::optimize);
    } catch (std::regex_error const&) {
      ok = false;
    }
  }
  apply_filter();
  return ok;
}

int ListBox::shown_row(int item) const {
  if (item < 0 || !filter_) return item;
  auto const it = std::lower_bound(shown_.begin(), shown_.end(), item);
  return it != shown_.end() && *it == item ? static_cast<int>(it - shown_.begin())
                                           : -1;
}

size_t ListBox::shown_count() const {
  return filter_ ? shown_.size() : items_->size();
}

void ListBox::apply_filter() {
  shown_.clear();
  if (!filter_) return;
  Lines const& items = *items_;
  for (int i = 0; i < static_cast<int>(items.size()); ++i) {
    if (std::regex_search(items[i], *filter_)) shown_.push_back(i);
  }
}
