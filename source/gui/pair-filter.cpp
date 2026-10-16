#include "pair-filter.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <utility>

#include "dfs-cli-args.h"
#include "letter-bag.h"
#include "widgets.h"

bool PairFilter::run(std::vector<SharedLines> const& inputs,
                     LetterToolsParams const& params, Lines* output) const {
  if (inputs.size() != 1) {
    fprintf(stderr, "pgui: pfilter takes 1 input, not %zu\n", inputs.size());
    return false;
  }

  PfilterOptions options;
  if (!params.letters.empty()) {
    LetterBag bag;
    if (!make_letter_bag(params.letters.c_str(), params.used_letters, &bag))
      return false;
    options.bag = bag;
  } else if (!params.used_letters.empty()) {
    fputs("pgui: pfilter used letters require letters\n", stderr);
    return false;
  } else if (cv) {
    fputs("pgui: pfilter cv requires letters\n", stderr);
    return false;
  }
  options.max_letters = max_letters;
  options.sentence = params.sentence;
  options.drop_yes = yes;
  options.dictionary = std::cref(params.dictionary->words);

  Pfilter filter;
  if (!filter.load("pgui", options, app_state().classified_pairs))
    return false;

  Lines const& lines = *inputs[0];
  Lines kept;
  std::vector<std::pair<double, size_t>> ratios;
  for (size_t i = 0; i < lines.size(); ++i) {
    DfsPairRow row;
    if (!parse_pair_row(lines[i], "pair list", "pfilter input", i + 1, false,
            &row))
      return false;
    if (!filter.keep(row)) continue;
    if (cv)
      ratios.emplace_back(
          remaining_cv_ratio(*options.bag, row.left + row.right), i);
    else
      kept.push_back(lines[i]);
  }
  if (cv) {
    std::ranges::stable_sort(ratios, {},
                             &std::pair<double, size_t>::first);
    kept.reserve(ratios.size());
    for (auto const& [ratio, i] : ratios) {
      char text[32];
      snprintf(text, sizeof text, " %.2f", ratio);
      kept.push_back(lines[i] + text);
    }
  } else {
    std::ranges::sort(kept);
  }
  *output = std::move(kept);
  return true;
}

bool PairFilter::render_options(bool* entered) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("x:");
  ImGui::SameLine(0, 0);
  ImGui::SetNextItemWidth(ImGui::CalcTextSize("00").x +
                          2 * ImGui::GetStyle().FramePadding.x);
  *entered |= ImGui::InputInt("##x", &max_letters_field, 0, 0,
                              ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("u:");
  ImGui::SameLine(0, 0);
  *entered |= clearable_input("u", used_text, sizeof used_text);
  return false;
}

bool PairFilter::render_display_options(bool* entered) {
  bool const changed = toggle_button("cv", cv);
  if (changed) {
    cv = !cv;
    ++display_version_;
  }
  float const width = ImGui::CalcTextSize("00.00").x +
                      2 * ImGui::GetStyle().FramePadding.x;
  auto const field = [&](char const* label, char const* id, char* text,
                         size_t size) {
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(0, 0);
    ImGui::SetNextItemWidth(width);
    *entered |= ImGui::InputText(id, text, size,
                                 ImGuiInputTextFlags_EnterReturnsTrue);
  };
  ImGui::BeginDisabled(!cv);
  field("m:", "##min_cv", min_cv_text, sizeof min_cv_text);
  field("x:", "##max_cv", max_cv_text, sizeof max_cv_text);
  ImGui::EndDisabled();
  return changed;
}

bool PairFilter::apply_fields() {
  for (auto [text, bound] : {std::pair{min_cv_text, &min_cv},
                             std::pair{max_cv_text, &max_cv}}) {
    double parsed;
    if (parse_double(text, "cv bound", &parsed) && parsed != *bound) {
      *bound = parsed;
      ++display_version_;
    }
  }
  bool changed = false;
  max_letters_field = std::max(max_letters_field, 0);
  if (max_letters_field != max_letters) {
    max_letters = max_letters_field;
    changed = true;
  }
  if (used != used_text) {
    used = used_text;
    changed = true;
  }
  return changed;
}

bool PairFilter::display_hides(std::string const& value) const {
  if (!cv || value.empty()) return false;
  double const ratio = std::strtod(value.c_str(), nullptr);
  return ratio < min_cv || ratio > max_cv;
}
