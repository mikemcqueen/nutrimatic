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
  std::unique_ptr<DfsScoreModel> model;
  std::unique_ptr<DfsSoloWords> solo_words;
  std::unique_ptr<DfsClassList> classes;
};

DfsBestBonusPolicy dfs_best_bonus_policy(size_t exact_segments);

// Builds dfs-anagrams' phase-1 class list. exact_segments selects descending
// BEST scoring when nonzero and the fixed BEST tier otherwise.
bool prepare_dfs_class_list(
    IndexReader* reader, std::string const& letters,
    DfsCommonArgs const& args,
    std::vector<std::string> const& exclude_pair_files,
    size_t exact_segments, DfsPreparedClassList* out);

#endif
