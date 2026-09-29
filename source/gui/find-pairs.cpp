#include "find-pairs.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

FindPairs::FindPairs() {
  options.min_word_length = 3;
  options.exact = true;
  options.allow_solo = true;
}

bool FindPairs::run(std::vector<SharedLines> const& inputs,
                    GlobalSettings const& settings, Lines* output) const {
  if (inputs.size() != 1) {
    fprintf(stderr, "pgui: pairs takes 1 input, not %zu\n", inputs.size());
    return false;
  }

  PairsOptions call = options;
  call.letters = settings.letters;
  call.used_letters = settings.used_letters;
  Pairs pairs;
  if (!pairs.load(call)) return false;

  for (std::string const& line : *inputs[0]) pairs.add(line);
  *output = pairs.lines();
  return true;
}

bool FindPairs::render_options() {
  bool changed = false;
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("m:");
  ImGui::SameLine();
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
  changed |= ImGui::Checkbox("x", &options.exact);
  ImGui::SameLine();
  ImGui::BeginDisabled(!options.exact);
  changed |= ImGui::Checkbox("solo", &options.allow_solo);
  ImGui::EndDisabled();
  return changed;
}
