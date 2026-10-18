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

  BagStatsOptions options;
  BagStatsArgs& args = options.common;
  args.letters = params.letters;
  args.used_letters = params.used_letters;
  args.threads = 0;
  args.progress = 0;
  args.min_word_bag_length = min_word_bag_length;
  args.min_pair_bag_length = min_pair_bag_length;
  bool const word_step = exact && this->word_step;
  args.search.pairs = word_step && pair_step ? 1 : 0;
  args.search.words = word_step ? 1 : 0;
  args.search.pair_filter = true;
  options.exact = exact;
  std::string letters;
  if (!remaining_letters(args, &letters)) return false;

  PairsCounts const bag = pairs_letter_counts(letters.c_str());
  BagStatsInputs loaded(bag);
  PairBagLoader pair_loader(bag, PairPrior{}, &loaded.final_bags,
                            &loaded.final_rows);
  Lines const& lines = *inputs[0];
  for (size_t i = 0; i < lines.size(); ++i) {
    if (params.stop.stop_requested()) return false;
    DfsPairRow row;
    if (!parse_pair_row(lines[i], "pairs", "bag-stats input", i + 1, false,
                        &row))
      return false;
    pair_loader.add(row);
  }
  // Each pb pick's rows.
  std::vector<std::vector<std::string> const*> pick_rows;
  if (args.search.pairs > 0) {
    PairPickFilter const filter(loaded.prospect_bags, min_pair_bag_length);
    for (size_t i = 0; i < loaded.final_bags.size(); ++i) {
      if (filter.dropped(loaded.final_bags[i])) continue;
      loaded.pair_bags.push_back(loaded.final_bags[i]);
      pick_rows.push_back(&loaded.final_rows[i]);
    }
  }
  loaded.steps.assign(size_t(args.search.pairs), &loaded.pair_bags);

  std::vector<std::vector<std::string>> spellings;
  WordBagLoader word_loader(bag, min_word_bag_length, &loaded.word_bags,
                            &spellings);
  if (word_step) {
    for (std::string const& line : *params.dictionary_lines) {
      if (params.stop.stop_requested()) return false;
      word_loader.add(line);
    }
  }

  std::vector<BagStatsCompletion> listed;
  if (!list_bag_stats(options, letters, loaded, params.stop, &listed) ||
      params.stop.stop_requested())
    return false;
  Lines kept;
  for (BagStatsCompletion const& completion : listed) {
    if (completion.word_bags.empty()) continue;
    std::string const value =
        completion.solo ? " SOLO" : " " + std::to_string(completion.pairs);
    uint32_t const index = completion.word_bags[0];
    std::vector<std::string> const& names =
        args.search.pairs > 0 ? *pick_rows[index]
        : args.search.words > 0 ? spellings[index]
        : loaded.final_rows[index];
    for (std::string const& name : names) kept.push_back(name + value);
  }
  *output = std::move(kept);
  return true;
}

namespace {

// A ":" and a two-digit field for `length` after the item before it; true
// when Enter is pressed in it.
bool length_field(char const* id, int* length) {
  ImGui::SameLine(0, 0);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(":");
  ImGui::SameLine(0, 0);
  ImGui::SetNextItemWidth(ImGui::CalcTextSize("00").x +
                          2 * ImGui::GetStyle().FramePadding.x);
  return ImGui::InputInt(id, length, 0, 0,
                         ImGuiInputTextFlags_EnterReturnsTrue);
}

// Sets `length` from `field`, at least 0; true when that changes it.
bool apply_length(int* field, int* length) {
  *field = std::max(*field, 0);
  bool const changed = *field != *length;
  *length = *field;
  return changed;
}

}  // namespace

bool BagStats::render_options(bool* entered) {
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
  *entered |= length_field("##pb", &min_pair_bag_length_field);
  ImGui::EndDisabled();
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!exact);
  if (toggle_button("wb", word_step)) {
    word_step = !word_step;
    changed = true;
  }
  ImGui::EndDisabled();
  *entered |= length_field("##wb", &min_word_bag_length_field);
  return changed;
}

bool BagStats::apply_fields() {
  bool const pair =
      apply_length(&min_pair_bag_length_field, &min_pair_bag_length);
  bool const word =
      apply_length(&min_word_bag_length_field, &min_word_bag_length);
  return pair || word;
}
