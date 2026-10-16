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
  min_letters = options.min_letters;
}

bool FreqSort::run(std::vector<SharedLines> const& inputs,
                   LetterToolsParams const& params, Lines* output) const {
  if (inputs.size() != 1) {
    fprintf(stderr, "pgui: freqsort takes 1 input, not %zu\n", inputs.size());
    return false;
  }

  FreqsortOptions call = options;
  call.letters = params.letters;
  call.used_letters = params.used_letters;
  Freqsort freqsort;
  if (!freqsort.load(call)) return false;

  for (std::string const& line : *inputs[0]) freqsort.add(line);
  *output = freqsort.lines();
  return true;
}

bool FreqSort::render_options(bool* entered) {
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
  *entered |= ImGui::InputText("##min_cv", min_cv, sizeof min_cv,
                               ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("m:");
  ImGui::SameLine(0, 0);
  ImGui::SetNextItemWidth(ImGui::CalcTextSize("00").x +
                          2 * ImGui::GetStyle().FramePadding.x);
  *entered |= ImGui::InputInt("##m", &min_letters, 0, 0,
                              ImGuiInputTextFlags_EnterReturnsTrue);
  return changed;
}

bool FreqSort::apply_fields() {
  bool changed = false;
  double parsed;
  if (parse_double(min_cv, "min: value", &parsed) &&
      parsed != options.min_cv) {
    options.min_cv = parsed;
    changed = true;
  }
  min_letters = std::max(min_letters, 0);
  if (min_letters != options.min_letters) {
    options.min_letters = min_letters;
    changed = true;
  }
  return changed;
}
