#include "hotspots.h"

#include <imgui.h>

#include <algorithm>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "app-state.h"
#include "letter-bag.h"
#include "log.h"

std::shared_ptr<HotspotSections const> read_hotspots_file(
    std::filesystem::path const& path, size_t number) {
  Lines lines;
  if (!read_file_lines(path.string(), &lines)) return nullptr;
  std::string const prefix = std::to_string(number) + ".";
  auto sections = std::make_shared<HotspotSections>();
  std::optional<size_t> all;
  Lines pairs;
  auto const close = [&] {
    if (!sections->empty())
      sections->back().second =
          std::make_shared<Lines const>(std::move(pairs));
    pairs.clear();
  };
  for (std::string const& line : lines) {
    size_t const begin = line.find_first_not_of(" \t\r");
    if (begin == std::string::npos) continue;
    std::string_view const text = std::string_view(line).substr(
        begin, line.find_last_not_of(" \t\r") + 1 - begin);
    if (text.size() >= 2 && text.front() == '[' && text.back() == ']') {
      close();
      std::string_view const name = text.substr(1, text.size() - 2);
      if (name == "all" && !all) all = sections->size();
      sections->emplace_back(prefix + std::string(name), nullptr);
    } else if (sections->empty()) {
      warn("pgui", "%s: skipping \"%.*s\" before any [section]",
           path.c_str(), static_cast<int>(text.size()), text.data());
    } else {
      pairs.emplace_back(text);
    }
  }
  close();
  if (all) {
    Lines combined;
    std::unordered_set<std::string_view> seen;
    for (auto const& [name, section_lines] : *sections)
      for (std::string const& pair : *section_lines)
        if (seen.insert(pair).second) combined.push_back(pair);
    (*sections)[*all].second =
        std::make_shared<Lines const>(std::move(combined));
  }
  return sections;
}

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
