// Queries aggregate index entries in three modes. By default, prints the
// highest-corpus-frequency words/phrases makeable from a subset of a letter
// bag. --require-completable adds shared phase-2 feasibility filtering.
// --score scores an exact sequence, while --near finds phrases with supplied
// endpoints and at least one complete intervening word.

#include "dfs-class-list.h"
#include "dfs-cli-args.h"
#include "dfs-diagnostic.h"
#include "dfs-score.h"
#include "dfs-solo-words.h"
#include "dfs-search-stats.h"
#include "dfs-search.h"
#include "index.h"
#include "optparse.h"
#include "workflow-paths.h"

#include <assert.h>
#include <ctype.h>
#include <stdio.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

static int const DEFAULT_TOP = 100;

struct Args {
  char const* index_file;
  std::string letters;
  std::string score_sequence;
  std::string near_input;
  std::string near_target;
  DfsCommonArgs common;
  bool words_only;
  bool csv;
  bool require_completable;
  bool score;
  bool near;
  char const* score_incompatible_option;
  char const* near_incompatible_option;
};

static void usage(char const* program) {
  fprintf(stderr,
      "usage: %s [-i INDEX] letters"
      " [--score] [-P|--segment-penalty P] [--word-bonus N]"
      " [--pair-bonus N]"
      " [--solo-words WORD[,WORD...]]"
      " [--hide-solo-words]"
      " [-u used-letters] [--dict PATH] [-m min-word-length] [-n top]"
      " [-x max-extract-words] [--pairs FILE]"
      " [--seed-pairs FILE]... [--yes-pairs FILE]..."
      " [--best-pairs FILE] [--more-best-pairs FILE]..."
      " [--wf|--wfroot DIR] [-t TARGET]"
      " [-w|--words-only] [--csv] [--require-completable]"
      " [-S|--search-threads N]\n"
      "       %s [-i INDEX] sequence --score"
      " [-P|--segment-penalty P] [--word-bonus N]"
      " [--pair-bonus N] [--pairs FILE]"
      " [--seed-pairs FILE]... [--yes-pairs FILE]..."
      " [--best-pairs FILE] [--more-best-pairs FILE]..."
      " [--wf|--wfroot DIR] [-t TARGET]"
      " [--solo-words WORD[,WORD...]]\n"
      "       %s -i INDEX input --near word [-n top]\n"
      "  -i, --idx INDEX reads the completed Nutrimatic index from INDEX;"
      " workflow mode defaults to DIR/%s; required otherwise and with"
      " --near\n"
      "  --score treats letters as a comma-separated sequence of exact index\n"
      "    entries and prints its DFS-model score\n"
      "  --near treats both arguments as literal lowercase a-z0-9 entries;\n"
      "    it prints aggregate phrases spanning the endpoints with at least\n"
      "    one complete intervening word, searching each endpoint that is an\n"
      "    aggregate index entry\n"
      "  -P, --segment-penalty P divides the score by P for each selected"
      " index entry after the first; P must be at least 1 and defaults to"
      " %.0f\n"
      "    k entries score as product(count) / (corpus-total * P)^(k-1)\n"
      "  --word-bonus N multiplies each multi-word index entry by %.0f^N;"
      " defaults to %.1f\n"
      "  --pair-bonus N multiplies each index entry found in --pairs by"
      " %.0f^N; defaults to %.1f\n"
      "  -m defaults to %d; 0 for no minimum\n"
      "  -n defaults to %d; 0 for no limit\n"
      "  --dict PATH filters entries to words in the dictionary; workflow"
      " mode defaults to DIR/%s\n"
      "  -x, --max-extract-words N explores at most N words inside one index"
      " entry; defaults to 0 (no limit)\n"
      "  --pairs FILE loads one \"word\" or \"word,word\" entry per line\n"
      "    during extraction, a pair containing a word shorter than -m is"
      " matched only in written order; other pairs match in either order\n"
      "    every loaded entry must contain at least -m normalized"
      " non-space characters in total\n"
      "    --score instead matches pairs in either order and does not apply"
      " the extraction minimum\n"
      "  --seed-pairs FILE and --yes-pairs FILE load fixed pair-bonus tiers"
      " %.2f and %.2f; --best-pairs FILE and --more-best-pairs FILE mark"
      " BEST entries\n"
      "    --score uses the sequence entry count N and descending BEST"
      " exponents from N through 1, counted once per entry; ordinary listing"
      " keeps the fixed %.2f exponent; each option except --best-pairs may"
      " be repeated\n"
      "    duplicates and reversed pairs retain the strongest tier; these"
      " options cannot be combined with legacy --pairs\n"
      "  --wfroot DIR uses DIR as a workflow root; --wf is an alias using"
      " the nonempty WFROOT environment variable\n"
      "    workflow mode loads DIR/%s as YES pairs and"
      " requires either --seed-pairs or -t beginning with sN\n"
      "  -t, --target TARGET selects a prefix of sN/[ou]-letters/mN/gN;"
      " its sentence seed is auto-loaded, and a complete target also loads"
      " its optional %s unless --best-pairs replaces it\n"
      "  --solo-words WORD[,WORD...] supplies up to 16 unique lowercase"
      " external words; they consume no letters and matched partners are"
      " printed in parentheses\n"
      "    a selected single-word entry earns --word-bonus when either"
      " phrase order is an aggregate index phrase or is asserted by"
      " a pair input; an asserted pair also earns its source's pair bonus\n"
      "    each solo word can be used once per row or --score sequence; both"
      " bonuses must be non-negative, and the aggregate phrase test matches"
      " phase 1\n"
      "  --hide-solo-words omits parenthesized solo partners from ordinary"
      " output\n"
      "  -w, --words-only excludes multi-word phrases\n"
      "  --csv prints only multi-word entries, as their comma-separated"
      " words, with no count or score column\n"
      "  --require-completable drops classes whose removal leaves a\n"
      "    remainder phase 2 can't fully turn into an anagram (subject to\n"
      "    -m), using shared exact validation without a score cache\n"
      "  -S, --search-threads defaults to 1; 0 uses hardware threads\n",
      program, program, program, WORKFLOW_INDEX_PATH,
      DFS_DEFAULT_SEGMENT_PENALTY,
      DFS_WORD_BONUS_BASE, DFS_DEFAULT_WORD_BONUS,
      DFS_PAIR_BONUS_BASE, DFS_DEFAULT_PAIR_BONUS,
      DFS_DEFAULT_MIN_WORD_LEN, DEFAULT_TOP, WORKFLOW_DICT_PATH,
      DFS_SEED_PAIR_BONUS, DFS_YES_PAIR_BONUS, DFS_BEST_PAIR_BONUS,
      WORKFLOW_YES_PAIRS_PATH, WORKFLOW_TARGET_BEST_PAIRS_NAME);
}

static int const OPT_REQUIRE_COMPLETABLE = 256;
static int const OPT_SCORE = 257;
static int const OPT_CSV = 258;
static int const OPT_NEAR = 259;

static struct optparse_long const long_options[] = {
  DFS_COMMON_LONG_OPTIONS,
  { "idx", 'i', OPTPARSE_REQUIRED },
  { "words-only", 'w', OPTPARSE_NONE },
  { "csv", OPT_CSV, OPTPARSE_NONE },
  { "score", OPT_SCORE, OPTPARSE_NONE },
  { "near", OPT_NEAR, OPTPARSE_REQUIRED },
  { "require-completable", OPT_REQUIRE_COMPLETABLE, OPTPARSE_NONE },
  { NULL, 0, OPTPARSE_NONE },
};

static void mark_score_incompatible(Args* args, char const* option) {
  if (args->score_incompatible_option == NULL)
    args->score_incompatible_option = option;
}

static void mark_near_incompatible(Args* args, char const* option) {
  if (args->near_incompatible_option == NULL)
    args->near_incompatible_option = option;
}

static bool validate_literal_entry(std::string const& entry) {
  for (size_t i = 0; i < entry.size(); ++i) {
    char const ch = entry[i];
    bool const letter_or_digit =
        (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
    bool const single_internal_space =
        ch == ' ' && i != 0 && i + 1 != entry.size() &&
        entry[i - 1] != ' ';
    if (!letter_or_digit && !single_internal_space) {
      fprintf(stderr, "error: malformed index entry \"%s\"\n",
              entry.c_str());
      return false;
    }
  }
  if (entry.empty()) {
    fputs("error: malformed index entry \"\"\n", stderr);
    return false;
  }
  return true;
}

static bool parse_args(char* argv[], Args* out) {
  out->index_file = NULL;
  out->common = DfsCommonArgs();
  out->common.top = DEFAULT_TOP;
  out->words_only = false;
  out->csv = false;
  out->require_completable = false;
  out->score = false;
  out->near = false;
  out->score_incompatible_option = NULL;
  out->near_incompatible_option = NULL;

  struct optparse options;
  optparse_init(&options, argv);

  int opt;
  while ((opt = optparse_long(&options, long_options, NULL)) != -1) {
    DfsCommonOption which;
    switch (dfs_parse_common_option(opt, &options, &out->common, &which)) {
      case DFS_OPTION_ERROR:
        return false;
      case DFS_OPTION_HANDLED:
        if (which.score_incompatible)
          mark_score_incompatible(out, which.name);
        if (opt != 'n') mark_near_incompatible(out, which.name);
        continue;
      case DFS_OPTION_OTHER:
        break;
    }
    switch (opt) {
      case 'i':
        out->index_file = options.optarg;
        break;
      case 'w':
        out->words_only = true;
        mark_score_incompatible(out, "--words-only");
        mark_near_incompatible(out, "--words-only");
        break;
      case OPT_CSV:
        out->csv = true;
        mark_score_incompatible(out, "--csv");
        mark_near_incompatible(out, "--csv");
        break;
      case OPT_SCORE:
        out->score = true;
        mark_near_incompatible(out, "--score");
        break;
      case OPT_NEAR:
        out->near = true;
        out->near_target = options.optarg;
        mark_score_incompatible(out, "--near");
        break;
      case OPT_REQUIRE_COMPLETABLE:
        out->require_completable = true;
        mark_score_incompatible(out, "--require-completable");
        mark_near_incompatible(out, "--require-completable");
        break;
      default:
        fprintf(stderr, "error: %s\n", options.errmsg);
        usage(argv[0]);
        return false;
    }
  }
  char const* letters = optparse_arg(&options);
  if (letters == NULL || optparse_arg(&options) != NULL) {
    usage(argv[0]);
    return false;
  }
  if (out->near) {
    if (out->near_incompatible_option != NULL) {
      fprintf(stderr, "error: %s cannot be used with --near\n",
              out->near_incompatible_option);
      return false;
    }
    if (out->index_file == NULL) {
      usage(argv[0]);
      return false;
    }
    out->near_input = letters;
    return validate_literal_entry(out->near_input) &&
        validate_literal_entry(out->near_target);
  }

  if (!finalize_dfs_workflow_args(
          &out->common, argv[0], &out->index_file))
    return false;
  if (out->index_file == NULL) {
    usage(argv[0]);
    return false;
  }

  if (!validate_solo_bonuses(out->common)) return false;

  if (out->score) {
    if (out->score_incompatible_option != NULL) {
      fprintf(stderr, "error: %s cannot be used with --score\n",
              out->score_incompatible_option);
      return false;
    }
    out->score_sequence = letters;
    return true;
  }

  if (out->csv && out->words_only) {
    fputs("error: --csv cannot be used with --words-only\n", stderr);
    return false;
  }

  std::string bag;
  std::string remove;
  if (!clean_letters(letters, "letters", &bag)) return false;
  if (!clean_letters(out->common.used_letters.c_str(), "used letters", &remove))
    return false;
  if (!subtract_letters(bag, remove, &out->letters)) return false;
  if (!check_bag_length(out->letters)) return false;
  return finalize_min_word_length(
      out->letters, out->common.min_word_len_given, &out->common.min_word_len);
}

// With both bonuses at zero every group collapses to log(count) descending,
// which is integer count descending: over this corpus's count range log is
// injective in double, so an integer compare reproduces the score order
// exactly, ties included, and the text tie-break still decides them.
static bool count_order(DfsPackedMember const& a, DfsPackedMember const& b) {
  if (a.count != b.count) return a.count > b.count;
  return dfs_member_text_compare(a, b) < 0;
}

static bool is_phrase(DfsPackedMember const& member) {
  return member.word_count > 1;
}

// Rewriting this as count * exp(bonus) versus count would round differently
// from log(count) + bonus, breaking exact ties differently and so changing the
// text tie-break. It stays the identical expression, applied O(E) times in the
// merge instead of O(E log E) times in a sort.
struct ScoreOrder {
  DfsScoreModel const* model;

  bool operator()(DfsPackedMember const& a, DfsPackedMember const& b) const {
    double const a_score =
        model->member_upper_log_score(
            a.count, is_phrase(a), a.score_flags);
    double const b_score =
        model->member_upper_log_score(
            b.count, is_phrase(b), b.score_flags);
    if (a_score != b_score) return a_score > b_score;
    return dfs_member_text_compare(a, b) < 0;
  }
};

static bool parse_score_sequence(
    std::string const& sequence, std::vector<std::string>* entries) {
  size_t start = 0;
  for (;;) {
    size_t const comma = sequence.find(',', start);
    size_t first = start;
    size_t last = comma == std::string::npos ? sequence.size() : comma;
    while (first < last && isspace((unsigned char) sequence[first])) ++first;
    while (last > first && isspace((unsigned char) sequence[last - 1])) --last;

    std::string const entry = sequence.substr(first, last - first);
    if (entry.empty()) {
      fputs("error: empty entry in --score sequence\n", stderr);
      return false;
    }
    if (!validate_literal_entry(entry)) return false;
    entries->push_back(entry);
    if (comma == std::string::npos) return true;
    start = comma + 1;
  }
}

struct NearResult {
  int64_t count;
  std::string phrase;
};

static bool near_result_better(NearResult const& a, NearResult const& b) {
  if (a.count != b.count) return a.count > b.count;
  return a.phrase < b.phrase;
}

struct NearHeapOrder {
  bool operator()(NearResult const& a, NearResult const& b) const {
    return near_result_better(a, b);
  }
};

class NearResults {
 public:
  explicit NearResults(size_t top) : top_(top) {}

  bool can_contain(int64_t count) const {
    return top_ == 0 || rows_.size() < top_ ||
        count >= rows_.front().count;
  }

  void add(int64_t count, std::string phrase) {
    NearResult result = {count, phrase};
    if (top_ == 0) {
      rows_.push_back(result);
      return;
    }
    NearHeapOrder const order;
    if (rows_.size() < top_) {
      rows_.push_back(result);
      std::push_heap(rows_.begin(), rows_.end(), order);
    } else if (near_result_better(result, rows_.front())) {
      std::pop_heap(rows_.begin(), rows_.end(), order);
      rows_.back() = result;
      std::push_heap(rows_.begin(), rows_.end(), order);
    }
  }

  std::vector<NearResult> finish() {
    std::vector<NearResult> result;
    result.swap(rows_);
    std::sort(result.begin(), result.end(), near_result_better);
    return result;
  }

 private:
  size_t top_;
  std::vector<NearResult> rows_;
};

static bool near_choice_better(
    IndexReader::Choice const& a, IndexReader::Choice const& b) {
  if (a.count != b.count) return a.count > b.count;
  return (unsigned char) a.ch < (unsigned char) b.ch;
}

static void walk_near_direction(
    IndexReader const& reader, IndexReader::EntryPosition const& position,
    std::string const& anchor, std::string const& target,
    std::string* path, NearResults* results) {
  if (!results->can_contain(position.aggregate_count)) return;

  IndexReader::CharSet allowed;
  allowed.fill();
  std::vector<IndexReader::Choice> choices;
  reader.children(position.continuation, position.aggregate_count,
                  allowed, &choices);
  std::sort(choices.begin(), choices.end(), near_choice_better);

  for (size_t i = 0; i < choices.size(); ++i) {
    IndexReader::Choice const& choice = choices[i];
    if (!results->can_contain(choice.count)) break;

    size_t const old_size = path->size();
    path->push_back(choice.ch);
    IndexReader::EntryPosition const child = {choice.next, choice.count};
    if (choice.ch == ' ') {
      IndexReader::EntryPosition match;
      if (reader.continuation_entry_position(child, target, &match))
        results->add(match.aggregate_count,
                     anchor + " " + *path + target);
    }
    walk_near_direction(reader, child, anchor, target, path, results);
    path->resize(old_size);
  }
}

static std::vector<NearResult> find_near_direction(
    IndexReader const& reader, IndexReader::EntryPosition const& anchor_position,
    std::string const& anchor, std::string const& target, size_t top) {
  NearResults results(top);
  std::string path;
  walk_near_direction(
      reader, anchor_position, anchor, target, &path, &results);
  return results.finish();
}

static bool same_near_phrase(NearResult const& a, NearResult const& b) {
  return a.phrase == b.phrase;
}

static bool near_phrase_order(NearResult const& a, NearResult const& b) {
  if (a.phrase != b.phrase) return a.phrase < b.phrase;
  return a.count > b.count;
}

static int run_near_query(IndexReader const& reader, Args const& args) {
  IndexReader::EntryPosition input_position;
  IndexReader::EntryPosition target_position;
  bool const input_present = reader.aggregate_entry_position(
      args.near_input, &input_position);
  bool const target_present = reader.aggregate_entry_position(
      args.near_target, &target_position);
  if (!input_present && !target_present) {
    fprintf(stderr,
            "error: index has neither near anchor \"%s\" nor \"%s\"\n",
            args.near_input.c_str(), args.near_target.c_str());
    return 2;
  }

  size_t const top = size_t(args.common.top);
  std::vector<NearResult> rows;
  if (input_present) {
    std::vector<NearResult> direction = find_near_direction(
        reader, input_position, args.near_input, args.near_target, top);
    rows.insert(rows.end(), direction.begin(), direction.end());
  }
  if (target_present && args.near_target != args.near_input) {
    std::vector<NearResult> direction = find_near_direction(
        reader, target_position, args.near_target, args.near_input, top);
    rows.insert(rows.end(), direction.begin(), direction.end());
  }

  std::sort(rows.begin(), rows.end(), near_phrase_order);
  rows.erase(std::unique(rows.begin(), rows.end(), same_near_phrase),
             rows.end());
  std::sort(rows.begin(), rows.end(), near_result_better);
  if (top != 0 && rows.size() > top) rows.resize(top);
  for (size_t i = 0; i < rows.size(); ++i)
    printf("%lld %s\n", (long long) rows[i].count, rows[i].phrase.c_str());
  return 0;
}

static bool print_sequence_score(
    IndexReader const& reader, Args const& args,
    std::vector<std::string> const& entries, DfsPairSet const& pairs,
    DfsPairBonusMap const& weighted_pairs,
    DfsScoreModel const& model, DfsSoloWords const* solo_words) {
  std::vector<int64_t> counts;
  counts.reserve(entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    int64_t count;
    // Aggregate, not exact: phase 1 scores an entry by the count on its
    // trailing-space node, which includes every longer phrase continuing it.
    // Using the exact residual here would silently disagree with the score
    // dfs-anagrams prints for the same sequence.
    if (!reader.aggregate_entry_count(entries[i], &count)) {
      fprintf(stderr, "error: index has no entry \"%s\"\n",
              entries[i].c_str());
      return false;
    }
    counts.push_back(count);
  }

  // An interior space is exactly what makes an entry multi-word; entries here
  // are the user's own text, already validated against the index above.
  std::vector<bool> multi_word;
  std::vector<uint16_t> score_flags;
  std::vector<DfsSoloMasks> profiles;
  std::vector<bool> profile_direct_best;
  size_t direct_best_segments = 0;
  multi_word.reserve(entries.size());
  score_flags.reserve(entries.size());
  profiles.reserve(entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    bool const phrase = entries[i].find(' ') != std::string::npos;
    multi_word.push_back(phrase);
    uint16_t flags = 0;
    DfsPairBonusMap::const_iterator const weighted =
        weighted_pairs.find(entries[i]);
    if (weighted != weighted_pairs.end())
      flags |= dfs_pair_bonus_score_flags(weighted->second);
    else if (pairs.count(entries[i]) != 0)
      flags |= dfs_pair_bonus_score_flags(DFS_PAIR_BONUS_LEGACY);
    bool const direct_best =
        dfs_member_pair_bonus_kind(flags) == DFS_PAIR_BONUS_BEST;
    if (direct_best) ++direct_best_segments;
    if (!phrase && solo_words != NULL) {
      DfsSoloMasks const profile = solo_words->resolve(entries[i]);
      flags |= dfs_solo_score_flags(profile);
      if (profile.word_mask != 0) {
        profiles.push_back(profile);
        profile_direct_best.push_back(direct_best);
      }
    }
    score_flags.push_back(flags);
  }

  double upper_log_score = model.member_upper_log_score(
      counts[0], multi_word[0], score_flags[0]);
  for (size_t i = 1; i < entries.size(); ++i)
    upper_log_score = model.append_log_score(
        upper_log_score, model.member_upper_log_score(
            counts[i], multi_word[i], score_flags[i]));
  DfsExactResultMatching const exact = dfs_exact_result_matching(
      profiles, profile_direct_best, direct_best_segments, model);
  double const log_score = upper_log_score + exact.correction;
  assert(log_score <= upper_log_score);

  printf("%#.4g %s\n", model.displayed_score(log_score),
         args.score_sequence.c_str());
  return true;
}

int main(int argc, char* argv[]) {
  dfs_reset_diagnostic_clock();
  dfs_set_diagnostic_stream(stderr);

  Args args;
  if (!parse_args(argv, &args)) return 2;

  std::vector<std::string> score_entries;
  if (args.score &&
      !parse_score_sequence(args.score_sequence, &score_entries))
    return 2;

  FILE* fp = fopen(args.index_file, "rb");
  if (fp == NULL) {
    fprintf(stderr, "error: can't open \"%s\"\n", args.index_file);
    return 1;
  }
  IndexReader reader(fp);

  if (args.near) return run_near_query(reader, args);

  DfsPairSet pairs;
  DfsPairBonusMap weighted_pairs;
  DfsPairSet exception_prefixes;
  if (args.common.pair_file != NULL) {
    bool const loaded = args.score
        ? load_pair_file(
              args.common.pair_file, "pair list", &pairs,
              false, false, true)
        : load_extraction_pair_file(
              args.common.pair_file, "pair list", args.common.min_word_len,
              &pairs, &exception_prefixes, false, false);
    if (!loaded) return 1;
  }
  if (!load_weighted_pair_files(
          args.common, args.score, args.common.min_word_len,
          &weighted_pairs, &exception_prefixes))
    return 1;
  if (args.common.pair_file == NULL) args.common.pair_bonus = 0.0;

  if (args.score) {
    DfsScoreModel const model(
        args.common.segment_penalty, reader.count(), args.common.word_bonus,
        args.common.pair_bonus,
        DfsBestBonusPolicy::descending(score_entries.size()));
    std::unique_ptr<DfsSoloWords> solo_words;
    if (!args.common.solo_words.empty() &&
        (args.common.word_bonus != 0.0 || args.common.pair_bonus != 0.0 ||
         !weighted_pairs.empty()))
      solo_words.reset(new DfsSoloWords(
          &reader, args.common.solo_words,
          args.common.pair_file != NULL ? &pairs : NULL, &model,
          !weighted_pairs.empty() ? &weighted_pairs : NULL));
    return print_sequence_score(
        reader, args, score_entries, pairs, weighted_pairs,
        model, solo_words.get()) ? 0 : 2;
  }

  DfsDictionary dictionary;
  DfsDictionary const* dictionary_filter = NULL;
  if (args.common.dictionary_file != NULL) {
    if (!load_dictionary(args.common.dictionary_file, &dictionary)) return 1;
    dictionary_filter = &dictionary;
  }

  DfsScoreModel const model(
      args.common.segment_penalty, reader.count(), args.common.word_bonus,
      args.common.pair_bonus);
  std::unique_ptr<DfsSoloWords> solo_words;
  if (!args.common.solo_words.empty() &&
      (args.common.word_bonus != 0.0 || args.common.pair_bonus != 0.0 ||
       !weighted_pairs.empty()))
    solo_words.reset(new DfsSoloWords(
        &reader, args.common.solo_words,
        args.common.pair_file != NULL ? &pairs : NULL, &model,
        !weighted_pairs.empty() ? &weighted_pairs : NULL));
  bool const include_phrases =
      args.require_completable || !args.words_only;
  DfsClassList classes(&reader, args.letters, args.common.min_word_len,
                       include_phrases, dictionary_filter,
                       args.common.max_extract_words,
                       &model,
                       args.common.pair_file != NULL ? &pairs : NULL,
                       !weighted_pairs.empty() ? &weighted_pairs : NULL,
                       !exception_prefixes.empty()
                           ? &exception_prefixes : NULL,
                       solo_words.get());
  dfs_diagnostic(
      "phase 1 complete: %zu entries, %zu classes, %lld trie nodes\n",
      classes.entry_count(), classes.classes().size(),
      (long long) classes.nodes_visited());
  if (solo_words != NULL)
    dfs_diagnostic(
        "solo words: %zu profiles, %zu word edges, %zu pair edges\n",
        solo_words->profile_count(), solo_words->word_edge_count(),
        solo_words->pair_edge_count());

  std::vector<bool> completable(classes.classes().size(), true);
  if (args.require_completable) {
    size_t const search_threads = resolve_search_threads(
        args.common.search_threads);
    dfs_diagnostic(
        "search threads %zu cache 0 segment penalty %.17g\n",
        search_threads, args.common.segment_penalty);
    DfsAnagramSearch search(
        &classes, args.letters, args.common.segment_penalty, reader.count(),
        /*score_cache_bytes=*/0, /*preprocess_threads=*/1,
        search_threads, /*exact_segments=*/0,
        args.common.word_bonus, args.common.pair_bonus);
    DfsSearchStats stats;
    if (!search.find_completable_classes(
            &completable, &stats, /*progress_factor=*/1,
            /*allow_cache_fallback=*/true, /*exact_letters=*/-1))
      return 2;
    DfsSearchStats::Execution const& run = stats.execution;
    DfsSearchStats::Bounds const& bounds = stats.bounds;
    if (run.search_threads > 1)
      dfs_diagnostic(
          "phase 2 exact validation parallelism: "
          "%zu requested, %zu used\n",
          search_threads, run.search_threads);
    dfs_diagnostic(
        "phase 2 timing: %.1fs setup, %.1fs exact validation\n",
        run.setup_seconds, run.search_seconds);
    dfs_diagnostic(
        "phase 2 score cache: %zu bound entries, %zu bound bytes\n",
        bounds.entries, bounds.bytes_charged);
  }

  // The class -> member grouping has no reader left: phase 2 touched it once at
  // setup and its search is already destroyed, and printing needs only each
  // member's count and text.
  DfsMemberFilter const filter = args.words_only
      ? DFS_RETAIN_WORDS
      : (args.csv ? DFS_RETAIN_PHRASES : DFS_RETAIN_ALL);
  DfsMemberSpan const survivors =
      classes.retain_members(completable, filter);
  size_t const top = args.common.top == 0
      ? survivors.count
      : std::min(survivors.count, size_t(args.common.top));

  DfsPackedMember* const first = survivors.data;
  DfsPackedMember* const last = first + survivors.count;
  auto const print_row = [&](DfsPackedMember const& row) {
    char const* partner = NULL;
    if (!args.common.hide_solo_words && solo_words != NULL &&
        row.word_count == 1 &&
        (row.score_flags & DFS_MEMBER_SOLO_WORD_EDGE) != 0) {
      std::vector<DfsSoloMasks> profiles(
          1, solo_words->lookup(
                 std::string_view(row.text, row.text_length)));
      DfsSoloMatching const matching = dfs_solo_exact_matching(
          profiles, model);
      if (matching.solo_word_indexes[0] != DFS_NO_SOLO_WORD)
        partner = solo_words->word(matching.solo_word_indexes[0]).c_str();
    }
    if (args.csv) {
      for (size_t i = 0; i < row.text_length; ++i)
        putchar(row.text[i] == ' ' ? ',' : row.text[i]);
      putchar('\n');
    } else if (args.common.word_bonus == 0.0 &&
               args.common.pair_bonus == 0.0 && weighted_pairs.empty())
      printf("%lld %.*s%s%s%s\n", (long long) row.count,
             int(row.text_length), row.text,
             partner != NULL ? " (" : "", partner != NULL ? partner : "",
             partner != NULL ? ")" : "");
    else
      printf("%#.4g %.*s%s%s%s\n",
             model.displayed_score(model.member_upper_log_score(
                 row.count, is_phrase(row), row.score_flags)),
             int(row.text_length), row.text,
             partner != NULL ? " (" : "", partner != NULL ? partner : "",
             partner != NULL ? ")" : "");
  };

  if (args.common.word_bonus == 0.0 && args.common.pair_bonus == 0.0 &&
      weighted_pairs.empty()) {
    std::partial_sort(first, first + top, last, count_order);
    for (size_t i = 0; i < top; ++i) print_row(first[i]);
  } else {
    ScoreOrder const order = { &model };
    std::partial_sort(first, first + top, last, order);
    for (size_t i = 0; i < top; ++i) print_row(first[i]);
  }
  return 0;
}
