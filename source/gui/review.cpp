#include "review.h"

#include <SDL.h>
#include <fcntl.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>

#include "app-state.h"
#include "classified.h"
#include "dfs-cli-args.h"
#include "workflow-paths.h"

namespace {

void wake_main_loop() {
  SDL_Event event = {};
  event.type = SDL_USEREVENT;
  SDL_PushEvent(&event);
}

// Writes `lines` to `path`, one per line. Returns false, with the error in
// `output`, when it can't.
bool write_lines(std::filesystem::path const& path,
                 std::vector<std::string> const& lines, std::string* output) {
  std::ofstream out(path);
  for (std::string const& line : lines) out << line << '\n';
  out.close();
  if (out) return true;
  *output = "can't write " + path.string() + "\n";
  return false;
}

// Runs `args` with stdout and stderr both appended to `output`. Returns true
// when it exits 0.
bool run_command(std::vector<std::string> const& args, std::string* output) {
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) {
    *output = std::string("pipe: ") + std::strerror(errno) + "\n";
    return false;
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
  std::vector<char*> argv;
  for (std::string const& arg : args)
    argv.push_back(const_cast<char*>(arg.c_str()));
  argv.push_back(nullptr);
  pid_t pid;
  int const error =
      posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(fds[1]);
  if (error != 0) {
    close(fds[0]);
    *output = "can't run " + args[0] + ": " + std::strerror(error) + "\n";
    return false;
  }
  char buffer[4096];
  ssize_t n;
  while ((n = read(fds[0], buffer, sizeof buffer)) > 0 ||
         (n < 0 && errno == EINTR)) {
    if (n > 0) output->append(buffer, n);
  }
  close(fds[0]);
  int status;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
  *output += WIFEXITED(status)
      ? args[0] + " exited with status " + std::to_string(WEXITSTATUS(status))
      : args[0] + " was killed by signal " + std::to_string(WTERMSIG(status));
  return false;
}

// Records `yes` and `no` for `sentence` through `wf -d root classify pairs`.
// Returns true when it succeeds; `output` gets what it printed.
bool classify_pairs(std::string const& wf, std::string const& root,
                    int sentence, std::vector<std::string> const& yes,
                    std::vector<std::string> const& no, std::string* output) {
  std::string pattern =
      (std::filesystem::temp_directory_path() / "pgui-review.XXXXXX").string();
  if (mkdtemp(pattern.data()) == nullptr) {
    *output = "can't make " + pattern + ": " + std::strerror(errno) + "\n";
    return false;
  }
  std::filesystem::path const dir = pattern;
  std::filesystem::path const yes_file = dir / "yes.pairs";
  std::filesystem::path const no_file = dir / "no.pairs";
  bool const ok = write_lines(yes_file, yes, output) &&
      write_lines(no_file, no, output) &&
      run_command({wf, "-d", root, "classify", "pairs", "-s",
                   std::to_string(sentence), "--yes", yes_file.string(),
                   "--no", no_file.string()},
                  output);
  std::error_code ignored;
  std::filesystem::remove_all(dir, ignored);
  return ok;
}

}  // namespace

std::unique_ptr<Review> Review::open(SharedLines items, SharedLines values,
                                     std::vector<int> rows, int sentence) {
  char const* const wf = std::getenv("WF");
  if (wf == nullptr || wf[0] == '\0') {
    std::fputs("pgui: WF must be set to review pairs\n", stderr);
    return nullptr;
  }
  char const* const root = require_workflow_root("pgui");
  if (root == nullptr) return nullptr;
  ClassifiedPairCache& cache = app_state().classified_pairs;
  std::shared_ptr<DfsPairSet const> const global_yes =
      cache.get("pgui", root, CLASSIFIED_NO_SENTENCE, "yes");
  std::shared_ptr<DfsPairSet const> const sentence_yes =
      cache.get("pgui", root, sentence, "yes");
  if (global_yes == nullptr || sentence_yes == nullptr) return nullptr;

  std::unique_ptr<Review> review(new Review);
  review->checked_.resize(rows.size());
  review->locked_.resize(rows.size());
  for (size_t n = 0; n < rows.size(); ++n) {
    int const i = rows[n];
    DfsPairRow row;
    if (!parse_pair_row((*items)[i], "pair list", "review", i + 1, false,
                        &row))
      return nullptr;
    if (global_yes->contains(row.entry()) ||
        sentence_yes->contains(row.entry())) {
      review->checked_[n] = 1;
      review->locked_[n] = 1;
      ++review->locked_count_;
    }
  }
  review->items_ = std::move(items);
  review->values_ = std::move(values);
  review->rows_ = std::move(rows);
  review->sentence_ = sentence;
  review->root_ = root;
  review->wf_ = wf;
  return review;
}

Review::Result Review::render(ImVec2 min, ImVec2 max) {
  Result result = Result::open;
  if (job_ && job_->done.load(std::memory_order_acquire)) {
    worker_.join();
    std::shared_ptr<Job> const job = std::move(job_);
    if (job->ok)
      result = Result::submitted;
    else
      failure_ = job->output;
  }

  if (!opened_) {
    ImGui::OpenPopup("Review");
    opened_ = true;
  }
  ImGuiStyle const& style = ImGui::GetStyle();
  float const header = ImGui::GetTextLineHeight() + style.ItemSpacing.y;
  ImGui::SetNextWindowPos(ImVec2(min.x, min.y - header));
  ImGui::SetNextWindowSize(ImVec2(max.x - min.x, max.y - min.y + header));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_PopupBg, style.Colors[ImGuiCol_WindowBg]);
  ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, IM_COL32(0, 0, 0, 0x60));
  bool const open = ImGui::BeginPopupModal(
      "Review", nullptr,
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(2);
  if (!open) return result;

  int const count = static_cast<int>(rows_.size());
  ImGui::Text("S%d: %d YES / %d NO", sentence_,
              locked_count_ + checked_count_,
              count - locked_count_ - checked_count_);
  if (locked_count_ > 0) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%d already YES)", locked_count_);
  }
  if (job_) {
    ImGui::SameLine();
    ImGui::TextDisabled("submitting...");
  }
  if (!failure_.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 96, 96, 255));
    ImGui::TextWrapped("%s", failure_.c_str());
    ImGui::PopStyleColor();
  }

  Confirm const before = confirm_;
  render_rows();
  if (confirm_ == Confirm::none && !job_ &&
      ImGui::Shortcut(ImGuiKey_Escape))
    confirm_ = Confirm::cancel;
  if (confirm_ == Confirm::cancel && checked_count_ == 0) {
    confirm_ = Confirm::none;
    result = Result::cancelled;
  }
  if (confirm_ != before && confirm_ != Confirm::none)
    ImGui::OpenPopup(confirm_ == Confirm::submit ? "Submit?" : "Cancel?");

  char question[64];
  std::snprintf(question, sizeof question, "Record %d YES / %d NO for S%d?",
                checked_count_, count - locked_count_ - checked_count_,
                sentence_);
  if (render_confirm("Submit?", question)) submit();
  if (render_confirm("Cancel?", "Discard these checks?"))
    result = Result::cancelled;

  if (result != Result::open) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
  return result;
}

void Review::render_rows() {
  ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0x31, 0x3b, 0x4a, 0xff));
  bool const open = ImGui::BeginListBox("##rows", ImVec2(-FLT_MIN, -FLT_MIN));
  ImGui::PopStyleColor();
  if (!open) return;
  Lines const& items = *items_;
  Lines const& values = *values_;
  int const count = static_cast<int>(rows_.size());
  float const height = ImGui::GetTextLineHeight();
  float const pad = height * 0.15f;
  float const spacing = ImGui::GetStyle().ItemSpacing.x;
  ImDrawList* const draw = ImGui::GetWindowDrawList();
  ImU32 const text = IM_COL32(0xe4, 0xe4, 0xe4, 0xff);
  ImU32 const disabled = ImGui::GetColorU32(ImGuiCol_TextDisabled);
  ImGuiListClipper clipper;
  clipper.Begin(count + 1);
  while (clipper.Step()) {
    for (int n = clipper.DisplayStart; n < clipper.DisplayEnd; ++n) {
      if (n == count) {
        ImGui::BeginDisabled(job_ != nullptr);
        if (ImGui::Button("SUBMIT")) confirm_ = Confirm::submit;
        ImGui::SameLine();
        if (ImGui::Button("CANCEL")) confirm_ = Confirm::cancel;
        ImGui::EndDisabled();
      } else {
        int const i = rows_[n];
        ImGui::PushID(n);
        if (ImGui::Selectable("##pair", checked_[n],
                              ImGuiSelectableFlags_NoAutoClosePopups,
                              ImVec2(0, height)) &&
            !locked_[n] && !job_) {
          checked_[n] = !checked_[n];
          checked_count_ += checked_[n] ? 1 : -1;
        }
        ImVec2 const min = ImGui::GetItemRectMin();
        float const right = ImGui::GetItemRectMax().x;
        ImU32 const color = locked_[n] ? disabled : text;
        draw->AddRect(ImVec2(min.x + pad, min.y + pad),
                      ImVec2(min.x + height - pad, min.y + height - pad),
                      color);
        if (checked_[n])
          ImGui::RenderCheckMark(draw,
                                 ImVec2(min.x + 2 * pad, min.y + 2 * pad),
                                 color, height - 4 * pad);
        draw->AddText(ImVec2(min.x + height + spacing, min.y), color,
                      items[i].c_str());
        if (!values.empty() && !values[i].empty())
          draw->AddText(
              ImVec2(right - ImGui::CalcTextSize(values[i].c_str()).x, min.y),
              disabled, values[i].c_str());
        ImGui::PopID();
      }
      if (focus_first_ && n == 0) {
        ImGui::FocusItem();
        ImGui::SetNavCursorVisibleAfterMove();
        focus_first_ = false;
      }
    }
  }
  ImGui::EndListBox();
}

bool Review::render_confirm(char const* id, std::string const& question) {
  if (!ImGui::BeginPopupModal(id, nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize |
                                  ImGuiWindowFlags_NoSavedSettings))
    return false;
  ImGui::TextUnformatted(question.c_str());
  bool const yes = ImGui::Button("YES");
  ImGui::SameLine();
  bool const no = ImGui::Button("NO") || ImGui::Shortcut(ImGuiKey_Escape);
  ImGui::SetItemDefaultFocus();
  if (yes || no) {
    confirm_ = Confirm::none;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
  return yes;
}

void Review::submit() {
  std::vector<std::string> yes;
  std::vector<std::string> no;
  for (size_t n = 0; n < rows_.size(); ++n) {
    if (locked_[n]) continue;
    (checked_[n] ? yes : no).push_back((*items_)[rows_[n]]);
  }
  failure_.clear();
  auto job = std::make_shared<Job>();
  job_ = job;
  worker_ = std::jthread([job, wf = wf_, root = root_, sentence = sentence_,
                          yes = std::move(yes), no = std::move(no)] {
    job->ok = classify_pairs(wf, root, sentence, yes, no, &job->output);
    job->done.store(true, std::memory_order_release);
    wake_main_loop();
  });
}
