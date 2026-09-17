#include "dfs-class-list-build.h"
#include "dfs-cli-args.h"
#include "dfs-diagnostic.h"
#include "dfs-output.h"
#include "index.h"
#include "optparse.h"
#include "pair-exclusions.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Args {
  DfsCommonArgs common;
  std::vector<std::string> reject_files;
  std::vector<std::string> exclude_pair_files;
  std::string letters;
  char const* index_file = NULL;
  char const* results_file = NULL;
  int num_segments = 0;
  bool show_score = true;
  bool show_bonus = false;
  bool ptm = false;
};

void usage(char const* program) {
  fprintf(stdout,
      "usage: %s (--wf | --wfroot DIR) -t FULL-TARGET"
      " [-r FILE]... [--best-pairs FILE | --one-best-pair PAIR]"
      " [--more-best-pairs FILE]..."
      " [--solo-words WORD[,WORD...]] [--hide-solo-words]"
      " [--show-bonus] [--no-score] [--ptm] [-n N] [FILE]\n"
      "  revalidate, rescore, and sort an ordinary dfs-anagrams result file\n"
      "  --wf                 use the nonempty WFROOT environment variable\n"
      "  --wfroot DIR         use DIR as the workflow root\n"
      "  -t, --target TARGET  require sN/[ou]-letters/mN/gN and derive the\n"
      "                       working bag, minimum, and segment count\n"
      "  --best-pairs FILE    replace the target's implicit best.pairs\n"
      "  --one-best-pair PAIR replace it with one WORD,WORD pair\n"
      "  --more-best-pairs FILE\n"
      "                       add BEST pairs; may be repeated\n"
      "  -r, --reject FILE    reject rows containing a listed word or exact\n"
      "                       bidirectional pair; may be repeated\n"
      "  --solo-words WORD[,WORD...]\n"
      "                       score single-word segments against external\n"
      "                       partners as dfs-anagrams does; parenthesized\n"
      "                       partners in the input are ignored\n"
      "  --hide-solo-words    omit parenthesized solo partners from output\n"
      "  --show-bonus         add the aligned S/Y/B/- marker column\n"
      "  --no-score           omit the leading score column; the output\n"
      "                       can't be filtered or reranked\n"
      "  --ptm                recalibrate the base count of every index\n"
      "                       entry, before any bonus, onto a scale whose\n"
      "                       upper tail is normal, as dfs-anagrams --ptm\n"
      "                       does; the fit covers the entries this bag\n"
      "                       reaches, so it reproduces a result file's own\n"
      "                       scores only when the bag, dictionary, pair\n"
      "                       inputs, and exclusions are the generator's\n"
      "  -n, --top N          print only the best N rows; 0, the default,\n"
      "                       prints every surviving row\n"
      "  with no FILE, or when FILE is -, read standard input\n",
      program);
}

inline constexpr int OPT_SHOW_BONUS = 256;
inline constexpr int OPT_NO_SCORE = 257;
inline constexpr int OPT_ONE_BEST_PAIR = 258;
inline constexpr int OPT_PTM = 259;

struct optparse_long const long_options[] = {
  { "wf", DFS_OPT_WF, OPTPARSE_NONE },
  { "wfroot", DFS_OPT_WFROOT, OPTPARSE_REQUIRED },
  { "target", 't', OPTPARSE_REQUIRED },
  { "best-pairs", DFS_OPT_BEST_PAIRS, OPTPARSE_REQUIRED },
  { "more-best-pairs", DFS_OPT_MORE_BEST_PAIRS, OPTPARSE_REQUIRED },
  { "one-best-pair", OPT_ONE_BEST_PAIR, OPTPARSE_REQUIRED },
  { "reject", 'r', OPTPARSE_REQUIRED },
  { "solo-words", DFS_OPT_SOLO_WORDS, OPTPARSE_REQUIRED },
  { "hide-solo-words", DFS_OPT_HIDE_SOLO_WORDS, OPTPARSE_NONE },
  { "show-bonus", OPT_SHOW_BONUS, OPTPARSE_NONE },
  { "no-score", OPT_NO_SCORE, OPTPARSE_NONE },
  { "ptm", OPT_PTM, OPTPARSE_NONE },
  { "top", 'n', OPTPARSE_REQUIRED },
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

bool parse_args(char* argv[], Args* out) {
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
      case OPT_SHOW_BONUS:
        out->show_bonus = true;
        break;
      case OPT_NO_SCORE:
        out->show_score = false;
        break;
      case OPT_PTM:
        out->ptm = true;
        break;
      case OPT_ONE_BEST_PAIR:
        if (out->common.best_pairs_given) {
          fputs("error: --one-best-pair and --best-pairs are mutually "
                "exclusive, and each may be specified only once\n", stderr);
          usage(argv[0]);
          return false;
        }
        if (!valid_one_best_pair(options.optarg)) {
          fputs("error: --one-best-pair requires two comma-separated "
                "lowercase words\n", stderr);
          usage(argv[0]);
          return false;
        }
        out->common.best_pairs_given = true;
        out->common.one_best_pair = options.optarg;
        break;
      default:
        fprintf(stderr, "error: %s\n", options.errmsg);
        usage(argv[0]);
        return false;
    }
  }

  out->results_file = optparse_arg(&options);
  if (optparse_arg(&options) != NULL) {
    fputs("rerank-anagrams: at most one FILE may be specified\n", stderr);
    usage(argv[0]);
    return false;
  }
  if (!out->common.workflow && out->common.workflow_root.empty()) {
    fputs("rerank-anagrams: requires exactly one of --wf or --wfroot\n",
          stderr);
    usage(argv[0]);
    return false;
  }
  if (out->common.target.empty()) {
    fputs("rerank-anagrams: requires -t FULL-TARGET\n", stderr);
    usage(argv[0]);
    return false;
  }
  if (!finalize_dfs_common_args(
          &out->common, argv[0], &out->index_file,
          &out->exclude_pair_files))
    return false;
  DfsWorkflowTargetSettings target;
  if (!load_dfs_workflow_target_settings(
          out->common, argv[0], &target))
    return false;
  out->letters = target.letters;
  out->common.min_word_len = target.min_word_len;
  out->common.min_word_len_given = true;
  out->num_segments = target.num_segments;
  return finalize_min_word_length(
      out->letters, /*explicitly_given=*/true,
      &out->common.min_word_len);
}

bool load_rejections(
    std::vector<std::string> const& paths, DfsPairSet* rejected) {
  for (size_t i = 0; i < paths.size(); ++i)
    if (!load_pair_file(
            paths[i].c_str(), "reject list", rejected,
            /*quiet=*/true, /*reject_hyphens=*/true,
            /*allow_single_words=*/true))
      return false;
  return true;
}

bool parse_segments(
    std::string const& line, char const* name, uint64_t line_number,
    std::string const& required_letters, int required_segments,
    std::vector<std::string>* segments) {
  char* score_end;
  errno = 0;
  double const input_score = strtod(line.c_str(), &score_end);
  if (score_end == line.c_str() || *score_end != ' ' ||
      score_end[1] == '\0' || errno == ERANGE || isnan(input_score) ||
      input_score < 0.0) {
    fprintf(stderr,
        "rerank-anagrams: %s:%" PRIu64
        ": expected \"score segment[,segment ...]\"\n",
        name, line_number);
    return false;
  }

  size_t start = size_t(score_end - line.c_str()) + 1;
  if ((line[start] >= 'A' && line[start] <= 'Z') || line[start] == '-') {
    fprintf(stderr,
        "rerank-anagrams: %s:%" PRIu64
        ": annotated input is not supported\n",
        name, line_number);
    return false;
  }

  std::string row_letters;
  while (true) {
    size_t const end = line.find(',', start);
    size_t const length = end == std::string::npos
        ? line.size() - start : end - start;
    if (length == 0) {
      fprintf(stderr, "rerank-anagrams: %s:%" PRIu64 ": empty segment\n",
              name, line_number);
      return false;
    }
    std::string segment = line.substr(start, length);
    size_t const partner = segment.rfind(" (");
    if (partner != std::string::npos && segment.back() == ')' &&
        partner + 3 < segment.size() &&
        std::all_of(segment.begin() + partner + 2, segment.end() - 1,
                    [](char ch) {
                      return (ch >= 'a' && ch <= 'z') ||
                             (ch >= '0' && ch <= '9');
                    }))
      segment.erase(partner);
    bool after_space = true;
    for (size_t i = 0; i < segment.size(); ++i) {
      char const ch = segment[i];
      if (ch == ' ') {
        if (after_space || i + 1 == segment.size()) {
          fprintf(stderr,
              "rerank-anagrams: %s:%" PRIu64
              ": malformed spacing in segment \"%s\"\n",
              name, line_number, segment.c_str());
          return false;
        }
        after_space = true;
      } else if ((ch >= 'a' && ch <= 'z') ||
                 (ch >= '0' && ch <= '9')) {
        row_letters.push_back(ch);
        after_space = false;
      } else {
        fprintf(stderr,
            "rerank-anagrams: %s:%" PRIu64
            ": bad character '%c' in segment\n",
            name, line_number, ch);
        return false;
      }
    }
    segments->push_back(segment);
    if (end == std::string::npos) break;
    start = end + 1;
  }

  if (segments->size() != size_t(required_segments)) {
    fprintf(stderr,
        "rerank-anagrams: %s:%" PRIu64
        ": expected %d segments, found %zu\n",
        name, line_number, required_segments, segments->size());
    return false;
  }
  std::string bag = required_letters;
  std::sort(bag.begin(), bag.end());
  std::sort(row_letters.begin(), row_letters.end());
  if (row_letters != bag) {
    fprintf(stderr,
        "rerank-anagrams: %s:%" PRIu64
        ": row does not spell target letters \"%s\"\n",
        name, line_number, required_letters.c_str());
    return false;
  }
  return true;
}

bool rerank_stream(
    std::istream* input, char const* name, Args const& args,
    DfsPairSet const& rejected, DfsPreparedClassList const& prepared) {
  DfsMemberIndex members;
  std::string duplicate;
  if (!dfs_index_members(*prepared.classes, &members, &duplicate)) {
    fprintf(stderr, "rerank-anagrams: duplicate phase-1 spelling \"%s\"\n",
            duplicate.c_str());
    return false;
  }
  std::vector<DfsSpelling> results;
  std::string line;
  uint64_t line_number = 0;
  while (std::getline(*input, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;

    std::vector<std::string> segments;
    if (!parse_segments(
            line, name, line_number, args.letters, args.num_segments,
            &segments))
      return false;

    std::vector<size_t> class_indexes;
    std::vector<size_t> member_indexes;
    class_indexes.reserve(segments.size());
    member_indexes.reserve(segments.size());
    bool keep = true;
    for (size_t i = 0; i < segments.size(); ++i) {
      if (is_rejected_segment(rejected, segments[i])) keep = false;
      DfsMemberIndex::const_iterator const found = members.find(segments[i]);
      if (found == members.end()) {
        keep = false;
      } else {
        class_indexes.push_back(found->second.class_index);
        member_indexes.push_back(found->second.member_index);
      }
    }
    if (!keep) continue;

    double const representative = dfs_representative_upper_log_score(
        *prepared.classes, *prepared.model, class_indexes);
    results.push_back(dfs_build_spelling(
        *prepared.classes, *prepared.model, prepared.solo_words.get(),
        class_indexes, member_indexes, representative, args.show_bonus));
  }
  if (input->bad()) {
    fprintf(stderr, "rerank-anagrams: can't read \"%s\"\n", name);
    return false;
  }

  std::sort(results.begin(), results.end(), dfs_spelling_better);
  if (args.common.top > 0 && results.size() > size_t(args.common.top))
    results.resize(size_t(args.common.top));
  return dfs_print_results(
      stdout, results, args.show_score, args.show_bonus,
      args.common.hide_solo_words ? NULL : prepared.solo_words.get());
}

}  // namespace

int main(int argc, char* argv[]) {
  dfs_reset_diagnostic_clock();
  dfs_set_diagnostic_stream(stderr);

  Args args;
  if (!parse_args(argv, &args)) return 2;

  DfsPairSet rejected;
  if (!load_rejections(args.reject_files, &rejected)) return 1;

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
    DfsPreparedClassList prepared;
    if (prepare_dfs_class_list(
            &reader, args.letters, args.common, args.exclude_pair_files,
            size_t(args.num_segments), &prepared, args.ptm)) {
      status = rerank_stream(
          input, input_name, args, rejected, prepared) ? 0 : 1;
    }
  }
  if (fclose(index) != 0 && status == 0) {
    fprintf(stderr, "rerank-anagrams: can't close index \"%s\"\n",
            args.index_file);
    return 1;
  }
  return status;
}
