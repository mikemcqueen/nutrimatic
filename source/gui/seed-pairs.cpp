#include "seed-pairs.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "dfs-cli-args.h"
#include "log.h"
#include "seed-pairs-impl.h"

bool SeedPairsFilter::run(std::vector<SharedLines> const& inputs,
                          LetterToolsParams const& params,
                          Lines* output) const {
  if (inputs.size() != 1) {
    report("pgui: seed-pairs", "takes 1 input, not %zu", inputs.size());
    return false;
  }
  if (min_len > max_len) {
    report("pgui: seed-pairs", "m %d is more than x %d", min_len, max_len);
    return false;
  }
  SeedPairsOptions options;
  options.p1 = params.p1_path;
  options.filter = {min_len, max_len, min_yes, &params.dictionary->words};
  SeedPairs seed_pairs;
  if (!seed_pairs.load("pgui", options)) return false;
  std::vector<SeedPair> pairs;
  if (!seed_pairs.run(std::span(*inputs[0]), &pairs)) return false;
  Lines kept;
  kept.reserve(pairs.size());
  for (SeedPair& pair : pairs) {
    char text[32];
    snprintf(text, sizeof text, " %.2f", pair.yes);
    kept.push_back(std::move(pair.line) + text);
  }
  *output = std::move(kept);
  return true;
}

bool SeedPairsFilter::render_options(bool* entered) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("pm:");
  ImGui::SameLine(0, 0);
  ImGui::SetNextItemWidth(ImGui::CalcTextSize("0.00").x +
                          2 * ImGui::GetStyle().FramePadding.x);
  *entered |= ImGui::InputText("##pm", min_yes_text, sizeof min_yes_text,
                               ImGuiInputTextFlags_EnterReturnsTrue);
  return false;
}

bool SeedPairsFilter::render_display_options(bool* entered) {
  float const width = ImGui::CalcTextSize("00").x +
                      2 * ImGui::GetStyle().FramePadding.x;
  auto const field = [&](char const* label, char const* id, int* value) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(0, 0);
    ImGui::SetNextItemWidth(width);
    *entered |= ImGui::InputInt(id, value, 0, 0,
                                ImGuiInputTextFlags_EnterReturnsTrue);
  };
  field("m", "##m", &min_len_field);
  ImGui::SameLine(0, ImGui::GetStyle().FramePadding.x);
  field("x", "##x", &max_len_field);
  return false;
}

bool SeedPairsFilter::apply_fields() {
  bool changed = false;
  double parsed;
  if (parse_double(min_yes_text, "pm", &parsed) &&
      float(parsed) != min_yes) {
    min_yes = float(parsed);
    changed = true;
  }
  for (auto [field, value] : {std::pair{&min_len_field, &min_len},
                              std::pair{&max_len_field, &max_len}}) {
    *field = std::max(*field, 1);
    if (*field != *value) {
      *value = *field;
      changed = true;
    }
  }
  return changed;
}
