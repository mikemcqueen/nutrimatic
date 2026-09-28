#include "column.h"

#include <SDL.h>
#include <imgui.h>

#include <cctype>
#include <cstdio>
#include <optional>
#include <utility>

#include "app-state.h"
#include "command.h"
#include "window.h"

namespace {

SharedLines const& empty_lines() {
  static SharedLines const lines = std::make_shared<Lines const>();
  return lines;
}

void wake_main_loop() {
  SDL_Event event = {};
  event.type = SDL_USEREVENT;
  SDL_PushEvent(&event);
}

}  // namespace

Column::Column(InputSource source, std::vector<std::string> choices)
    : source_(std::move(source)),
      choices_(std::move(choices)),
      list_(empty_lines()) {}

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

  if (job_ && job_->done.load(std::memory_order_acquire)) finish();
  Column const* source = source_column();
  bool const waiting = source && source->pending();
  Key const want = wanted_key();
  if (!job_ && want != made_key_ && !waiting) start(want);

  if (job_) {
    ImGui::TextDisabled("running");
  } else if (want != made_key_) {
    ImGui::TextDisabled("waiting for column %d",
                        std::get<ColumnIdentifier>(source_) + 1);
  } else if (failed_) {
    ImGui::TextDisabled("failed");
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
  return {choice_, app_state().generation, source ? source->version() : 0};
}

void Column::start(Key key) {
  std::optional<Command> command =
      choices_.empty() ? std::nullopt : make_command(choices_[key.choice]);
  if (!command) {
    publish(key, true, empty_lines());
    return;
  }

  SharedLines input;
  std::string path;
  std::string selected;
  if (auto const* id = std::get_if<ColumnIdentifier>(&source_)) {
    Column const* source = source_column();
    if (!source) {
      fprintf(stderr, "pgui: no column %d\n", *id);
      publish(key, false, empty_lines());
      return;
    }
    input = source->output();
    selected = source->selected_item();
  } else if (auto const* file = std::get_if<std::string>(&source_)) {
    path = *file;
  } else {
    input = empty_lines();
  }

  AppState const& state = app_state();
  GlobalSettings settings = {state.actual_letters, state.used_letters,
                             state.sentence};
  for (char const c : selected) {
    if (std::isalpha(static_cast<unsigned char>(c)))
      settings.used_letters += c;
  }
  auto job = std::make_shared<Job>();
  job->key = key;
  job_ = job;
  worker_ = std::jthread([job, command = std::move(*command),
                          settings = std::move(settings),
                          input = std::move(input), path = std::move(path)] {
    SharedLines in = input;
    if (!in) {
      auto lines = std::make_shared<Lines>();
      if (read_file_lines(path, lines.get())) in = std::move(lines);
    }
    Lines output;
    job->ok = in && command.run({in}, settings, &output);
    job->output = std::make_shared<Lines const>(std::move(output));
    job->done.store(true, std::memory_order_release);
    wake_main_loop();
  });
}

void Column::finish() {
  worker_.join();
  std::shared_ptr<Job> const job = std::move(job_);
  if (job->key == wanted_key()) {
    publish(job->key, job->ok, job->ok ? job->output : empty_lines());
  }
}

void Column::publish(Key key, bool ok, SharedLines output) {
  list_.set_items(std::move(output));
  ++version_;
  made_key_ = key;
  failed_ = !ok;
}
