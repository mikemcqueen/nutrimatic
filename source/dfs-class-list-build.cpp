#include "dfs-class-list-build.h"

#include <stdio.h>

#include "dfs-diagnostic.h"
#include "index.h"

DfsBestBonusPolicy dfs_best_bonus_policy(size_t exact_segments) {
  return exact_segments > 0
      ? DfsBestBonusPolicy::descending(exact_segments)
      : DfsBestBonusPolicy::fixed(DFS_BEST_PAIR_BONUS);
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

  if (args.pair_file != NULL &&
      !load_extraction_pair_file(
          args.pair_file, "pair list", args.min_word_len,
          &out->pairs, &out->exception_prefixes, false, false))
    return false;
  if (!load_weighted_pair_files(
          args, /*score_mode=*/false, args.min_word_len,
          &out->weighted_pairs, &out->exception_prefixes))
    return false;
  if (!load_exclude_pair_files(
          exclude_pair_files, &out->exclude_pairs))
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
