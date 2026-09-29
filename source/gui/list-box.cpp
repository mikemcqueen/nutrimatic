#include "list-box.h"

#include <imgui.h>

#include <utility>

ListBox::ListBox(SharedLines items) : items_(std::move(items)) {}

void ListBox::render() {
  if (!ImGui::BeginListBox("##list", ImVec2(-FLT_MIN, -FLT_MIN))) return;
  Lines const& items = *items_;
  auto const row = [&](int i) {
    ImGui::PushID(i);
    if (ImGui::Selectable(items[i].c_str(), selected_ == i))
      selected_ = selected_ == i ? -1 : i;
    ImGui::PopID();
  };
  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(shown_count()));
  while (clipper.Step()) {
    for (int n = clipper.DisplayStart; n < clipper.DisplayEnd; ++n)
      row(filter_ ? shown_[n] : n);
  }
  ImGui::EndListBox();
}

void ListBox::set_items(SharedLines items) {
  items_ = std::move(items);
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
