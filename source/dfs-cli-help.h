#ifndef NUTRIMATIC_DFS_CLI_HELP_H
#define NUTRIMATIC_DFS_CLI_HELP_H

// Help formatting and descriptions shared by dfs-anagrams, query-index and
// rerank-anagrams.
void dfs_help_option(char const* option, char const* format, ...);
void dfs_help_index(bool env_default = false);
void dfs_help_used_letters();
void dfs_help_dictionary();
void dfs_help_min_word_length();
void dfs_help_top(int default_top);
void dfs_help_max_extract_words(int default_max);
void dfs_help_seed_pairs();
void dfs_help_yes_pairs();
void dfs_help_best_pairs();
void dfs_help_tier_bonuses();
void dfs_help_wf();
void dfs_help_target();
void dfs_help_segment_penalty();
void dfs_help_word_bonus();
void dfs_help_pair_bonus();
void dfs_help_hide_solo_words();
void dfs_help_no_score();

#endif
