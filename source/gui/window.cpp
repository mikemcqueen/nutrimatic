#include "window.h"

#include <SDL.h>
#include <imgui.h>

#include <cstdio>
#include <utility>

#include "app-state.h"
#include "widgets.h"

namespace {

constexpr Uint32 judge_flash_ms = 200;

Uint32 end_judge_flash(Uint32, void*) {
  SDL_Event event = {};
  event.type = SDL_USEREVENT;
  SDL_PushEvent(&event);
  return 0;
}

}  // namespace

Window::Window() : sentence_(app_state().sentence) {
  AppState const& state = app_state();
  std::snprintf(seed_, sizeof seed_, "%s", state.seed_name.c_str());
  std::snprintf(letters_, sizeof letters_, "%s", state.visual_letters.c_str());
  std::snprintf(used_letters_, sizeof used_letters_, "%s",
                state.used_letters.c_str());
}

void Window::render() {
  ImGuiViewport const* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::Begin("pgui", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);

  if (ImGui::BeginTable("fields", 5)) {
    for (int i = 0; i < 5; ++i) {
      ImGui::TableSetupColumn("", i == 0 || i == 2 || i == 3
                                      ? ImGuiTableColumnFlags_WidthStretch
                                      : ImGuiTableColumnFlags_WidthFixed);
    }
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("seed:");
    ImGui::SameLine(0, 0);
    ImGui::SetNextItemWidth(-FLT_MIN);
    bool const seed_entered = ImGui::InputText(
        "##seed", seed_, sizeof seed_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    if (!app_state().seed_readable) ImGui::TextDisabled("unreadable");
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("l:");
    ImGui::SameLine(0, 0);
    ImGui::SetNextItemWidth(-FLT_MIN);
    bool entered = ImGui::InputText("##letters", letters_, sizeof letters_,
                                    ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("u:");
    ImGui::SameLine(0, 0);
    entered |= clearable_input("used", used_letters_, sizeof used_letters_);
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("s:");
    for (int i = 1; i <= 9; ++i) {
      ImGui::SameLine();
      bool const selected = sentence_ == i;
      char label[4];
      std::snprintf(label, sizeof label, "S%d", i);
      if (toggle_button(label, selected))
        sentence_ = selected ? CLASSIFIED_NO_SENTENCE : i;
    }
    ImGui::EndTable();
    if (seed_entered) app_state().load_seed_and_bad_bags(seed_);
    AppState& state = app_state();
    if (entered && (state.visual_letters != letters_ ||
                    state.used_letters != used_letters_)) {
      state.set_letters(letters_);
      state.used_letters = used_letters_;
      ++state.generation;
    }
    if (state.sentence != sentence_) {
      state.sentence = sentence_;
      ++state.generation;
    }
  }

  if (ImGui::BeginTable("columns", column_count(),
                        ImGuiTableFlags_Resizable |
                            ImGuiTableFlags_BordersInnerV,
                        ImVec2(-FLT_MIN, -FLT_MIN))) {
    ImGui::TableNextRow();
    for (Column& column : columns_) {
      ImGui::TableNextColumn();
      column.render();
    }
    ImGui::EndTable();
  }
  for (int i = 0; i < column_count(); ++i) {
    if (columns_[i].shows_list() && columns_[i].list_.judge_pressed())
      toggle_judge(i);
    if (columns_[i].shows_list() && columns_[i].list_.bad_pressed() &&
        judge_ && !columns_[*judge_].remaining_sorted_.empty())
      app_state().add_judged_bad(columns_[*judge_].remaining_sorted_);
    int const step =
        columns_[i].shows_list() ? columns_[i].list_.sideways_pressed() : 0;
    if (step == 0) continue;
    for (int j = i + step; j >= 0 && j < column_count(); j += step) {
      if (columns_[j].shows_list()) {
        columns_[j].list_.focus();
        break;
      }
    }
  }
  ImGui::End();
}

void Window::toggle_judge(ColumnIdentifier id) {
  if (!judge_) {
    judge_ = id;
  } else if (*judge_ == id) {
    judge_.reset();
  } else {
    judge_flash_until_ = ImGui::GetTime() + judge_flash_ms / 1000.0;
    SDL_AddTimer(judge_flash_ms, end_judge_flash, nullptr);
  }
}

bool Window::judge_flashing() const {
  return ImGui::GetTime() < judge_flash_until_;
}

ColumnIdentifier Window::add_column(Column column) {
  column.id_ = column_count();
  columns_.push_back(std::move(column));
  return static_cast<ColumnIdentifier>(columns_.size() - 1);
}

Window& main_window() {
  static Window window;
  return window;
}
