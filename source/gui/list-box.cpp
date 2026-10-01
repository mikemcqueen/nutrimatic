#include "list-box.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <utility>

ListBox::ListBox(SharedLines items, SharedLines values)
    : items_(std::move(items)), values_(std::move(values)) {}

void ListBox::render() {
  judge_pressed_ = false;
  bad_pressed_ = false;
  ImGui::PushStyleColor(ImGuiCol_FrameBg, focused_ ? IM_COL32(0x31, 0x3b, 0x4a, 0xff)
                                                   : IM_COL32(0x2a, 0x33, 0x40, 0xff));
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
  bad_pressed_ = ImGui::Shortcut(ImGuiKey_D);
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
    ImGui::PushStyleColor(ImGuiCol_Text, focused_ ? IM_COL32(0xe4, 0xe4, 0xe4, 0xff)
                                                  : IM_COL32(0xd4, 0xd4, 0xd4, 0xff));
    bool const clicked = ImGui::Selectable(items[i].c_str(), selected_ == i,
                                           ImGuiSelectableFlags_SelectOnNav);
    ImGui::PopStyleColor();
    if (clicked) {
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
      int const i = filtering() ? shown_[n] : n;
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
  std::optional<std::string> const selected =
      selected_ < 0 ? std::nullopt : std::optional((*items_)[selected_]);
  items_ = std::move(items);
  values_ = std::move(values);
  selected_ = -1;
  hidden_.clear();
  if (selected) {
    auto const found = std::ranges::find(*items_, *selected);
    if (found != items_->end())
      selected_ = static_cast<int>(found - items_->begin());
  }
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

void ListBox::set_hidden(std::vector<char> hidden) {
  hidden_ = std::move(hidden);
  if (std::ranges::none_of(hidden_, [](char h) { return h; })) hidden_.clear();
  apply_filter();
}

bool ListBox::selected_hidden() const {
  return selected_ >= 0 && !hidden_.empty() && hidden_[selected_];
}

void ListBox::step_off_hidden() {
  if (!selected_hidden()) return;
  auto const next = std::ranges::lower_bound(shown_, selected_);
  if (next != shown_.end())
    selected_ = *next;
  else
    selected_ = shown_.empty() ? -1 : shown_.back();
  if (focused_) focus_requested_ = true;
}

void ListBox::deselect_hidden() {
  if (selected_hidden()) selected_ = -1;
}

int ListBox::shown_row(int item) const {
  if (item < 0 || !filtering()) return item;
  auto const it = std::lower_bound(shown_.begin(), shown_.end(), item);
  return it != shown_.end() && *it == item ? static_cast<int>(it - shown_.begin())
                                           : -1;
}

size_t ListBox::shown_count() const {
  return filtering() ? shown_.size() : items_->size();
}

void ListBox::apply_filter() {
  shown_.clear();
  if (!filtering()) return;
  Lines const& items = *items_;
  for (int i = 0; i < static_cast<int>(items.size()); ++i) {
    if (!hidden_.empty() && hidden_[i]) continue;
    if (!filter_ || std::regex_search(items[i], *filter_)) shown_.push_back(i);
  }
}
