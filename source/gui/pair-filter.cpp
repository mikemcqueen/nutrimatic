#include "pair-filter.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <utility>

#include "letter-bag.h"
#include "widgets.h"

bool PairFilter::run(std::vector<SharedLines> const& inputs,
                     GlobalSettings const& settings, Lines* output) const {
  if (inputs.size() != 1) {
    fprintf(stderr, "pgui: pfilter takes 1 input, not %zu\n", inputs.size());
    return false;
  }

  PfilterOptions options;
  if (!settings.letters.empty()) {
    LetterBag bag;
    if (!make_letter_bag(settings.letters.c_str(), settings.used_letters, &bag))
      return false;
    options.bag = bag;
  } else if (!settings.used_letters.empty()) {
    fputs("pgui: pfilter used letters require letters\n", stderr);
    return false;
  } else if (cv) {
    fputs("pgui: pfilter cv requires letters\n", stderr);
    return false;
  }
  options.sentence = settings.sentence;
  options.drop_yes = yes;
  options.dictionary = std::cref(app_state().dictionaries.at("big_dict").words);

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

bool PairFilter::render_options() {
  bool const cv_changed = toggle_button("cv", cv);
  if (cv_changed) cv = !cv;
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("u:");
  ImGui::SameLine(0, 0);
  if (!clearable_input("u", used_text, sizeof used_text) || used == used_text)
    return cv_changed;
  used = used_text;
  return true;
}
