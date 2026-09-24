#include "classified.h"
#include "dfs-class-list-build.h"
#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "dfs-diagnostic.h"
#include "dfs-output.h"
#include "dfs-score.h"
#include "index.h"
#include "optparse.h"
#include "segment-rows.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Args {
  DfsCommonArgs common;
  std::vector<std::string> reject_files;
  std::vector<std::string> workflow_reject_files;
  // Empty, or -1 for the segment count, until the first row supplies it.
  std::string letters;
  char const* index_file = NULL;
  char const* results_file = NULL;
  int num_segments = -1;
  int sentence = CLASSIFIED_NO_SENTENCE;
  bool show_bonus = false;
  bool ptm = true;
};

void usage(char const* program, FILE* out) {
  fprintf(out,
      "usage: %s (--wf | --wfroot DIR) [-t TARGET] [--seed-pairs FILE]..."
      " [-m N] [-g N] [-s N]"
      " [-r FILE]... [--best-pairs FILE | --one-best-pair PAIR]"
      " [--more-best-pairs FILE]..."
      " [--solo-words WORD[,WORD...]] [--hide-solo-words]"
      " [--sb N] [--yb N] [--bb N]"
      " [--show-bonus] [--no-score] [--no-ptm] [-n N] [FILE]\n",
      program);
  if (out != stdout) return;

  fputs(
      "  revalidate, rescore, and sort an ordinary dfs-anagrams result file\n"
      "  with no FILE, or when FILE is -, read standard input\n"
      "  requires -t, --seed-pairs, or both\n"
      "\noptions:\n",
      stdout);
  dfs_help_wf();
  dfs_help_option("--wfroot DIR", "use DIR as the workflow root");
  dfs_help_option("-t, --target TARGET",
      "select a prefix of sN/[ou]-letters/mN/gN; its sentence seed is "
      "loaded unless --seed-pairs is given, and a complete target also "
      "loads its optional best.pairs and sets the bag, -m, and -g; "
      "otherwise the bag comes from the first row");
  dfs_help_seed_pairs();
  dfs_help_min_word_length();
  dfs_help_option("-g, --num-segments N",
      "require N segments per row, scoring BEST with the descending bonus; "
      "0 accepts any count and scores BEST with the fixed bonus; defaults "
      "to the first row's count, and a later row with another count fails");
  classified_help_sentence_pairs();
  dfs_help_option("--best-pairs FILE",
      "replace the target's implicit best.pairs");
  dfs_help_option("--one-best-pair PAIR",
      "replace it with one WORD,WORD pair");
  dfs_help_option("--more-best-pairs FILE", "add BEST pairs; may be repeated");
  dfs_help_option("-r, --reject FILE",
      "reject as dfs-anagrams -r does: a pair drops the index entry spelled "
      "like it in either order, and a single word is removed from the "
      "dictionary unless a BEST pair uses it; rows using a rejected entry "
      "are dropped; may be repeated");
  dfs_help_option("--solo-words WORD[,WORD...]",
      "score single-word segments against external partners as "
      "dfs-anagrams does; parenthesized partners in the input are ignored");
  dfs_help_hide_solo_words();
  dfs_help_tier_bonuses();
  dfs_help_option("--show-bonus", "add the aligned S/Y/B/- marker column");
  dfs_help_option("--no-score",
      "omit the leading score column; the output can't be filtered or "
      "reranked");
  dfs_help_option("--no-ptm",
      "turn off ptm, which is on by default; ptm recalibrates the base count "
      "of every index entry, before any bonus, onto a scale whose upper tail "
      "is normal, as dfs-anagrams does; the fit covers the entries this bag "
      "reaches, so it reproduces a result file's own scores only when the "
      "bag, dictionary, pair inputs, and exclusions are the generator's");
  dfs_help_option("-n, --top N",
      "print only the best N rows; 0, the default, prints every surviving "
      "row");
  dfs_help_option("-h, --help", "show this help");
}

inline constexpr int OPT_SHOW_BONUS = 256;
inline constexpr int OPT_ONE_BEST_PAIR = 258;
inline constexpr int OPT_NO_PTM = 259;

struct optparse_long const long_options[] = {
  { "wf", DFS_OPT_WF, OPTPARSE_NONE },
  { "wfroot", DFS_OPT_WFROOT, OPTPARSE_REQUIRED },
  { "target", 't', OPTPARSE_REQUIRED },
  { "seed-pairs", DFS_OPT_SEED_PAIRS, OPTPARSE_REQUIRED },
  { "min-word-length", 'm', OPTPARSE_REQUIRED },
  { "num-segments", 'g', OPTPARSE_REQUIRED },
  { "best-pairs", DFS_OPT_BEST_PAIRS, OPTPARSE_REQUIRED },
  { "more-best-pairs", DFS_OPT_MORE_BEST_PAIRS, OPTPARSE_REQUIRED },
  { "one-best-pair", OPT_ONE_BEST_PAIR, OPTPARSE_REQUIRED },
  { "reject", 'r', OPTPARSE_REQUIRED },
  CLASSIFIED_SENTENCE_LONG_OPTION,
  { "solo-words", DFS_OPT_SOLO_WORDS, OPTPARSE_REQUIRED },
  { "hide-solo-words", DFS_OPT_HIDE_SOLO_WORDS, OPTPARSE_NONE },
  { "show-bonus", OPT_SHOW_BONUS, OPTPARSE_NONE },
  { "no-score", DFS_OPT_NO_SCORE, OPTPARSE_NONE },
  { "no-ptm", OPT_NO_PTM, OPTPARSE_NONE },
  DFS_TIER_BONUS_LONG_OPTIONS,
  { "top", 'n', OPTPARSE_REQUIRED },
  { "help", 'h', OPTPARSE_NONE },
  { NULL, 0, OPTPARSE_NONE },
};

bool valid_one_best_pair(char const* value) {
  char const* const comma = strchr(value, ',');
  if (comma == NULL || comma == value || comma[1] == '\0') return false;
  if (strchr(comma + 1, ',') != NULL) return false;
  for (char const* p = value; *p != '\0'; ++p)
    if (p != comma && (*p < 'a' || *p > 'z')) return false;
  return true;
}

bool parse_args(char* argv[], Args* out, bool* help) {
  out->common = DfsCommonArgs();
  out->common.max_extract_words = 2;

  struct optparse options;
  optparse_init(&options, argv);
  int option;
  while ((option = optparse_long(&options, long_options, NULL)) != -1) {
    switch (dfs_parse_common_option(
        option, &options, &out->common, NULL)) {
      case DFS_OPTION_ERROR:
        return false;
      case DFS_OPTION_HANDLED:
        continue;
      case DFS_OPTION_OTHER:
        break;
    }
    switch (option) {
      case 'r':
        out->reject_files.push_back(options.optarg);
        break;
      case 's':
        if (!parse_classified_sentence(options.optarg, &out->sentence))
          return false;
        break;
      case OPT_SHOW_BONUS:
        out->show_bonus = true;
        break;
      case OPT_NO_PTM:
        out->ptm = false;
        break;
      case 'h':
        *help = true;
        return true;
      case 'g':
        if (!parse_count(options.optarg, "--num-segments",
                         &out->num_segments))
          return false;
        break;
      case OPT_ONE_BEST_PAIR:
        if (out->common.best_pairs_given) {
          fputs("error: --one-best-pair and --best-pairs are mutually "
                "exclusive, and each may be specified only once\n", stderr);
          usage(argv[0], stderr);
          return false;
        }
        if (!valid_one_best_pair(options.optarg)) {
          fputs("error: --one-best-pair requires two comma-separated "
                "lowercase words\n", stderr);
          usage(argv[0], stderr);
          return false;
        }
        out->common.best_pairs_given = true;
        out->common.one_best_pair = options.optarg;
        break;
      default:
        fprintf(stderr, "error: %s\n", options.errmsg);
        usage(argv[0], stderr);
        return false;
    }
  }

  out->results_file = optparse_arg(&options);
  if (optparse_arg(&options) != NULL) {
    fputs("rerank-anagrams: at most one FILE may be specified\n", stderr);
    usage(argv[0], stderr);
    return false;
  }
  if (!out->common.workflow && out->common.workflow_root.empty()) {
    fputs("rerank-anagrams: requires exactly one of --wf or --wfroot\n",
          stderr);
    usage(argv[0], stderr);
    return false;
  }
  for (size_t i = 0; i < out->reject_files.size(); ++i) {
    struct stat status;
    if (stat(out->reject_files[i].c_str(), &status) == 0 &&
        S_ISDIR(status.st_mode)) {
      fprintf(stderr, "rerank-anagrams: -r takes a file, not directory"
              " \"%s\"\n", out->reject_files[i].c_str());
      return false;
    }
  }
  if (out->sentence != CLASSIFIED_NO_SENTENCE &&
      !add_classified_sentence_pairs(
          argv[0], out->sentence, &out->common, &out->workflow_reject_files))
    return false;
  if (!finalize_dfs_common_args(
          &out->common, argv[0], &out->index_file,
          &out->workflow_reject_files))
    return false;
  if (!check_classified_sentence_target(
          argv[0], out->sentence, out->common.target))
    return false;
  if (!out->common.target_complete) return true;

  DfsWorkflowTargetSettings target;
  if (!load_dfs_workflow_target_settings(
          out->common, argv[0], &target))
    return false;
  if (out->common.min_word_len_given &&
      out->common.min_word_len != target.min_word_len) {
    fprintf(stderr, "rerank-anagrams: -m %d does not match target \"%s\"\n",
            out->common.min_word_len, out->common.target.c_str());
    return false;
  }
  if (out->num_segments >= 0 && out->num_segments != target.num_segments) {
    fprintf(stderr, "rerank-anagrams: -g %d does not match target \"%s\"\n",
            out->num_segments, out->common.target.c_str());
    return false;
  }
  out->letters = target.letters;
  out->common.min_word_len = target.min_word_len;
  out->common.min_word_len_given = true;
  out->num_segments = target.num_segments;
  return true;
}

void score_row(
    SegmentRow const& row, DfsMemberIndex const& members, bool show_bonus,
    DfsPreparedClassList const& prepared, std::vector<DfsSpelling>* results) {
  std::vector<size_t> class_indexes;
  std::vector<size_t> member_indexes;
  class_indexes.reserve(row.segments.size());
  member_indexes.reserve(row.segments.size());
  for (size_t i = 0; i < row.segments.size(); ++i) {
    DfsMemberIndex::const_iterator const found =
        members.find(row.segments[i]);
    if (found == members.end()) return;
    class_indexes.push_back(found->second.class_index);
    member_indexes.push_back(found->second.member_index);
  }

  double const representative = dfs_representative_upper_log_score(
      *prepared.classes, *prepared.model, class_indexes);
  results->push_back(dfs_build_spelling(
      *prepared.classes, *prepared.model, prepared.solo_words.get(),
      class_indexes, member_indexes, representative, show_bonus));
}

// The class list depends on the bag and segment count, which the first row
// supplies when the target does not, so it is prepared after that row.
bool rerank_stream(
    std::istream* input, char const* name, Args* args, IndexReader* index) {
  SegmentRowReader reader = {input, name, "rerank-anagrams"};
  reader.required_letters = args->letters;
  reader.required_segments = std::max(args->num_segments, 0);
  reader.infer_letters = args->letters.empty();
  reader.infer_segments = args->num_segments < 0;
  SegmentRow row;
  if (!segment_rows_next(&reader, &row)) return !reader.failed;

  args->letters = reader.required_letters;
  args->num_segments = reader.required_segments;
  if (!check_bag_length(args->letters) ||
      !finalize_min_word_length(
          args->letters, args->common.min_word_len_given,
          &args->common.min_word_len))
    return false;
  DfsPreparedClassList prepared;
  if (!prepare_dfs_class_list(
          index, args->letters, args->common, args->workflow_reject_files,
          size_t(args->num_segments), &prepared, args->ptm,
          &args->reject_files))
    return false;
  DfsMemberIndex members;
  std::string duplicate;
  if (!dfs_index_members(*prepared.classes, &members, &duplicate)) {
    fprintf(stderr, "rerank-anagrams: duplicate phase-1 spelling \"%s\"\n",
            duplicate.c_str());
    return false;
  }

  std::vector<DfsSpelling> results;
  do {
    score_row(row, members, args->show_bonus, prepared, &results);
  } while (segment_rows_next(&reader, &row));
  if (reader.failed) return false;

  std::sort(results.begin(), results.end(), dfs_spelling_better);
  if (args->common.top > 0 && results.size() > size_t(args->common.top))
    results.resize(size_t(args->common.top));
  return dfs_print_results(
      stdout, results, args->common.show_score, args->show_bonus,
      args->common.hide_solo_words ? NULL : prepared.solo_words.get());
}

}  // namespace

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

  std::ifstream results_input;
  std::istream* input = &std::cin;
  char const* input_name = "-";
  if (args.results_file != NULL && strcmp(args.results_file, "-") != 0) {
    errno = 0;
    results_input.open(args.results_file, std::ios::binary);
    if (!results_input.is_open()) {
      fprintf(stderr, "rerank-anagrams: can't open \"%s\": %s\n",
              args.results_file, strerror(errno));
      return 1;
    }
    input = &results_input;
    input_name = args.results_file;
  }

  FILE* index = fopen(args.index_file, "rb");
  if (index == NULL) {
    fprintf(stderr, "error: can't open \"%s\"\n", args.index_file);
    return 1;
  }

  int status = 1;
  {
    IndexReader reader(index);
    status = rerank_stream(input, input_name, &args, &reader) ? 0 : 1;
  }
  if (fclose(index) != 0 && status == 0) {
    fprintf(stderr, "rerank-anagrams: can't close index \"%s\"\n",
            args.index_file);
    return 1;
  }
  return status;
}
