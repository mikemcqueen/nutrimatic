#include "freq-sort.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

#include "dfs-cli-args.h"
#include "widgets.h"

FreqSort::FreqSort() {
  options.mode = FREQSORT_SCORE_CV;
  options.min_cv = 1.0;
  std::snprintf(min_cv, sizeof min_cv, "%.1f", options.min_cv);
}

bool FreqSort::run(std::vector<SharedLines> const& inputs,
                   GlobalSettings const& settings, Lines* output) const {
  if (inputs.size() != 1) {
    fprintf(stderr, "pgui: freqsort takes 1 input, not %zu\n", inputs.size());
    return false;
  }

  FreqsortOptions call = options;
  call.letters = settings.letters;
  call.used_letters = settings.used_letters;
  Freqsort freqsort;
  if (!freqsort.load(call)) return false;

  for (std::string const& line : *inputs[0]) freqsort.add(line);
  *output = freqsort.lines();
  return true;
}

bool FreqSort::render_options() {
  float const width = ImGui::CalcTextSize("00.0").x +
                      2 * ImGui::GetStyle().FramePadding.x;
  bool changed = false;
  bool const cv = options.mode == FREQSORT_SCORE_CV;
  if (toggle_button("cv", cv)) {
    options.mode = cv ? FreqsortOptions{}.mode : FREQSORT_SCORE_CV;
    changed = true;
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(options.mode != FREQSORT_SCORE_CV);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("min:");
  ImGui::SameLine(0, 0);
  ImGui::SetNextItemWidth(width);
  if (ImGui::InputText("##min_cv", min_cv, sizeof min_cv,
                       ImGuiInputTextFlags_EnterReturnsTrue)) {
    double parsed;
    if (parse_double(min_cv, "min: value", &parsed)) {
      options.min_cv = parsed;
      changed = true;
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("m:");
  ImGui::SameLine(0, 0);
  ImGui::SetNextItemWidth(ImGui::CalcTextSize("00").x +
                          2 * ImGui::GetStyle().FramePadding.x);
  int letters = options.min_letters;
  if (ImGui::InputInt("##m", &letters, 0, 0,
                      ImGuiInputTextFlags_EnterReturnsTrue)) {
    letters = std::max(letters, 0);
    changed |= letters != options.min_letters;
    options.min_letters = letters;
  }
  return changed;
}
