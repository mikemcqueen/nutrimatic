#ifndef NUTRIMATIC_DFS_CLASS_LIST_BUILD_H
#define NUTRIMATIC_DFS_CLASS_LIST_BUILD_H

#include <memory>
#include <string>
#include <vector>

#include "dfs-class-list.h"
#include "dfs-cli-args.h"
#include "dfs-score.h"
#include "dfs-solo-words.h"

class IndexReader;

// Owns every object whose address may be retained by the phase-1 and phase-3
// scoring paths. Field order also makes their destruction run from the class
// list back toward its inputs.
struct DfsPreparedClassList {
  DfsDictionary dictionary;
  DfsPairSet pairs;
  DfsPairBonusMap weighted_pairs;
  DfsPairSet exception_prefixes;
  DfsPairSet exclude_pairs;
  // Rows in each pair file's own written order, as extraction mode loads
  // them; the dictionary reports below read these once every pair input is
  // in. Score mode leaves both empty, its loaders being symmetric and
  // keeping no row order.
  std::vector<DfsPairRow> external_rows;
  std::vector<DfsPairRow> best_rows;
  std::unique_ptr<DfsScoreModel> model;
  std::unique_ptr<DfsSoloWords> solo_words;
  std::unique_ptr<DfsClassList> classes;
};

DfsBestBonusPolicy dfs_best_bonus_policy(size_t exact_segments);

// Loads the positive scoring evidence and builds the score model and, when
// solo words are supplied and a bonus can apply, the solo-word context. It
// reads no rejection input: no dictionary and no exclusions, neither of which
// has a consumer outside the class list. exact_segments selects the BEST
// policy as it does below.
//
// score_mode takes the symmetric, minimum-independent path through both pair
// loaders, so a caller that scores entries it already chose neither applies
// the extraction minimum nor keeps a short-word pair in written order only.
// It leaves `classes` null; nothing here walks the trie.
bool prepare_dfs_scoring_inputs(
    IndexReader* reader, DfsCommonArgs const& args, bool score_mode,
    size_t exact_segments, DfsPreparedClassList* out);

// Builds the shared dfs-anagrams-family phase-1 class list, including external
// pair synthesis and exclusions. exact_segments selects descending BEST
// scoring when nonzero and the fixed BEST tier otherwise. With a dictionary in
// workflow mode, every BEST word outside the dictionary is first added to it
// and reported once on stderr, in yellow when stderr is a terminal. Then
// every --pairs, YES, or BEST row that fits letters and would otherwise
// reach phase 1, but has a word outside the dictionary, is reported once on
// stderr as a WARNING, in red when stderr is a terminal.
bool prepare_dfs_class_list(
    IndexReader* reader, std::string const& letters,
    DfsCommonArgs const& args,
    std::vector<std::string> const& exclude_pair_files,
    size_t exact_segments, DfsPreparedClassList* out);

#endif
