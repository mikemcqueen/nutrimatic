#include "pair-filter.h"

#include <imgui.h>

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
  }
  options.sentence = settings.sentence;
  options.drop_yes = yes;
  options.dictionary = std::cref(app_state().dictionaries.at("big_dict").words);

  Pfilter filter;
  if (!filter.load("pgui", options)) return false;

  Lines const& lines = *inputs[0];
  Lines kept;
  for (size_t i = 0; i < lines.size(); ++i) {
    DfsPairRow row;
    if (!parse_pair_row(lines[i], "pair list", "pfilter input", i + 1, false,
            &row))
      return false;
    if (filter.keep(row)) kept.push_back(lines[i]);
  }
  *output = std::move(kept);
  return true;
}

bool PairFilter::render_options() {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("u:");
  ImGui::SameLine();
  if (!clearable_input("u", used_text, sizeof used_text) || used == used_text)
    return false;
  used = used_text;
  return true;
}
