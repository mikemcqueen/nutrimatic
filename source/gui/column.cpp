#include "column.h"

#include <SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <optional>
#include <span>
#include <utility>

#include "app-state.h"
#include "widgets.h"
#include "window.h"

namespace {

SharedLines const& empty_lines() {
  static SharedLines const lines = std::make_shared<Lines const>();
  return lines;
}

// A dropdown's width: the longest of `labels`, framed, and its arrow.
float combo_width(std::span<char const* const> labels) {
  float longest = 0;
  for (char const* const label : labels)
    longest = std::max(longest, ImGui::CalcTextSize(label).x);
  return longest + 2 * ImGui::GetStyle().FramePadding.x +
         ImGui::GetFrameHeight();
}

// "(N): letters" for the N letters of `letters` left once `used` is removed,
// in the order typed, then "  over: letters" for those of `used` that
// weren't there to remove. Characters other than letters and digits are
// skipped.
std::string remaining_letters(std::string const& letters,
                              std::string const& used) {
  int counts[UCHAR_MAX + 1] = {};
  for (char const c : used) {
    if (std::isalnum(static_cast<unsigned char>(c)))
      ++counts[static_cast<unsigned char>(c)];
  }
  std::string left;
  for (char const c : letters) {
    if (!std::isalnum(static_cast<unsigned char>(c))) continue;
    int& count = counts[static_cast<unsigned char>(c)];
    if (count > 0)
      --count;
    else
      left += c;
  }
  std::string over;
  for (char const c : used) {
    int& count = counts[static_cast<unsigned char>(c)];
    if (count > 0) {
      over += c;
      --count;
    }
  }
  std::string text = "(" + std::to_string(left.size()) + "): " + left;
  if (!over.empty()) text += "  over: " + over;
  return text;
}

// The source dropdown's label for `source`: "seed", or a column's number
// counting from 1.
std::string source_label(InputSource const& source) {
  if (auto const* id = std::get_if<ColumnIdentifier>(&source))
    return std::to_string(*id + 1);
  if (std::holds_alternative<Seed>(source)) return "seed";
  return "none";
}

void wake_main_loop() {
  SDL_Event event = {};
  event.type = SDL_USEREVENT;
  SDL_PushEvent(&event);
}

}  // namespace

Column::Column(InputSource source)
    : source_(std::move(source)), list_(empty_lines()) {
  for (char const* const name : command_names)
    commands_.push_back(make_command(name));
}

void Column::render() {
  ImGui::PushID(this);
  render_source();
  ImGui::SameLine();
  ImGui::TextUnformatted("filter:");
  ImGui::SameLine();
  if (clearable_input("filter", filter_, sizeof filter_))
    bad_filter_ = !list_.set_filter(filter_);

  if (ImGui::BeginTable("##command", 2)) {
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,
                            combo_width(command_names));
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##choice", command_names[choice_])) {
      for (int i = 0; i < static_cast<int>(command_names.size()); ++i) {
        if (ImGui::Selectable(command_names[i], choice_ == i)) choice_ = i;
      }
      ImGui::EndCombo();
    }
    ImGui::TableNextColumn();
    if (commands_[choice_] && commands_[choice_]->render_options())
      ++options_version_;
    ImGui::EndTable();
  }

  if (job_ && job_->done.load(std::memory_order_acquire)) finish();
  Column const* source = source_column();
  bool const waiting = source && source->pending();
  Key const want = wanted_key();
  if (!job_ && want != made_key_ && !waiting) start(want);

  if (want != remaining_key_) {
    remaining_ = remaining_letters(app_state().actual_letters, used_letters());
    remaining_key_ = want;
  }
  ImGui::TextDisabled("%s", remaining_.c_str());

  if (job_) {
    ImGui::TextDisabled("running");
  } else if (want != made_key_) {
    ImGui::TextDisabled("waiting for column %d",
                        std::get<ColumnIdentifier>(source_) + 1);
  } else if (failed_) {
    ImGui::TextDisabled("failed");
  } else if (bad_filter_) {
    ImGui::TextDisabled("%zu items, bad filter", output()->size());
  } else if (list_.shown_count() != output()->size()) {
    ImGui::TextDisabled("%zu of %zu items", list_.shown_count(),
                        output()->size());
  } else {
    ImGui::TextDisabled("%zu items", output()->size());
  }

  if (!std::holds_alternative<std::monostate>(source_)) {
    int const selected = list_.selected();
    list_.render();
    if (list_.selected() != selected) ++version_;
  }
  ImGui::PopID();
}

void Column::render_source() {
  Window& window = main_window();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("src:");
  ImGui::SameLine();
  static constexpr char const* widest[] = {"seed"};
  ImGui::SetNextItemWidth(combo_width(widest));
  if (!ImGui::BeginCombo("##source", source_label(source_).c_str())) return;
  if (ImGui::Selectable("seed", std::holds_alternative<Seed>(source_)))
    source_ = Seed();
  for (ColumnIdentifier id = 0; id < window.column_count(); ++id) {
    bool const cycle = window.get_column(id).reads_from(this);
    InputSource const choice = id;
    if (ImGui::Selectable(source_label(choice).c_str(), source_ == choice,
                          cycle ? ImGuiSelectableFlags_Disabled : 0))
      source_ = choice;
  }
  ImGui::EndCombo();
}

bool Column::reads_from(Column const* column) const {
  for (Column const* c = this; c; c = c->source_column()) {
    if (c == column) return true;
  }
  return false;
}

bool Column::pending() const {
  return job_ || wanted_key() != made_key_;
}

std::string Column::selected_item() const {
  int const i = list_.selected();
  return i < 0 ? std::string() : (*output())[i];
}

Column const* Column::source_column() const {
  auto const* id = std::get_if<ColumnIdentifier>(&source_);
  if (!id) return nullptr;
  Window& window = main_window();
  if (*id < 0 || *id >= window.column_count()) return nullptr;
  return &window.get_column(*id);
}

Column::Key Column::wanted_key() const {
  Column const* source = source_column();
  return {choice_, options_version_, app_state().generation,
          source ? source->version() : 0, source_};
}

std::string Column::chain_letters() const {
  Column const* source = source_column();
  std::string chain = source ? source->chain_used_letters() : std::string();
  if (commands_[choice_]) chain += commands_[choice_]->used_letters();
  return chain;
}

std::string Column::used_letters() const {
  std::string used = app_state().used_letters + chain_letters();
  if (Column const* source = source_column()) {
    for (char const c : source->selected_item()) {
      if (std::isalpha(static_cast<unsigned char>(c))) used += c;
    }
  }
  return used;
}

void Column::start(Key key) {
  std::optional<Command> command = commands_[key.choice];
  if (!command) {
    publish(key, true, empty_lines(), "");
    return;
  }

  SharedLines input;
  if (auto const* id = std::get_if<ColumnIdentifier>(&source_)) {
    Column const* source = source_column();
    if (!source) {
      fprintf(stderr, "pgui: no column %d\n", *id);
      publish(key, false, empty_lines(), "");
      return;
    }
    input = source->output();
  } else if (std::holds_alternative<Seed>(source_)) {
    input = app_state().seed;
  } else {
    input = empty_lines();
  }

  AppState const& state = app_state();
  GlobalSettings settings = {state.actual_letters, used_letters(),
                             state.sentence};
  auto job = std::make_shared<Job>();
  job->key = key;
  job->chain_used_letters = chain_letters();
  job_ = job;
  worker_ = std::jthread([job, command = std::move(*command),
                          settings = std::move(settings),
                          input = std::move(input)] {
    Lines output;
    job->ok = command.run({input}, settings, &output);
    job->output = std::make_shared<Lines const>(std::move(output));
    job->done.store(true, std::memory_order_release);
    wake_main_loop();
  });
}

void Column::finish() {
  worker_.join();
  std::shared_ptr<Job> const job = std::move(job_);
  if (job->key == wanted_key()) {
    publish(job->key, job->ok, job->ok ? job->output : empty_lines(),
            job->chain_used_letters);
  }
}

void Column::publish(Key key, bool ok, SharedLines output,
                     std::string chain_used_letters) {
  list_.set_items(std::move(output));
  chain_used_letters_ = std::move(chain_used_letters);
  ++version_;
  made_key_ = key;
  failed_ = !ok;
}
