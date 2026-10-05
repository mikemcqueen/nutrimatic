#include "find-pairs.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

#include "widgets.h"

FindPairs::FindPairs() {
  options.min_word_length = 3;
  options.exact = true;
  options.min_words = 1;
  options.max_words = 2;
}

bool FindPairs::run(std::vector<SharedLines> const& inputs,
                    LetterToolsParams const& params, Lines* output) const {
  if (inputs.size() != 1) {
    fprintf(stderr, "pgui: pairs takes 1 input, not %zu\n", inputs.size());
    return false;
  }

  PairsOptions call = options;
  call.letters = params.letters;
  call.used_letters = params.used_letters;
  Pairs pairs;
  if (!pairs.load(call)) return false;

  for (std::string const& line : *inputs[0]) pairs.add(line);
  *output = pairs.lines();
  std::sort(output->begin(), output->end());
  return true;
}

bool FindPairs::render_options() {
  bool changed = false;
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("m:");
  ImGui::SameLine(0, 0);
  ImGui::SetNextItemWidth(ImGui::CalcTextSize("00").x +
                          2 * ImGui::GetStyle().FramePadding.x);
  int length = options.min_word_length;
  if (ImGui::InputInt("##m", &length, 0, 0,
                      ImGuiInputTextFlags_EnterReturnsTrue)) {
    length = std::max(length, 0);
    changed = length != options.min_word_length;
    options.min_word_length = length;
  }
  ImGui::SameLine();
  if (toggle_button("ex", options.exact)) {
    options.exact = !options.exact;
    changed = true;
  }
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("mx:");
  ImGui::SameLine(0, 0);
  float const words_width = ImGui::CalcTextSize("0").x +
                            2 * ImGui::GetStyle().FramePadding.x;
  ImGui::SetNextItemWidth(words_width);
  int min_words = options.min_words;
  if (ImGui::InputInt("##min_words", &min_words, 0, 0,
                      ImGuiInputTextFlags_EnterReturnsTrue)) {
    min_words = std::clamp(min_words, 1, PAIRS_MAX_WORDS);
    changed |= min_words != options.min_words;
    options.min_words = min_words;
    options.max_words = std::max(options.max_words, min_words);
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(words_width);
  int max_words = options.max_words;
  if (ImGui::InputInt("##max_words", &max_words, 0, 0,
                      ImGuiInputTextFlags_EnterReturnsTrue)) {
    max_words = std::clamp(max_words, 1, PAIRS_MAX_WORDS);
    changed |= max_words != options.max_words;
    options.max_words = max_words;
    options.min_words = std::min(options.min_words, max_words);
  }
  return changed;
}
