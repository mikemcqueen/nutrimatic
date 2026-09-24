// Queries aggregate index entries in three modes. By default, prints the
// highest-corpus-frequency words/phrases makeable from a subset of a letter
// bag. --require-completable adds shared phase-2 feasibility filtering.
// --score scores an exact query, while --near finds phrases with supplied
// endpoints and at least one complete intervening word.

#include "dfs-class-list.h"
#include "dfs-class-list-build.h"
#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "dfs-diagnostic.h"
#include "dfs-score.h"
#include "dfs-solo-words.h"
#include "dfs-search-stats.h"
#include "dfs-search.h"
#include "index.h"
#include "log.h"
#include "optparse.h"
#include "workflow-paths.h"
#include "tail-map.h"

#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

static int const DEFAULT_TOP = 100;

struct Args {
  char const* index_file;
  std::string letters;
  std::string score_sequence;
  std::string near_input;
  std::string near_target;
  DfsCommonArgs common;
  std::vector<std::string> workflow_reject_files;
  int num_words;
  int min_words;
  bool min_words_given;
  bool require_completable;
  bool score;
  bool near;
  bool sd;
  bool ptm;
  char const* score_incompatible_option;
  char const* near_incompatible_option;
};

static void usage(char const* program, FILE* out) {
  fprintf(out,
      "usage: %s [-i INDEX] [options] letters\n"
      "       %s [-i INDEX] [options] query|- --score\n"
      "       %s [-i INDEX] input --near WORD [-n N]\n",
      program, program, program);
  if (out != stdout) return;

  fputs("\noptions:\n", stdout);
  dfs_help_index(true);
  dfs_help_used_letters();
  dfs_help_dictionary();
  dfs_help_min_word_length();
  dfs_help_top(DEFAULT_TOP);
  dfs_help_max_extract_words(0);
  dfs_help_option("-w, --num-words N",
      "show only entries containing exactly N words, walking at most N "
      "words per entry unless --require-completable is given (N must be "
      "positive; cannot be combined with --min-words or --max-words)");
  dfs_help_option("--min-words N",
      "show entries containing at least N words (default: 0; N cannot "
      "exceed a positive -x limit)");
  dfs_help_option("--pairs FILE",
      "load one \"word\" or \"word,word\" entry per line; during extraction, "
      "a pair containing a word shorter than -m matches only in written "
      "order, while other pairs match either way; each entry needs at least "
      "-m normalized non-space characters; listing admits eligible missing "
      "pairs with corpus count 1 and still applies dictionary, bag, -m, -x, "
      "and workflow NO rules; --score matches pairs in either order without "
      "the extraction minimum or filtering");
  dfs_help_seed_pairs();
  dfs_help_yes_pairs();
  dfs_help_best_pairs();
  dfs_help_option("--more-best-pairs FILE",
      "mark additional BEST entries; may be repeated; --score treats each "
      "query as one index entry and uses the final BEST exponent; listing "
      "keeps the fixed %.2f exponent; duplicate and "
      "reversed pairs retain the strongest tier; weighted pair options "
      "cannot be combined with legacy --pairs", DFS_BEST_PAIR_BONUS);
  dfs_help_wf();
  dfs_help_option("--wfroot DIR",
      "use DIR as a workflow root; with -t loads DIR/%s as YES pairs; "
      "without -t warns and uses no classified YES or sentence seed; "
      "listing also excludes "
      "DIR/%s and a complete target's %s when present; --score reads neither "
      "NO file; BEST pair words missing from the dictionary are added to it, "
      "with a stderr notice for each",
      WORKFLOW_YES_PAIRS_PATH, WORKFLOW_NO_PAIRS_PATH,
      WORKFLOW_TARGET_NO_PAIRS_NAME);
  dfs_help_target();
  dfs_help_option("--solo-words WORD[,WORD...]",
      "supply up to 16 unique lowercase external words; they consume no "
      "letters and matched partners are printed in parentheses; a selected "
      "single-word entry earns --word-bonus when either phrase order is an "
      "aggregate index phrase or is asserted by a pair input, and an "
      "asserted pair also earns its source's pair bonus; each solo word can "
      "be used once per row or --score query; both bonuses must be "
      "non-negative, and the aggregate phrase test matches phase 1");
  dfs_help_option("--hide-solo-words",
      "omit parenthesized solo partners from ordinary output");
  dfs_help_segment_penalty();
  dfs_help_word_bonus();
  dfs_help_pair_bonus();
  dfs_help_option("--csv",
      "omit the leading count or score from each result and print an "
      "entry's words comma-separated; --score prints each query as given; "
      "cannot be combined with --sd");
  dfs_help_option("--require-completable",
      "drop classes whose removal leaves a remainder phase 2 cannot fully "
      "turn into an anagram (subject to -m), using shared exact validation "
      "without a score cache");
  dfs_help_option("-S, --search-threads N",
      "set search threads (default: 1; 0 uses hardware threads)");
  dfs_help_option("-s, --score",
      "treat a positional query without a comma as one exact index entry; "
      "with one comma, score it like a pair read from stdin by trying both "
      "word orderings and printing the higher score, or print nothing if "
      "neither is in the index; more than one comma is invalid; no "
      "dictionary or exclusion filtering applies; query - "
      "reads one comma-separated value per stdin line, each as one "
      "space-separated index entry, and sorts by score; a two-word value "
      "uses its higher-scoring orientation and is omitted if neither is in "
      "the index");
  dfs_help_option("--sd",
      "with stdin --score, print the geometric mean first, in score units, "
      "with one standard deviation as a multiplicative factor, then add "
      "each value's distance from the mean in those deviations; any bonus "
      "withholds the whole summary; requires --score");
  dfs_help_option("--no-ptm",
      "turn off ptm, which is on by default and applies only to stdin "
      "--score; ptm adds mapped-deviation and corresponding score columns, "
      "with a normal rather than exponential upper tail; the top percent of "
      "log scores uses a fitted exponential tail and the rest use their "
      "ranks; it is withheld by any bonus, needs three finite scores and one "
      "below the tail, and adds the fitted tail rate to the --sd summary "
      "line");
  dfs_help_option("--near WORD",
      "treat input and WORD as literal lowercase a-z0-9 entries; print "
      "aggregate phrases spanning the endpoints with at least one complete "
      "intervening word, searching each endpoint that is an aggregate "
      "index entry");
  dfs_help_option("-h, --help", "show this help");
}

static void put_csv(char const* text, size_t length) {
  for (size_t i = 0; i < length; ++i)
    putchar(text[i] == ' ' ? ',' : text[i]);
  putchar('\n');
}

static int const OPT_REQUIRE_COMPLETABLE = 256;
static int const OPT_NEAR = 259;
static int const OPT_NO_PTM = 260;
static int const OPT_MIN_WORDS = 261;
static int const OPT_SD = 262;

static struct optparse_long const long_options[] = {
  DFS_COMMON_LONG_OPTIONS,
  { "csv", DFS_OPT_NO_SCORE, OPTPARSE_NONE },
  { "idx", 'i', OPTPARSE_REQUIRED },
  { "num-words", 'w', OPTPARSE_REQUIRED },
  { "min-words", OPT_MIN_WORDS, OPTPARSE_REQUIRED },
  { "score", 's', OPTPARSE_NONE },
  { "sd", OPT_SD, OPTPARSE_NONE },
  { "no-ptm", OPT_NO_PTM, OPTPARSE_NONE },
  { "near", OPT_NEAR, OPTPARSE_REQUIRED },
  { "require-completable", OPT_REQUIRE_COMPLETABLE, OPTPARSE_NONE },
  { "help", 'h', OPTPARSE_NONE },
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

static bool index_from_env(char const* program, char const* missing_hint,
                           char const** index_file) {
  char const* idx = getenv("IDX");
  if (idx == NULL || *idx == '\0') {
    fprintf(stderr, "error: missing index; use %s or set IDX\n", missing_hint);
    usage(program, stderr);
    return false;
  }
  std::error_code ec;
  if (!std::filesystem::is_regular_file(idx, ec)) {
    fprintf(stderr, "error: IDX \"%s\" is not a file\n", idx);
    return false;
  }
  *index_file = idx;
  return true;
}

static bool parse_args(char* argv[], Args* out, bool* help) {
  out->index_file = NULL;
  out->common = DfsCommonArgs();
  out->common.top = DEFAULT_TOP;
  out->num_words = 0;
  out->min_words = 0;
  out->min_words_given = false;
  out->require_completable = false;
  out->score = false;
  out->near = false;
  out->sd = false;
  out->ptm = true;
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
        if (opt != 'n' && opt != DFS_OPT_NO_SCORE)
          mark_near_incompatible(out, which.name);
        continue;
      case DFS_OPTION_OTHER:
        break;
    }
    switch (opt) {
      case 'i':
        out->index_file = options.optarg;
        break;
      case 'w':
        if (!parse_count(options.optarg, "--num-words", &out->num_words))
          return false;
        if (out->num_words == 0) {
          fputs("error: --num-words must be positive\n", stderr);
          return false;
        }
        mark_score_incompatible(out, "--num-words");
        mark_near_incompatible(out, "--num-words");
        break;
      case OPT_MIN_WORDS:
        if (!parse_count(options.optarg, "--min-words", &out->min_words))
          return false;
        out->min_words_given = true;
        mark_score_incompatible(out, "--min-words");
        mark_near_incompatible(out, "--min-words");
        break;
      case 's':
        out->score = true;
        mark_near_incompatible(out, "--score");
        break;
      case OPT_SD:
        out->sd = true;
        mark_near_incompatible(out, "--sd");
        break;
      case OPT_NO_PTM:
        out->ptm = false;
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
      case 'h':
        *help = true;
        return true;
      default:
        fprintf(stderr, "error: %s\n", options.errmsg);
        usage(argv[0], stderr);
        return false;
    }
  }
  char const* letters = optparse_arg(&options);
  if (letters == NULL) {
    fprintf(stderr, "error: missing %s argument\n",
            out->near ? "input" : (out->score ? "query" : "letters"));
    usage(argv[0], stderr);
    return false;
  }
  char const* extra = optparse_arg(&options);
  if (extra != NULL) {
    fprintf(stderr, "error: unexpected argument \"%s\"\n", extra);
    usage(argv[0], stderr);
    return false;
  }
  if (out->near) {
    if (out->near_incompatible_option != NULL) {
      fprintf(stderr, "error: %s cannot be used with --near\n",
              out->near_incompatible_option);
      return false;
    }
    if (out->index_file == NULL &&
        !index_from_env(argv[0], "-i INDEX", &out->index_file))
      return false;
    out->near_input = letters;
    return validate_literal_entry(out->near_input) &&
        validate_literal_entry(out->near_target);
  }

  if (!finalize_dfs_common_args(
          &out->common, argv[0], &out->index_file,
          out->score ? NULL : &out->workflow_reject_files,
          /*allow_targetless_workflow=*/true))
    return false;
  if (out->index_file == NULL &&
      !index_from_env(argv[0], "-i INDEX or --wfroot DIR", &out->index_file))
    return false;

  if (out->score) {
    if (out->score_incompatible_option != NULL) {
      fprintf(stderr, "error: %s cannot be used with --score\n",
              out->score_incompatible_option);
      return false;
    }
    out->score_sequence = letters;
    if (out->sd && !out->common.show_score) {
      fputs("error: --sd cannot be used with --csv\n", stderr);
      return false;
    }
    return true;
  }

  if (out->sd) {
    fputs("error: --sd cannot be used without --score\n", stderr);
    return false;
  }

  if (out->num_words != 0 &&
      (out->min_words_given || out->common.max_extract_words_given)) {
    fputs("error: --num-words cannot be combined with --min-words or "
          "--max-words\n", stderr);
    return false;
  }
  if (out->num_words != 0 && !out->require_completable)
    out->common.max_extract_words = out->num_words;
  if (out->min_words > 0 && out->common.max_extract_words > 0 &&
      out->min_words > out->common.max_extract_words) {
    fprintf(stderr, "error: --min-words %d exceeds --max-words %d\n",
            out->min_words, out->common.max_extract_words);
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

struct ScoreValue {
  std::string value;
  std::string entry;
  std::string reverse_entry;
  bool pair;
};

static bool is_literal_word(std::string const& word) {
  if (word.empty()) return false;
  for (size_t i = 0; i < word.size(); ++i) {
    char const ch = word[i];
    if ((ch < 'a' || ch > 'z') && (ch < '0' || ch > '9')) return false;
  }
  return true;
}

static bool parse_positional_score_value(
    std::string const& query, ScoreValue* out) {
  size_t const comma = query.find(',');
  if (comma == std::string::npos) {
    size_t first = 0;
    size_t last = query.size();
    while (first < last && isspace((unsigned char) query[first])) ++first;
    while (last > first && isspace((unsigned char) query[last - 1])) --last;
    out->entry = query.substr(first, last - first);
    if (out->entry.empty()) {
      fputs("error: empty entry in --score query\n", stderr);
      return false;
    }
    if (!validate_literal_entry(out->entry)) return false;
    out->value = query;
    out->pair = false;
    return true;
  }
  if (query.find(',', comma + 1) != std::string::npos) {
    fputs("error: positional --score query may contain at most one comma\n",
          stderr);
    return false;
  }

  size_t left_first = 0;
  size_t left_last = comma;
  while (left_first < left_last &&
         isspace((unsigned char) query[left_first]))
    ++left_first;
  while (left_last > left_first &&
         isspace((unsigned char) query[left_last - 1]))
    --left_last;
  size_t right_first = comma + 1;
  size_t right_last = query.size();
  while (right_first < right_last &&
         isspace((unsigned char) query[right_first]))
    ++right_first;
  while (right_last > right_first &&
         isspace((unsigned char) query[right_last - 1]))
    --right_last;

  std::string const left = query.substr(left_first, left_last - left_first);
  std::string const right =
      query.substr(right_first, right_last - right_first);
  if (!is_literal_word(left) || !is_literal_word(right)) {
    fputs("error: positional --score pair must be two lowercase a-z0-9 words\n",
          stderr);
    return false;
  }

  DfsPairRow const row = { left, right, 0 };
  out->value = query;
  out->entry = row.entry();
  if (left != right) out->reverse_entry = row.entry(/*reverse=*/true);
  out->pair = true;
  return true;
}

static bool parse_score_values(
    std::vector<ScoreValue>* values) {
  std::string line;
  size_t line_number = 0;
  while (std::getline(std::cin, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') line.pop_back();

    std::string value;
    std::vector<std::string> words;
    size_t start = 0;
    for (;;) {
      size_t const comma = line.find(',', start);
      size_t first = start;
      size_t last = comma == std::string::npos ? line.size() : comma;
      while (first < last && isspace((unsigned char) line[first])) ++first;
      while (last > first && isspace((unsigned char) line[last - 1])) --last;

      std::string const word = line.substr(first, last - first);
      if (!is_literal_word(word)) {
        fprintf(stderr,
            "error: stdin line %zu: expected comma-separated words\n",
            line_number);
        return false;
      }
      if (!value.empty()) {
        value += ',';
      }
      value += word;
      words.push_back(word);
      if (comma == std::string::npos) break;
      start = comma + 1;
    }

    std::string entry;
    std::string reverse_entry;
    if (words.size() == 2) {
      DfsPairRow const row = { words[0], words[1], line_number };
      entry = row.entry();
      if (row.left != row.right)
        reverse_entry = row.entry(/*reverse=*/true);
    } else {
      for (size_t i = 0; i < words.size(); ++i) {
        if (!entry.empty()) entry += ' ';
        entry += words[i];
      }
    }
    ScoreValue const value_entry = {
      std::move(value),
      std::move(entry),
      std::move(reverse_entry),
      words.size() == 2,
    };
    values->push_back(value_entry);
  }
  if (std::cin.bad()) {
    fputs("error: can't read stdin\n", stderr);
    return false;
  }
  return true;
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
  for (size_t i = 0; i < rows.size(); ++i) {
    if (args.common.show_score)
      printf("%lld %s\n", (long long) rows[i].count, rows[i].phrase.c_str());
    else
      put_csv(rows[i].phrase.data(), rows[i].phrase.size());
  }
  return 0;
}

// Everything one exact entry contributes to a score, gathered without
// enumerating anything: see
// findings/query-index-score-needs-no-phase-1.md.
struct DfsEntryTerms {
  int64_t count;
  bool multi_word;
  uint16_t flags;
  bool direct_best;
  bool has_profile;
  DfsSoloMasks profile;
};

static bool gather_entry_terms(
    IndexReader const& reader, DfsPreparedClassList const& prepared,
    std::string const& entry, DfsEntryTerms* out) {
  if (!reader.aggregate_entry_count(entry, &out->count)) return false;

  out->multi_word = entry.find(' ') != std::string::npos;
  uint16_t flags = 0;
  DfsPairBonusMap::const_iterator const weighted =
      prepared.weighted_pairs.find(entry);
  if (weighted != prepared.weighted_pairs.end())
    flags |= dfs_pair_bonus_score_flags(weighted->second);
  else if (prepared.pairs.count(entry) != 0)
    flags |= dfs_pair_bonus_score_flags(DFS_PAIR_BONUS_LEGACY);
  out->direct_best =
      dfs_member_pair_bonus_kind(flags) == DFS_PAIR_BONUS_BEST;

  out->has_profile = false;
  if (!out->multi_word && prepared.solo_words != NULL) {
    DfsSoloMasks const profile = prepared.solo_words->resolve(entry);
    flags |= dfs_solo_score_flags(profile);
    if (profile.word_mask != 0) {
      out->profile = profile;
      out->has_profile = true;
    }
  }
  out->flags = flags;
  return true;
}

static double entry_upper_log_score(
    DfsScoreModel const& model, DfsEntryTerms const& terms) {
  return model.member_upper_log_score(
      terms.count, terms.multi_word, terms.flags);
}

static bool score_entry_sequence(
    IndexReader const& reader, DfsPreparedClassList const& prepared,
    std::vector<std::string> const& entries, double* log_score,
    bool* bonus_applied = NULL) {
  DfsScoreModel const& model = *prepared.model;
  std::vector<DfsSoloMasks> profiles;
  std::vector<bool> profile_direct_best;
  size_t direct_best_segments = 0;
  profiles.reserve(entries.size());
  profile_direct_best.reserve(entries.size());

  double upper_log_score = 0.0;
  double plain_log_score = 0.0;
  for (size_t i = 0; i < entries.size(); ++i) {
    DfsEntryTerms terms;
    if (!gather_entry_terms(reader, prepared, entries[i], &terms)) {
      fprintf(stderr, "error: index has no entry \"%s\"\n",
              entries[i].c_str());
      return false;
    }
    if (terms.direct_best) ++direct_best_segments;
    if (terms.has_profile) {
      profiles.push_back(terms.profile);
      profile_direct_best.push_back(terms.direct_best);
    }
    double const segment = entry_upper_log_score(model, terms);
    double const plain =
        model.segment_log_score(terms.count, /*multi_word=*/false);
    upper_log_score = i == 0
        ? segment : model.append_log_score(upper_log_score, segment);
    plain_log_score = i == 0
        ? plain : model.append_log_score(plain_log_score, plain);
  }

  DfsExactResultMatching const exact = dfs_exact_result_matching(
      profiles, profile_direct_best, direct_best_segments, model);
  *log_score = upper_log_score + exact.correction;
  if (bonus_applied != NULL)
    *bonus_applied = *log_score != plain_log_score;
  assert(*log_score <= upper_log_score);
  return true;
}

static std::string format_score(double score) {
  char buffer[32];
  snprintf(buffer, sizeof buffer, "%#.7g", score);
  return std::string(buffer);
}

struct ScoreResult {
  double log_score;
  std::string value;
};

static bool score_result_better(
    ScoreResult const& a, ScoreResult const& b) {
  if (a.log_score != b.log_score) return a.log_score > b.log_score;
  return a.value < b.value;
}

// Displayed scores span the corpus by orders of magnitude, so the summary
// describes the log scores; see findings/stdin-score-statistics.md for why a
// bonus of any kind withdraws it.
struct ScoreStats {
  bool shown;
  double mean;
  double deviation;
};

static ScoreStats score_stats(
    std::vector<ScoreResult> const& results, bool bonus_applied) {
  ScoreStats stats = { false, 0.0, 0.0 };
  if (bonus_applied) return stats;

  size_t count = 0;
  double total = 0.0;
  for (size_t i = 0; i < results.size(); ++i) {
    if (!isfinite(results[i].log_score)) continue;
    total += results[i].log_score;
    ++count;
  }
  if (count == 0) return stats;

  stats.shown = true;
  stats.mean = total / double(count);
  double squares = 0.0;
  for (size_t i = 0; i < results.size(); ++i) {
    if (!isfinite(results[i].log_score)) continue;
    double const difference = results[i].log_score - stats.mean;
    squares += difference * difference;
  }
  stats.deviation = sqrt(squares / double(count));
  return stats;
}

static std::string format_deviation(
    ScoreStats const& stats, double log_score) {
  if (!isfinite(log_score)) return "-";
  double const deviations = stats.deviation == 0.0
      ? 0.0 : (log_score - stats.mean) / stats.deviation;
  char buffer[32];
  snprintf(buffer, sizeof buffer, "%.2f", deviations);
  return std::string(buffer);
}

static std::string format_mapped(double mapped) {
  if (!isfinite(mapped)) return "-";
  char buffer[32];
  snprintf(buffer, sizeof buffer, "%.2f", mapped);
  return std::string(buffer);
}

static std::string format_mapped_score(
    ScoreStats const& stats, DfsScoreModel const& model, double mapped) {
  if (!isfinite(mapped)) return "-";
  return format_score(
      model.displayed_score(stats.mean + mapped * stats.deviation));
}

static bool score_value(
    IndexReader const& reader, DfsPreparedClassList const& prepared,
    ScoreValue const& value, double* log_score,
    bool* bonus_applied = NULL) {
  DfsEntryTerms forward;
  DfsEntryTerms reverse;
  bool const has_forward =
      gather_entry_terms(reader, prepared, value.entry, &forward);
  bool const has_reverse = !value.reverse_entry.empty() &&
      gather_entry_terms(reader, prepared, value.reverse_entry, &reverse);

  std::string const* chosen = NULL;
  if (has_forward && has_reverse)
    chosen = entry_upper_log_score(*prepared.model, reverse) >
             entry_upper_log_score(*prepared.model, forward)
        ? &value.reverse_entry : &value.entry;
  else if (has_forward)
    chosen = &value.entry;
  else if (has_reverse)
    chosen = &value.reverse_entry;
  else if (!value.pair) {
    fprintf(stderr, "error: index has no entry \"%s\"\n",
            value.entry.c_str());
    return false;
  }

  *log_score = -INFINITY;
  if (bonus_applied != NULL) *bonus_applied = false;
  if (chosen == NULL) return true;

  std::vector<std::string> const entries(1, *chosen);
  return score_entry_sequence(
      reader, prepared, entries, log_score, bonus_applied);
}

static int score_value_list(IndexReader& reader, Args const& args) {
  std::vector<ScoreValue> values;
  if (!parse_score_values(&values)) return 2;

  DfsPreparedClassList prepared;
  if (!prepare_dfs_scoring_inputs(
          &reader, args.common, /*score_mode=*/true,
          /*exact_segments=*/1, &prepared))
    return 1;

  std::vector<ScoreResult> results;
  results.reserve(values.size());
  bool bonus_applied = false;
  for (size_t i = 0; i < values.size(); ++i) {
    double log_score;
    bool entry_bonus = false;
    if (!score_value(
            reader, prepared, values[i], &log_score, &entry_bonus))
      return 2;
    if (!isfinite(log_score)) continue;
    if (entry_bonus) bonus_applied = true;
    ScoreResult result = {
      log_score,
      std::move(values[i].value),
    };
    results.push_back(std::move(result));
  }

  std::sort(results.begin(), results.end(), score_result_better);
  ScoreStats const stats = score_stats(results, bonus_applied);
  TailMap map;
  if (args.ptm && stats.shown) {
    std::vector<double> finite;
    finite.reserve(results.size());
    for (size_t i = 0; i < results.size(); ++i) {
      if (!isfinite(results[i].log_score)) break;
      finite.push_back(results[i].log_score);
    }
    map.fit(finite);
  }
  std::vector<double> mapped(results.size(), NAN);
  for (size_t i = 0; i < results.size(); ++i)
    mapped[i] = map.deviation(results[i].log_score);

  std::vector<std::string> scores;
  std::vector<std::string> deviations;
  std::vector<std::string> mapped_text;
  std::vector<std::string> mapped_scores;
  scores.reserve(results.size());
  deviations.reserve(results.size());
  mapped_text.reserve(results.size());
  mapped_scores.reserve(results.size());
  size_t width = 0;
  size_t deviation_width = 0;
  size_t mapped_width = 0;
  size_t mapped_score_width = 0;
  for (size_t i = 0; i < results.size(); ++i) {
    scores.push_back(format_score(
        prepared.model->displayed_score(results[i].log_score)));
    width = std::max(width, scores[i].size());
    if (!stats.shown) continue;
    deviations.push_back(format_deviation(stats, results[i].log_score));
    deviation_width = std::max(deviation_width, deviations[i].size());
    if (!map.valid()) continue;
    mapped_text.push_back(format_mapped(mapped[i]));
    mapped_width = std::max(mapped_width, mapped_text[i].size());
    mapped_scores.push_back(
        format_mapped_score(stats, *prepared.model, mapped[i]));
    mapped_score_width = std::max(mapped_score_width, mapped_scores[i].size());
  }

  if (args.sd && stats.shown) {
    std::string const mean =
        format_score(prepared.model->displayed_score(stats.mean));
    if (map.valid())
      printf("Mean: %s  1 sigma: x%.2f  tail rate: %.3f\n",
             mean.c_str(), exp(stats.deviation), map.rate());
    else
      printf("Mean: %s  1 sigma: x%.2f\n", mean.c_str(), exp(stats.deviation));
  }
  for (size_t i = 0; i < results.size(); ++i) {
    if (args.sd && stats.shown && map.valid()) {
      if (args.common.show_score)
        printf("%*s %*s %*s %*s %s\n", int(width), scores[i].c_str(),
               int(deviation_width), deviations[i].c_str(),
               int(mapped_width), mapped_text[i].c_str(),
               int(mapped_score_width), mapped_scores[i].c_str(),
               results[i].value.c_str());
      else
        printf("%*s %*s %*s %s\n",
               int(deviation_width), deviations[i].c_str(),
               int(mapped_width), mapped_text[i].c_str(),
               int(mapped_score_width), mapped_scores[i].c_str(),
               results[i].value.c_str());
    } else if (args.sd && stats.shown) {
      if (args.common.show_score)
        printf("%*s %*s %s\n", int(width), scores[i].c_str(),
               int(deviation_width), deviations[i].c_str(),
               results[i].value.c_str());
      else
        printf("%*s %s\n", int(deviation_width), deviations[i].c_str(),
               results[i].value.c_str());
    } else if (map.valid()) {
      if (args.common.show_score)
        printf("%*s %*s %*s %s\n", int(width), scores[i].c_str(),
               int(mapped_width), mapped_text[i].c_str(),
               int(mapped_score_width), mapped_scores[i].c_str(),
               results[i].value.c_str());
      else
        printf("%*s %*s %s\n", int(mapped_width), mapped_text[i].c_str(),
               int(mapped_score_width), mapped_scores[i].c_str(),
               results[i].value.c_str());
    } else if (args.common.show_score) {
      printf("%*s %s\n", int(width), scores[i].c_str(),
             results[i].value.c_str());
    } else {
      puts(results[i].value.c_str());
    }
  }
  return 0;
}

int main(int argc, char* argv[]) {
  dfs_reset_diagnostic_clock();
  dfs_set_diagnostic_stream(stderr);

  Args args;
  bool help = false;
  if (!parse_args(argv, &args, &help)) return 2;
  if (help) {
    usage(argv[0], stdout);
    return 0;
  }
  if (!args.common.workflow_root.empty() && args.common.target.empty())
    warn(argv[0],
        "no target supplied; only dictionary and classified-no filtering "
        "are active");
  if (!args.score && !args.near)
    dfs_diagnostic_letter_bag(args.letters);

  ScoreValue positional_score_value;
  bool const score_stdin = args.score && args.score_sequence == "-";
  if (args.score && !score_stdin &&
      !parse_positional_score_value(
          args.score_sequence, &positional_score_value))
    return 2;

  FILE* fp = fopen(args.index_file, "rb");
  if (fp == NULL) {
    fprintf(stderr, "error: can't open \"%s\"\n", args.index_file);
    return 1;
  }
  IndexReader reader(fp);

  if (args.near) return run_near_query(reader, args);

  if (args.score) {
    if (score_stdin) return score_value_list(reader, args);

    DfsPreparedClassList prepared;
    if (!prepare_dfs_scoring_inputs(
            &reader, args.common, /*score_mode=*/true,
            /*exact_segments=*/1, &prepared))
      return 1;

    double log_score;
    if (!score_value(
            reader, prepared, positional_score_value, &log_score))
      return 2;
    if (!isfinite(log_score)) return 0;
    if (args.common.show_score)
      printf("%s %s\n",
             format_score(prepared.model->displayed_score(log_score)).c_str(),
             args.score_sequence.c_str());
    else
      puts(args.score_sequence.c_str());
    return 0;
  }

  DfsPreparedClassList prepared;
  if (!prepare_dfs_class_list(
          &reader, args.letters, args.common, args.workflow_reject_files,
          /*exact_segments=*/0, &prepared))
    return 1;
  DfsClassList& classes = *prepared.classes;
  DfsScoreModel const& model = *prepared.model;
  DfsSoloWords* const solo_words = prepared.solo_words.get();
  DfsPairBonusMap const& weighted_pairs = prepared.weighted_pairs;

  std::vector<bool> completable(classes.classes().size(), true);
  if (args.require_completable) {
    size_t const search_threads = resolve_search_threads(
        args.common.search_threads);
    dfs_diagnostic(
        "search threads %zu cache 0 segment penalty %.17g\n",
        search_threads, args.common.segment_penalty);
    DfsAnagramSearch search(
        &classes, args.letters, model,
        /*score_cache_bytes=*/0, /*preprocess_threads=*/1,
        search_threads, /*exact_segments=*/0);
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
  DfsMemberSpan const survivors =
      classes.retain_members(completable, DFS_RETAIN_ALL);
  DfsPackedMember* const first = survivors.data;
  DfsPackedMember* const last = args.num_words == 0 && args.min_words == 0
      ? first + survivors.count
      : std::remove_if(first, first + survivors.count,
            [&](DfsPackedMember const& member) {
              return args.num_words != 0
                  ? member.word_count != args.num_words
                  : member.word_count < args.min_words;
            });
  size_t const survivor_count = size_t(last - first);
  size_t const top = args.common.top == 0
      ? survivor_count
      : std::min(survivor_count, size_t(args.common.top));

  auto const print_row = [&](DfsPackedMember const& row) {
    if (!args.common.show_score) {
      put_csv(row.text, row.text_length);
      return;
    }
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
    if (args.common.word_bonus == 0.0 &&
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
