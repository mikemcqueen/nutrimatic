#include "bag-stats.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <utility>
#include <string>

#include "bag-stats-cpu.h"
#include "widgets.h"

bool BagStats::run(std::vector<SharedLines> const& inputs,
                   LetterToolsParams const& params, Lines* output) const {
  if (inputs.size() != 1) {
    fprintf(stderr, "pgui: bag-stats takes 1 input, not %zu\n", inputs.size());
    return false;
  }

  BagStatsArgs args;
  args.letters = params.letters;
  args.used_letters = params.used_letters;
  args.threads = 0;
  std::string letters;
  if (!remaining_letters(args, &letters)) return false;

  bool const word_step = exact && this->word_step;
  PairsCounts const bag = pairs_letter_counts(letters.c_str());
  PairFilterInputs pf(word_step && pair_step);
  PairBagLoader pair_loader(bag, PairPrior{}, &pf.pair_bags,
                            pf.pair_rows_out());
  Lines const& lines = *inputs[0];
  for (size_t i = 0; i < lines.size(); ++i) {
    if (params.stop.stop_requested()) return false;
    DfsPairRow row;
    if (!parse_pair_row(lines[i], "pairs", "bag-stats input", i + 1, false,
                        &row))
      return false;
    pair_loader.add(row);
  }
  pf.pick_pair_step_bags(min_pair_bag_length);

  WordBagLoader word_loader(bag, min_word_bag_length, &pf.word_bags,
                            pf.word_spellings_out());
  for (std::string const& line : *params.dictionary_lines) {
    if (params.stop.stop_requested()) return false;
    word_loader.add(line);
  }

  std::vector<std::vector<std::string>> const& spellings = pf.spellings();
  PairFilterResult const result =
      count_pair_filter(bag, pf, exact, word_step ? 1 : 0, PairFilterSort::TP,
                        bag_stats_threads(args), spellings.size(),
                        params.stop);
  if (params.stop.stop_requested()) return false;
  Lines kept;
  for (BagStatsCompletion const& completion : result.best) {
    std::string const value =
        completion.solo ? " SOLO" : " " + std::to_string(completion.pairs);
    for (std::string const& word : spellings[completion.word_bags[0]])
      kept.push_back(word + value);
  }
  *output = std::move(kept);
  return true;
}

namespace {

// A ":" and a two-digit field for `length`, at least 0, after the item before
// it; true when Enter changes it.
bool length_field(char const* id, int* length) {
  ImGui::SameLine(0, 0);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(":");
  ImGui::SameLine(0, 0);
  ImGui::SetNextItemWidth(ImGui::CalcTextSize("00").x +
                          2 * ImGui::GetStyle().FramePadding.x);
  int value = *length;
  if (!ImGui::InputInt(id, &value, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue))
    return false;
  value = std::max(value, 0);
  bool const changed = value != *length;
  *length = value;
  return changed;
}

}  // namespace

bool BagStats::render_options() {
  bool changed = false;
  if (toggle_button("ex", exact)) {
    exact = !exact;
    changed = true;
  }
  ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
  ImGui::BeginDisabled(!exact || !word_step);
  if (toggle_button("pb", pair_step)) {
    pair_step = !pair_step;
    changed = true;
  }
  ImGui::BeginDisabled(!pair_step);
  changed |= length_field("##pb", &min_pair_bag_length);
  ImGui::EndDisabled();
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!exact);
  if (toggle_button("wb", word_step)) {
    word_step = !word_step;
    changed = true;
  }
  ImGui::EndDisabled();
  changed |= length_field("##wb", &min_word_bag_length);
  return changed;
}
