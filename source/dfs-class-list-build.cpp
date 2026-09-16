#include "dfs-class-list-build.h"

#include <stdio.h>
#include <unistd.h>

#include <array>

#include "dfs-diagnostic.h"
#include "index.h"

namespace {

bool row_fits_bag(std::array<int, 256> remaining, DfsPairRow const& row) {
  std::string const text = row.left + row.right;
  for (size_t i = 0; i < text.size(); ++i) {
    unsigned char const ch = (unsigned char) text[i];
    if (remaining[ch] == 0) return false;
    --remaining[ch];
  }
  return true;
}

void warn_dictionary_drops(
    IndexReader const& reader, std::string const& letters,
    DfsCommonArgs const& args, DfsDictionary const& dictionary,
    DfsPairSet const& exclude_pairs, std::vector<DfsPairRow> const& rows) {
  std::array<int, 256> bag;
  bag.fill(0);
  for (size_t i = 0; i < letters.size(); ++i)
    ++bag[(unsigned char) letters[i]];

  bool const tty = isatty(fileno(stderr));
  DfsPairSet reported;
  for (size_t i = 0; i < rows.size(); ++i) {
    DfsPairRow const& row = rows[i];
    bool const pair = !row.right.empty();
    bool const left_missing = dictionary.count(row.left) == 0;
    bool const right_missing = pair && dictionary.count(row.right) == 0;
    if (!left_missing && !right_missing) continue;
    if (!row_fits_bag(bag, row)) continue;

    std::string const entry = pair ? row.left + "," + row.right : row.left;
    if (reported.count(entry) != 0) continue;
    if (pair) {
      if (args.max_extract_words == 1) continue;
      if (exclude_pairs.count(row.left + " " + row.right) != 0) continue;
    } else {
      int64_t count;
      if (!reader.aggregate_entry_count(row.left, &count)) continue;
    }
    reported.insert(entry);

    bool const both = left_missing && right_missing && row.left != row.right;
    std::string const missing = both
        ? row.left + " and " + row.right
        : (left_missing ? row.left : row.right);
    dfs_diagnostic_to_stream(stderr,
        "%sWARNING: %s dropped because %s %s not in dictionary%s\n",
        tty ? "\033[31m" : "", entry.c_str(), missing.c_str(),
        both ? "are" : "is", tty ? "\033[0m" : "");
  }
}

void admit_best_words(
    std::vector<DfsPairRow> const& rows, DfsDictionary* dictionary) {
  bool const tty = isatty(fileno(stderr));
  for (size_t i = 0; i < rows.size(); ++i) {
    std::string const* const words[] = { &rows[i].left, &rows[i].right };
    for (size_t w = 0; w < 2; ++w) {
      std::string const& word = *words[w];
      if (word.empty() || !dictionary->insert(word).second) continue;
      dfs_diagnostic_to_stream(stderr,
          "%sadded %s to dictionary from BEST pairs%s\n",
          tty ? "\033[33m" : "", word.c_str(), tty ? "\033[0m" : "");
    }
  }
}

}  // namespace

DfsBestBonusPolicy dfs_best_bonus_policy(size_t exact_segments) {
  return exact_segments > 0
      ? DfsBestBonusPolicy::descending(exact_segments)
      : DfsBestBonusPolicy::fixed(DFS_BEST_PAIR_BONUS);
}

bool prepare_dfs_scoring_inputs(
    IndexReader* reader, DfsCommonArgs const& args, bool score_mode,
    size_t exact_segments, DfsPreparedClassList* out) {
  if (args.pair_file != NULL) {
    bool const loaded = score_mode
        ? load_pair_file(
              args.pair_file, "pair list", &out->pairs, false, false, true)
        : load_extraction_pair_file(
              args.pair_file, "pair list", args.min_word_len,
              &out->pairs, &out->exception_prefixes, false, false,
              &out->external_rows);
    if (!loaded) return false;
  }
  if (!load_weighted_pair_files(
          args, score_mode, args.min_word_len,
          &out->weighted_pairs, &out->exception_prefixes,
          &out->external_rows, &out->best_rows))
    return false;

  DfsBestBonusPolicy const best_bonus =
      dfs_best_bonus_policy(exact_segments);
  double const pair_bonus = args.pair_file == NULL ? 0.0 : args.pair_bonus;
  out->model.reset(new DfsScoreModel(
      args.segment_penalty, reader->count(), args.word_bonus,
      pair_bonus, best_bonus));
  if (!args.solo_words.empty() &&
      (args.word_bonus != 0.0 || pair_bonus != 0.0 ||
       !out->weighted_pairs.empty()))
    out->solo_words.reset(new DfsSoloWords(
        reader, args.solo_words,
        args.pair_file != NULL ? &out->pairs : NULL, out->model.get(),
        !out->weighted_pairs.empty() ? &out->weighted_pairs : NULL));
  return true;
}

bool prepare_dfs_class_list(
    IndexReader* reader, std::string const& letters,
    DfsCommonArgs const& args,
    std::vector<std::string> const& exclude_pair_files,
    size_t exact_segments, DfsPreparedClassList* out) {
  DfsDictionary const* dictionary_filter = NULL;
  if (args.dictionary_file != NULL) {
    if (!load_dictionary(args.dictionary_file, &out->dictionary)) return false;
    dictionary_filter = &out->dictionary;
  }

  if (!prepare_dfs_scoring_inputs(
          reader, args, /*score_mode=*/false, exact_segments, out))
    return false;
  if (!load_exclude_pair_files(
          exclude_pair_files, &out->exclude_pairs))
    return false;
  if (dictionary_filter != NULL) {
    if (!args.workflow_root.empty())
      admit_best_words(out->best_rows, &out->dictionary);
    out->external_rows.insert(
        out->external_rows.end(),
        out->best_rows.begin(), out->best_rows.end());
    warn_dictionary_drops(*reader, letters, args, out->dictionary,
                          out->exclude_pairs, out->external_rows);
  }

  out->classes.reset(new DfsClassList(
      reader, letters, args.min_word_len, /*include_phrases=*/true,
      dictionary_filter, args.max_extract_words, out->model.get(),
      args.pair_file != NULL ? &out->pairs : NULL,
      !out->weighted_pairs.empty() ? &out->weighted_pairs : NULL,
      !out->exception_prefixes.empty() ? &out->exception_prefixes : NULL,
      out->solo_words.get(),
      !exclude_pair_files.empty() ? &out->exclude_pairs : NULL,
      DFS_EXTERNAL_PAIRS_SYNTHESIZE_MISSING));
  dfs_diagnostic(
      "phase 1 external pairs: %zu synthetic entries\n",
      out->classes->synthetic_external_pair_count());
  dfs_diagnostic(
      "phase 1 complete: %zu entries, %zu classes, %lld trie nodes\n",
      out->classes->entry_count(), out->classes->classes().size(),
      (long long) out->classes->nodes_visited());
  if (out->solo_words != NULL)
    dfs_diagnostic(
        "solo words: %zu profiles, %zu word edges, %zu pair edges\n",
        out->solo_words->profile_count(), out->solo_words->word_edge_count(),
        out->solo_words->pair_edge_count());
  fflush(stderr);
  return true;
}
