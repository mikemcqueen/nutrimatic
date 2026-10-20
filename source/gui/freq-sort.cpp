#include "freq-sort.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <limits>
#include <utility>

#include "dfs-cli-args.h"
#include "widgets.h"

FreqSort::FreqSort() {
  options.mode = FREQSORT_SCORE_CV;
  options.min_cv = 1.0;
  std::snprintf(min_cv, sizeof min_cv, "%.2f", options.min_cv);
  std::snprintf(max_cv, sizeof max_cv, "%.2f", 0.0);
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
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("m:");
  ImGui::SameLine(0, 0);
  ImGui::SetNextItemWidth(ImGui::CalcTextSize("00").x +
                          2 * ImGui::GetStyle().FramePadding.x);
  *entered |= ImGui::InputInt("##m", &min_letters, 0, 0,
                              ImGuiInputTextFlags_EnterReturnsTrue);
  return false;
}

bool FreqSort::render_display_options(bool* entered) {
  bool const cv = options.mode == FREQSORT_SCORE_CV;
  bool const changed = toggle_button("cv", cv);
  if (changed) options.mode = cv ? FreqsortOptions{}.mode : FREQSORT_SCORE_CV;
  ImGui::SameLine();
  static FreqSort const defaults;
  cv_range_input(min_cv, max_cv, sizeof min_cv, defaults.min_cv,
                 defaults.max_cv, options.mode == FREQSORT_SCORE_CV, entered);
  return changed;
}

bool FreqSort::apply_fields() {
  bool changed = false;
  for (auto [text, bound] : {std::pair{min_cv, &options.min_cv},
                             std::pair{max_cv, &options.max_cv}}) {
    double parsed;
    if (!parse_double(text, "cv bound", &parsed)) continue;
    if (bound == &options.max_cv && parsed == 0)
      parsed = std::numeric_limits<double>::infinity();
    if (parsed != *bound) {
      *bound = parsed;
      changed = true;
    }
  }
  min_letters = std::max(min_letters, 0);
  if (min_letters != options.min_letters) {
    options.min_letters = min_letters;
    changed = true;
  }
  return changed;
}
