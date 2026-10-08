#include "column.h"

#include <SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "app-state.h"
#include "lc-impl.h"
#include "letter-bag.h"
#include "widgets.h"
#include "window.h"

namespace {

SharedLines const& empty_lines() {
  static SharedLines const lines = std::make_shared<Lines const>();
  return lines;
}

// Cuts each of `lines` at its first space and returns what followed, trimmed
// of whitespace, one entry per line, empty for lines without a space. Returns
// empty_lines(), leaving `lines` as they are, when none has a space.
SharedLines split_values(Lines* lines) {
  if (std::ranges::none_of(*lines, [](std::string const& line) {
        return line.find(' ') != std::string::npos;
      }))
    return empty_lines();
  Lines values;
  values.reserve(lines->size());
  for (std::string& line : *lines) {
    size_t const space = line.find(' ');
    if (space == std::string::npos) {
      values.emplace_back();
      continue;
    }
    size_t const begin = line.find_first_not_of(" \t\r", space);
    if (begin == std::string::npos)
      values.emplace_back();
    else
      values.push_back(
          line.substr(begin, line.find_last_not_of(" \t\r") + 1 - begin));
    line.resize(space);
  }
  return std::make_shared<Lines const>(std::move(values));
}

// A dropdown's width: the longest of `labels`, framed, and its arrow.
float combo_width(std::span<char const* const> labels) {
  float longest = 0;
  for (char const* const label : labels)
    longest = std::max(longest, ImGui::CalcTextSize(label).x);
  return longest + 2 * ImGui::GetStyle().FramePadding.x +
         ImGui::GetFrameHeight();
}

// The D: dropdown's choices: each label and its key in app_state()'s
// dictionaries.
constexpr std::pair<char const*, char const*> dictionary_choices[] = {
    {"DICT", "sml_dict"}, {"BIG", "big_dict"}};

// "(N): letters" for the N letters of `letters` left once `used` is removed,
// in the order typed, then "  over: letters" for those of `used` that
// weren't there to remove; sets `cv` to the consonant/vowel ratio of the
// letters left, and `sorted` to their a-z letters, lowercase and sorted, or
// empty when some of `used` weren't there. Characters other than letters and
// digits are skipped.
std::string remaining_letters(std::string const& letters,
                              std::string const& used, double* cv,
                              std::string* sorted) {
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
  LetterCounts letter_counts = {};
  for (char const c : left) {
    if (std::isalpha(static_cast<unsigned char>(c)))
      ++letter_counts[std::tolower(static_cast<unsigned char>(c)) - 'a'];
  }
  *cv = cv_ratio(letter_counts);
  sorted->clear();
  if (over.empty()) {
    for (int i = 0; i < 26; ++i) sorted->append(letter_counts[i], 'a' + i);
  }
  std::string text = "(" + std::to_string(left.size()) + "): " + left;
  if (!over.empty()) text += "  over: " + over;
  return text;
}

char const* const LETTER_COUNT_FONT = "/mnt/c/Windows/Fonts/CascadiaMono.ttf";

// The font lc's output is shown in, or null for the current font.
ImFont* letter_count_font = nullptr;

// lc's output on app_state()'s letters less `used`.
std::vector<std::string> letter_count(std::string const& used) {
  LetterCountOptions options;
  options.text = app_state().actual_letters;
  options.used_letters = used;
  std::vector<std::string> lines;
  if (!letter_count_lines(options, &lines)) return {"lc failed; see stderr"};
  return lines;
}

// The source dropdown's label for `source`: a seed or dictionary key, or a
// column's number counting from 1.
std::string source_label(InputSource const& source) {
  if (auto const* id = std::get_if<ColumnIdentifier>(&source))
    return std::to_string(*id + 1);
  if (auto const* seed = std::get_if<Seed>(&source)) return seed->key;
  if (std::holds_alternative<Dict>(source)) return "dict";
  return "none";
}

// The letter source dropdown's label for `source`: a column's number
// counting from 1, or "all".
std::string letter_source_label(LetterSource const& source) {
  return source ? std::to_string(*source + 1) : "all";
}

void wake_main_loop() {
  SDL_Event event = {};
  event.type = SDL_USEREVENT;
  SDL_PushEvent(&event);
}

}  // namespace

Column::Column(InputSource source, LetterSource letter_source)
    : source_(std::move(source)),
      letter_source_(letter_source),
      list_(empty_lines(), empty_lines()) {
  for (char const* const name : command_names)
    commands_.push_back(make_command(name));
}

void Column::render() {
  ImGui::PushID(this);
  render_sources();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("f:");
  ImGui::SameLine(0, 0);
  char const* dictionary_labels[std::size(dictionary_choices)];
  for (size_t i = 0; i < std::size(dictionary_choices); ++i)
    dictionary_labels[i] = dictionary_choices[i].first;
  float const dictionary_width = combo_width(dictionary_labels);
  if (clearable_input("filter", filter_, sizeof filter_,
                      ImGui::GetStyle().ItemSpacing.x +
                          ImGui::CalcTextSize("D:").x + dictionary_width))
    bad_filter_ = !list_.set_filter(filter_);
  ImGui::SameLine();
  ImGui::TextUnformatted("D:");
  ImGui::SameLine(0, 0);
  ImGui::SetNextItemWidth(dictionary_width);
  if (ImGui::BeginCombo("##dictionary", dictionary_labels[dictionary_])) {
    for (int i = 0; i < static_cast<int>(std::size(dictionary_labels)); ++i) {
      if (ImGui::Selectable(dictionary_labels[i], dictionary_ == i))
        dictionary_ = i;
    }
    ImGui::EndCombo();
  }

  if (ImGui::BeginTable("##command", 2)) {
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,
                            combo_width(command_names));
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##choice", command_names[choice_])) {
      for (int i = 0; i < static_cast<int>(command_names.size()); ++i) {
        if (!ImGui::Selectable(command_names[i], choice_ == i)) continue;
        choice_ = i;
        if (commands_[choice_] && commands_[choice_]->reads_dictionaries() &&
            !std::holds_alternative<Dict>(source_))
          source_ = Dict{};
        if (commands_[choice_] && commands_[choice_]->reads_pairs() &&
            std::holds_alternative<Dict>(source_)) {
          if (id_ > 0)
            source_ = id_ - 1;
          else if (std::string const key = "seed85" + app_state().seed_m;
                   app_state().seed_map.contains(key))
            source_ = Seed{key};
        }
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
  Column const* letters = letters_column();
  std::optional<ColumnIdentifier> waiting_for;
  if (source && source->pending())
    waiting_for = std::get<ColumnIdentifier>(source_);
  else if (letters && letters->pending())
    waiting_for = letter_source_;
  bool const waiting = waiting_for.has_value();
  Key const want = wanted_key();
  if (!job_ && want != made_key_ && !waiting) start(want);

  if (want != remaining_key_) {
    remaining_ = remaining_letters(app_state().actual_letters, used_letters(),
                                   &remaining_cv_, &remaining_sorted_);
    remaining_key_ = want;
  }
  if (made_key_ == remaining_key_ &&
      (made_key_ != hidden_key_ ||
       app_state().judged_bad_version != hidden_judged_version_))
    update_hidden();
  ImVec2 const status_min = ImGui::GetCursorScreenPos();
  float const status_width = ImGui::GetContentRegionAvail().x;
  ImGui::InvisibleButton("##remaining",
      ImVec2(std::max(1.0f, status_width), ImGui::GetTextLineHeight()));
  if (ImGui::IsItemActivated()) letter_count_ = letter_count(used_letters());
  bool const show_letter_count = ImGui::IsItemActive();
  ImGui::SetCursorScreenPos(status_min);
  Window const& window = main_window();
  bool const judge = window.judge() == id_;
  if (!judge) {
    ImGui::TextDisabled("%s", remaining_.c_str());
  } else {
    ImGui::TextColored(ImVec4(1, 1, 1, 1), "%s", remaining_.c_str());
    float const pad = ImGui::GetStyle().ItemSpacing.y / 2;
    ImU32 const color = window.judge_flashing() ? IM_COL32(255, 0, 0, 255)
                                                : IM_COL32(0, 255, 0, 255);
    ImGui::GetWindowDrawList()->AddRect(
        ImVec2(status_min.x - 2, status_min.y - pad),
        ImVec2(status_min.x + status_width + 2, ImGui::GetItemRectMax().y + pad),
        color, 0.0f, 0, 1.0f);
  }
  if (show_letter_count)
    render_letter_count(status_min.x, ImGui::GetItemRectMax().y);

  if (job_) {
    ImGui::TextDisabled("running");
  } else if (waiting) {
    ImGui::TextDisabled("waiting for column %d", *waiting_for + 1);
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
  char cv_text[32];
  std::snprintf(cv_text, sizeof cv_text, "CV: %.2f", remaining_cv_);
  float const cv_width = ImGui::CalcTextSize(cv_text).x;
  ImGui::SameLine();
  float const cv_gap = ImGui::GetContentRegionAvail().x - cv_width;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, cv_gap));
  ImGui::TextDisabled("%s", cv_text);

  if (shows_list()) {
    int const selected = list_.selected();
    list_.render();
    if (list_.selected() != selected) ++version_;
  }
  ImGui::PopID();
}

void load_letter_count_font() {
  ImFontConfig config;
  config.Flags |= ImFontFlags_NoLoadError;
  letter_count_font =
      ImGui::GetIO().Fonts->AddFontFromFileTTF(LETTER_COUNT_FONT, 0.0f, &config);
  if (!letter_count_font)
    std::fprintf(stderr, "pgui: can't load \"%s\"\n", LETTER_COUNT_FONT);
}

void Column::render_letter_count(float x, float y) const {
  ImGui::PushFont(letter_count_font, 0.0f);
  float width = 0;
  for (std::string const& line : letter_count_)
    width = std::max(width, ImGui::CalcTextSize(line.c_str()).x);
  width += 2 * ImGui::GetStyle().WindowPadding.x;
  ImGuiViewport const* const viewport = ImGui::GetMainViewport();
  x = std::max(viewport->WorkPos.x,
               std::min(x, viewport->WorkPos.x + viewport->WorkSize.x - width));
  ImGui::SetNextWindowPos(ImVec2(x, y));
  ImGui::BeginTooltip();
  for (std::string const& line : letter_count_)
    ImGui::TextUnformatted(line.c_str());
  ImGui::EndTooltip();
  ImGui::PopFont();
}

void Column::render_sources() {
  std::vector<std::string> const& keys = app_state().seed_keys;
  bool const reads_dictionaries =
      commands_[choice_] && commands_[choice_]->reads_dictionaries();
  std::string const label = source_label(source_);
  std::vector<char const*> widest = {label.c_str()};
  for (std::string const& key : keys) widest.push_back(key.c_str());
  if (reads_dictionaries) widest.push_back("dict");
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("src:");
  ImGui::SameLine(0, 0);
  ImGui::SetNextItemWidth(combo_width(widest));
  if (ImGui::BeginCombo("##source", label.c_str())) {
    for (std::string const& key : keys) {
      InputSource const choice = Seed{key};
      if (ImGui::Selectable(key.c_str(), source_ == choice)) source_ = choice;
    }
    if (reads_dictionaries) {
      InputSource const choice = Dict{};
      if (ImGui::Selectable("dict", source_ == choice)) source_ = choice;
    }
    for (ColumnIdentifier id = 0; id < id_; ++id) {
      InputSource const choice = id;
      if (ImGui::Selectable(source_label(choice).c_str(), source_ == choice))
        source_ = choice;
    }
    ImGui::EndCombo();
  }

  ImGui::SameLine();
  ImGui::TextUnformatted("ltrs:");
  ImGui::SameLine(0, 0);
  static constexpr char const* letters_widest[] = {"all"};
  ImGui::SetNextItemWidth(combo_width(letters_widest));
  if (ImGui::BeginCombo("##letter_source",
                        letter_source_label(letter_source_).c_str())) {
    if (ImGui::Selectable("all", !letter_source_)) letter_source_.reset();
    for (ColumnIdentifier id = 0; id < id_; ++id) {
      LetterSource const choice = id;
      if (ImGui::Selectable(letter_source_label(choice).c_str(),
                            letter_source_ == choice))
        letter_source_ = choice;
    }
    ImGui::EndCombo();
  }
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

Column const* Column::letters_column() const {
  Window& window = main_window();
  if (!letter_source_ || *letter_source_ < 0 ||
      *letter_source_ >= window.column_count())
    return nullptr;
  return &window.get_column(*letter_source_);
}

Column::Key Column::wanted_key() const {
  Column const* source = source_column();
  Column const* letters = letters_column();
  return {choice_,
          options_version_,
          app_state().generation,
          source ? source->version() : 0,
          source_,
          letters ? letters->version() : 0,
          letter_source_,
          dictionary_};
}

std::string Column::used_letters() const {
  Column const* letters = letters_column();
  std::string used =
      letters ? letters->output_used_letters() : app_state().used_letters;
  if (commands_[choice_]) used += commands_[choice_]->used_letters();
  return used;
}

std::string Column::output_used_letters() const {
  std::string used = used_letters();
  for (char const c : selected_item()) {
    if (std::isalpha(static_cast<unsigned char>(c))) used += c;
  }
  return used;
}

void Column::update_hidden() {
  bool const rerun = made_key_ != hidden_key_;
  hidden_key_ = made_key_;
  AppState const& state = app_state();
  hidden_judged_version_ = state.judged_bad_version;
  std::vector<char> hidden;
  if (state.judged_bad_filter &&
      (!state.judged_bad.empty() || state.bad_bags_bitmap) &&
      !remaining_sorted_.empty()) {
    LetterCounts left = {};
    for (char const c : remaining_sorted_) ++left[c - 'a'];
    Lines const& items = *output();
    std::string key;
    for (size_t i = 0; i < items.size(); ++i) {
      LetterCounts counts = left;
      bool fits = true;
      for (char const c : items[i]) {
        if (!std::isalpha(static_cast<unsigned char>(c))) continue;
        if (--counts[std::tolower(static_cast<unsigned char>(c)) - 'a'] < 0) {
          fits = false;
          break;
        }
      }
      if (!fits) continue;
      key.clear();
      for (int j = 0; j < 26; ++j) key.append(counts[j], 'a' + j);
      if (!state.is_judged_bad(key, counts)) continue;
      if (hidden.empty()) hidden.resize(items.size());
      hidden[i] = 1;
    }
  }
  list_.set_hidden(std::move(hidden));
  int const selected = list_.selected();
  if (rerun)
    list_.deselect_hidden();
  else
    list_.step_off_hidden();
  if (list_.selected() != selected) ++version_;
}

void Column::start(Key key) {
  std::optional<Command> command = commands_[key.choice];
  if (!command) {
    publish(key, true, empty_lines(), empty_lines());
    return;
  }

  AppState& state = app_state();
  SharedDictionary dictionary =
      state.dictionary(dictionary_choices[key.dictionary].second);
  SharedLines input;
  if (auto const* id = std::get_if<ColumnIdentifier>(&source_)) {
    Column const* source = source_column();
    if (!source) {
      fprintf(stderr, "pgui: no column %d\n", *id);
      publish(key, false, empty_lines(), empty_lines());
      return;
    }
    input = source->output();
  } else if (auto const* seed = std::get_if<Seed>(&source_)) {
    input = state.seed_lines(seed->key);
  } else if (std::holds_alternative<Dict>(source_)) {
    input = dictionary->lines;
  } else {
    input = empty_lines();
  }

  LetterToolsParams params = {state.actual_letters, used_letters(),
                              state.sentence, std::move(dictionary)};
  auto job = std::make_shared<Job>();
  job->key = key;
  job_ = job;
  worker_ = std::jthread([job, command = std::move(*command),
                          params = std::move(params),
                          input = std::move(input)] {
    Lines output;
    job->ok = command.run({input}, params, &output);
    job->values = split_values(&output);
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
            job->ok ? job->values : empty_lines());
  }
}

void Column::publish(Key key, bool ok, SharedLines output,
                     SharedLines values) {
  list_.set_items(std::move(output), std::move(values));
  ++version_;
  made_key_ = key;
  failed_ = !ok;
}
