#include "hotspots.h"

#include <imgui.h>

#include <algorithm>
#include <utility>

#include "letter-bag.h"

bool Hotspots::run(std::vector<SharedLines> const&,
                   LetterToolsParams const& params, Lines* output) const {
  LetterBag bag;
  if (!make_letter_bag(params.letters.c_str(), params.used_letters, &bag))
    return false;
  Lines kept;
  for (std::string const& line : *lines)
    if (fits_letter_bag(bag, az_letters(line))) kept.push_back(line);
  *output = std::move(kept);
  return true;
}

bool Hotspots::render_display_options(bool*) {
  AppState::Hotspots const& hotspots = app_state().hotspots;
  AppState::Hotspots::Sections const& sections = *hotspots.sections;
  std::string chosen = section;
  auto found = std::ranges::find_if(
      sections, [&](auto const& entry) { return entry.first == chosen; });
  if (found == sections.end()) {
    found = sections.begin();
    chosen = found == sections.end() ? std::string() : found->first;
  }
  float longest = 0;
  for (auto const& [name, section_lines] : sections)
    longest = std::max(longest, ImGui::CalcTextSize(name.c_str()).x);
  ImGui::SetNextItemWidth(longest + 2 * ImGui::GetStyle().FramePadding.x +
                          ImGui::GetFrameHeight());
  if (ImGui::BeginCombo("##hotspot", chosen.c_str())) {
    for (auto i = sections.begin(); i != sections.end(); ++i)
      if (ImGui::Selectable(i->first.c_str(), i == found)) found = i;
    ImGui::EndCombo();
    if (found != sections.end()) chosen = found->first;
  }
  if (chosen == section && hotspots.version == version) return false;
  section = std::move(chosen);
  version = hotspots.version;
  lines = found == sections.end() ? std::make_shared<Lines const>()
                                  : found->second;
  return true;
}
