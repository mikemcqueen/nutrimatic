#include "dfs-class-list-build.h"
#include "dfs-cli-args.h"
#include "dfs-diagnostic.h"
#include "dfs-output.h"
#include "dfs-search-stats.h"
#include "dfs-search.h"
#include "index.h"
#include "optparse.h"
#include "segment-report.h"
#include "workflow-paths.h"

#include <math.h>
#include <stdio.h>

#include <algorithm>
#include <string>
#include <vector>

static int const DEFAULT_TOP = 10000;

struct Args {
  char const* index_file;
  std::string letters;
  DfsCommonArgs common;
  std::vector<std::string> exclude_pair_files;
  int max_combine_words;
  int num_segments;
  int64_t progress_factor;
  size_t score_cache_bytes;
  int preprocess_threads;
  int exact_letters;
  bool allow_cache_fallback;
  bool segments;
  bool show_bonus;
  bool weighted;
  bool verbose;
};

static void report_segments(std::vector<DfsSpelling> const& results,
                            bool weighted) {
  SegmentReport report;
  for (size_t i = 0; i < results.size(); ++i) {
    DfsSpelling const& result = results[i];
    size_t offset = 0;
    for (size_t s = 0; s < result.segment_lengths.size(); ++s) {
      size_t const length = result.segment_lengths[s];
      segment_report_add(&report, result.text.substr(offset, length),
                         result.log_score);
      offset += length + 1;
    }
  }
  segment_report_print(
      stdout, weighted ? segment_report_weighted(report) : report);
}

static void usage(char const* program) {
  fprintf(stdout,
      "usage: %s [-i INDEX] letters"
      " [-u used-letters] [--dict PATH] [-m min-word-length]"
      " [-g num-segments] [-n top]"
      " [-x max-extract-words] [--pairs FILE]"
      " [--seed-pairs FILE]... [--yes-pairs FILE]..."
      " [--best-pairs FILE] [--more-best-pairs FILE]..."
      " [--wf|--wfroot DIR] [-t TARGET]"
      " [--exclude-pairs FILE|WORKFLOW-DIR]..."
      " [--solo-words WORD[,WORD...]]"
      " [--hide-solo-words]"
      " [-p progress-factor] [--cache-size MiB]"
      " [--preprocess-threads N] [--search-threads N]"
      " [-d projection-depth]"
      " [-P segment-penalty] [--word-bonus N] [--pair-bonus N]"
      " [--segments] [--show-bonus] [--weighted]"
      " [-F|--allow-cache-fallback] [-v|--verbose]\n"
      "  -i, --idx INDEX reads the completed Nutrimatic index from INDEX;"
      " workflow mode defaults to DIR/%s; required otherwise\n"
      "  -u, --used-letters LETTERS subtracts letters already used from the"
      " input letters before searching\n"
      "  -m defaults to %d; 0 for no minimum\n"
      "  -n defaults to %d; 0 returns all results\n"
      "  --dict PATH filters entries to words in the dictionary; workflow"
      " mode defaults to DIR/%s\n"
      "  -g, --num-segments N returns only results using exactly N index"
      " entries; defaults to 0 (any number)\n"
      "    every result then carries the same (corpus-total * P)^(N-1)"
      " divisor, so -P cannot change their order and scores are comparable"
      " only within one -g run\n"
      "  -x, --max-extract-words N explores at most N words inside one index"
      " entry; defaults to 2; 0 means no limit\n"
      "  --pairs FILE loads one \"word\" or \"word,word\" entry per line\n"
      "    during extraction, a pair containing a word shorter than -m is"
      " matched only in written order; other pairs match in either order\n"
      "    every loaded entry must contain at least -m normalized non-space"
      " characters in total\n"
      "    eligible listed pairs absent from the index are admitted with"
      " corpus count 1; standalone entries are not\n"
      "    dictionary, bag, -m, -x, and exclusion rules still apply\n"
      "  --seed-pairs FILE and --yes-pairs FILE load fixed pair-bonus tiers"
      " %.2f and %.2f; --best-pairs FILE and --more-best-pairs FILE mark"
      " BEST entries\n"
      "    with -g N, BEST-marked segments receive descending exponents from"
      " N through 1, counted once per segment; without -g the exponent"
      " remains fixed at %.2f; each option except --best-pairs may be"
      " repeated\n"
      "    duplicates and reversed pairs retain the strongest tier; these"
      " options cannot be combined with legacy --pairs\n"
      "  --wfroot DIR uses DIR as a workflow root; --wf is an alias using"
      " the nonempty WFROOT environment variable\n"
      "    workflow mode loads DIR/%s as YES pairs and"
      " requires either --seed-pairs or -t beginning with sN\n"
      "    it also excludes DIR/%s, and a complete target's %s, as if each"
      " were an --exclude-pairs file; either is"
      " skipped when absent\n"
      "    BEST pair words missing from the dictionary are added to it,"
      " with a stderr notice for each\n"
      "  -t, --target TARGET selects a prefix of sN/[ou]-letters/mN/gN;"
      " its sentence seed is auto-loaded, and a complete target also loads"
      " its optional %s unless --best-pairs replaces it\n"
      "  --exclude-pairs FILE|WORKFLOW-DIR loads word pairs, one"
      " \"word,word\" line each, and drops every index entry spelled exactly"
      " like one, in either order, so"
      " no result can contain it\n"
      "    may be repeated to combine inputs, but only one argument may be a"
      " directory\n"
      "    the test is whole-entry equality, so a longer entry containing the"
      " pair is kept; -x 2 is what confines entries to the two words this"
      " compares\n"
      "    a directory resolves to DIR/%s and must"
      " hold a .wf subdirectory\n"
      "  --solo-words WORD[,WORD...] supplies up to 16 unique lowercase"
      " external words; they consume no letters and matched partners are"
      " printed in parentheses\n"
      "    a selected single-word entry earns --word-bonus when either"
      " phrase order is an aggregate index phrase or is asserted by"
      " a pair input; an asserted pair also earns its source's pair bonus\n"
      "    each solo word can be used once per answer; both bonuses must be"
      " non-negative, and the aggregate phrase test matches phase 1\n"
      "  --hide-solo-words omits parenthesized solo partners from output\n"
      "  -C, --cache-size defaults to %zu MiB; 0 disables it with -F\n"
      "  --preprocess-threads defaults to 0: automatic for 26+ letters;"
      " 1 disables it\n"
      "  -S, --search-threads defaults to 0 (hardware threads)\n"
      "  -d, --projection-depth keeps this many rarest letter types exact in"
      " the projected cache; the default is the largest depth that fits -C\n"
      "  -P, --segment-penalty P divides the score by P for each selected"
      " index entry after the first; P must be at least 1 and defaults to"
      " %.0f\n"
      "    k entries score as product(count) / (corpus-total * P)^(k-1)\n"
      "  --word-bonus N multiplies each multi-word index entry by %.0f^N;"
      " defaults to %.1f\n"
      "    at N=1 a multi-word entry earns back the default -P it costs, so"
      " adding one as a further entry is free\n"
      "  --pair-bonus N multiplies each index entry found in --pairs by"
      " %.0f^N; defaults to %.1f\n"
      "  --segments prints the index entries used by the results instead of"
      " the results, as best-score, result-count and text, by descending"
      " best score\n"
      "  --show-bonus adds one marker per segment between the score and"
      " anagram: W for word bonus, P for legacy pair bonus, S/Y/B for pair"
      " sources, and - for none; cannot be combined with --segments\n"
      "  --weighted sorts and reports each segment by best-score times"
      " result-count instead of best score alone; requires --segments\n"
      "  -F, --allow-cache-fallback allows score-cache fallback when the"
      " requested table does not fit\n"
      "  -v, --verbose reports search task splitting\n",
      program, WORKFLOW_INDEX_PATH, DFS_DEFAULT_MIN_WORD_LEN, DEFAULT_TOP,
      WORKFLOW_DICT_PATH,
      DFS_SEED_PAIR_BONUS, DFS_YES_PAIR_BONUS, DFS_BEST_PAIR_BONUS,
      WORKFLOW_YES_PAIRS_PATH, WORKFLOW_NO_PAIRS_PATH,
      WORKFLOW_TARGET_NO_PAIRS_NAME, WORKFLOW_TARGET_BEST_PAIRS_NAME,
      WORKFLOW_NO_PAIRS_PATH,
      DFS_DEFAULT_SCORE_CACHE_MIB, DFS_DEFAULT_SEGMENT_PENALTY,
      DFS_WORD_BONUS_BASE, DFS_DEFAULT_WORD_BONUS, DFS_PAIR_BONUS_BASE,
      DFS_DEFAULT_PAIR_BONUS);
}

static int const OPT_SEGMENTS = 256;
static int const OPT_WEIGHTED = 257;
static int const OPT_EXCLUDE_PAIRS = 258;
static int const OPT_SHOW_BONUS = 259;

static struct optparse_long const long_options[] = {
  DFS_COMMON_LONG_OPTIONS,
  { "idx", 'i', OPTPARSE_REQUIRED },
  { "exclude-pairs", OPT_EXCLUDE_PAIRS, OPTPARSE_REQUIRED },
  { "num-segments", 'g', OPTPARSE_REQUIRED },
  { "progress-factor", 'p', OPTPARSE_REQUIRED },
  { "cache-size", 'C', OPTPARSE_REQUIRED },
  { "preprocess-threads", 'T', OPTPARSE_REQUIRED },
  { "projection-depth", 'd', OPTPARSE_REQUIRED },
  { "segments", OPT_SEGMENTS, OPTPARSE_NONE },
  { "show-bonus", OPT_SHOW_BONUS, OPTPARSE_NONE },
  { "weighted", OPT_WEIGHTED, OPTPARSE_NONE },
  { "allow-cache-fallback", 'F', OPTPARSE_NONE },
  { "verbose", 'v', OPTPARSE_NONE },
  { NULL, 0, OPTPARSE_NONE },
};

static bool parse_args(char* argv[], Args* out) {
  out->index_file = NULL;
  out->common = DfsCommonArgs();
  out->common.top = DEFAULT_TOP;
  out->common.max_extract_words = 2;
  out->common.search_threads = 0;
  out->exclude_pair_files.clear();
  out->num_segments = 0;
  out->progress_factor = 1;
  out->score_cache_bytes = DFS_DEFAULT_SCORE_CACHE_MIB * DFS_MIB;
  out->preprocess_threads = 0;
  out->exact_letters = -1;
  out->allow_cache_fallback = false;
  out->segments = false;
  out->show_bonus = false;
  out->weighted = false;
  out->verbose = false;

  struct optparse options;
  optparse_init(&options, argv);

  int opt;
  while ((opt = optparse_long(&options, long_options, NULL)) != -1) {
    switch (dfs_parse_common_option(opt, &options, &out->common, NULL)) {
      case DFS_OPTION_ERROR:
        return false;
      case DFS_OPTION_HANDLED:
        continue;
      case DFS_OPTION_OTHER:
        break;
    }
    switch (opt) {
      case 'i':
        out->index_file = options.optarg;
        break;
      case 'g':
        if (!parse_count(options.optarg, "--num-segments",
                         &out->num_segments))
          return false;
        break;
      case 'p':
        if (!parse_count64(options.optarg, "--progress-factor",
                           &out->progress_factor))
          return false;
        if (out->progress_factor < 1) {
          fputs("error: --progress-factor must be at least 1\n", stderr);
          return false;
        }
        break;
      case 'C':
        if (!parse_mib(options.optarg, "--cache-size",
                       &out->score_cache_bytes))
          return false;
        break;
      case 'T':
        if (!parse_count(options.optarg, "--preprocess-threads",
                         &out->preprocess_threads))
          return false;
        break;
      case 'd':
        if (!parse_count(options.optarg, "--projection-depth",
                         &out->exact_letters))
          return false;
        break;
      case OPT_EXCLUDE_PAIRS:
        out->exclude_pair_files.push_back(options.optarg);
        break;
      case OPT_SEGMENTS:
        out->segments = true;
        break;
      case OPT_SHOW_BONUS:
        out->show_bonus = true;
        break;
      case OPT_WEIGHTED:
        out->weighted = true;
        break;
      case 'F':
        out->allow_cache_fallback = true;
        break;
      case 'v':
        out->verbose = true;
        break;
      default:
        fprintf(stderr, "error: %s\n", options.errmsg);
        usage(argv[0]);
        return false;
    }
  }

  if (!validate_solo_bonuses(out->common)) return false;
  if (!finalize_dfs_workflow_args(
          &out->common, argv[0], &out->index_file))
    return false;
  if (!collect_workflow_exclude_pair_files(
          out->common, argv[0], &out->exclude_pair_files))
    return false;

  if (out->weighted && !out->segments) {
    fputs("error: --weighted requires --segments\n", stderr);
    return false;
  }
  if (out->show_bonus && out->segments) {
    fputs("error: --show-bonus cannot be combined with --segments\n", stderr);
    return false;
  }

  char const* letters = optparse_arg(&options);
  if (out->index_file == NULL || letters == NULL ||
      optparse_arg(&options) != NULL) {
    usage(argv[0]);
    return false;
  }

  std::string bag;
  std::string remove;
  if (!clean_letters(letters, "letters", &bag)) return false;
  if (!clean_letters(out->common.used_letters.c_str(), "used letters",
                     &remove))
    return false;

  if (!subtract_letters(bag, remove, &out->letters)) return false;
  if (!check_bag_length(out->letters)) return false;

  if (!finalize_min_word_length(
          out->letters, out->common.min_word_len_given, &out->common.min_word_len))
    return false;

  // Words across the whole answer, which is also what phase 2 derives its
  // maximum depth from; --max-extract-words bounds words within one entry
  // instead and leaves this alone. 0 means there is no minimum to divide by.
  out->max_combine_words = out->common.min_word_len > 1
      ? int(out->letters.size()) / out->common.min_word_len
      : 0;
  return true;
}

int main(int argc, char* argv[]) {
  dfs_reset_diagnostic_clock();
  dfs_set_diagnostic_stream(stderr);

  Args args;
  if (!parse_args(argv, &args)) return 2;
  if (args.common.pair_file == NULL) args.common.pair_bonus = 0.0;

  size_t const preprocess_threads = resolve_preprocess_threads(
      args.preprocess_threads, args.letters.size());
  size_t const search_threads = resolve_search_threads(
      args.common.search_threads);
  dfs_diagnostic(
      "depth %d top %d threads %zu search threads %zu cache %zu "
      "segment penalty %.17g\n",
      args.exact_letters, args.common.top, preprocess_threads, search_threads,
      args.score_cache_bytes / DFS_MIB, args.common.segment_penalty);

  FILE* fp = fopen(args.index_file, "rb");
  if (fp == NULL) {
    fprintf(stderr, "error: can't open \"%s\"\n", args.index_file);
    return 1;
  }

  IndexReader reader(fp);
  DfsPreparedClassList prepared;
  if (!prepare_dfs_class_list(
          &reader, args.letters, args.common, args.exclude_pair_files,
          size_t(args.num_segments), &prepared))
    return 1;

  // Both headers carry the segment constraint, since -g is independent of
  // whether a minimum word length narrowed the search.
  char segments_note[64];
  segments_note[0] = '\0';
  if (args.num_segments > 0) {
    snprintf(segments_note, sizeof segments_note, ", exactly %d segment%s",
             args.num_segments, args.num_segments == 1 ? "" : "s");
  }
  bool const active_short_pair_exception =
      !prepared.exception_prefixes.empty() &&
      (args.common.max_extract_words <= 0 ||
       args.common.max_extract_words >= 2);
  if (active_short_pair_exception) {
    dfs_diagnostic(
        "%zu letters \"%s\", entries of %d+ letters, at most %d segment%s%s\n",
        args.letters.size(), args.letters.c_str(), args.common.min_word_len,
        args.max_combine_words, args.max_combine_words == 1 ? "" : "s",
        segments_note);
  } else if (args.max_combine_words > 0) {
    dfs_diagnostic(
        "%zu letters \"%s\", words of %d+, at most %d word%s%s\n",
        args.letters.size(), args.letters.c_str(), args.common.min_word_len,
        args.max_combine_words, args.max_combine_words == 1 ? "" : "s",
        segments_note);
  } else {
    dfs_diagnostic("%zu letters \"%s\", no minimum word length%s\n",
                   args.letters.size(), args.letters.c_str(), segments_note);
  }

  if (args.common.max_extract_words > 0)
    dfs_diagnostic("at most %d word%s per index entry\n",
                   args.common.max_extract_words,
                   args.common.max_extract_words == 1 ? "" : "s");

  DfsBestBonusPolicy const best_bonus =
      dfs_best_bonus_policy(size_t(args.num_segments));
  DfsAnagramSearch search(
      prepared.classes.get(), args.letters, args.common.segment_penalty,
      reader.count(),
      args.score_cache_bytes, preprocess_threads,
      search_threads, size_t(args.num_segments),
      args.common.word_bonus, args.common.pair_bonus, best_bonus);
  DfsTopN output(
      prepared.classes.get(), prepared.model.get(), size_t(args.common.top),
      prepared.solo_words.get(),
      args.show_bonus);
  DfsSearchStats stats;
  if (!search.run(&output, &stats,
                  args.progress_factor, args.allow_cache_fallback,
                  args.exact_letters, args.verbose))
    return 2;
  dfs_diagnostic(
      "phase 2 timing: %.1fs setup, %.1fs search, "
      "%llu successful bound transitions, %llu nextafter calls\n",
      stats.execution.setup_seconds,
      stats.execution.search_seconds,
      (unsigned long long) stats.bounds.projected.transitions,
      (unsigned long long) stats.bounds.projected.nextafter_calls);
  if (stats.bounds.mode == DFS_SCORE_BOUND_PROJECTED)
    dfs_diagnostic(
        "phase 2 projected work: %llu candidate tests, "
        "%llu fitting transitions\n",
        (unsigned long long) stats.bounds.projected.candidate_tests,
        (unsigned long long) stats.bounds.projected.fitting_transitions);
  if (stats.certificate.ready) {
    dfs_diagnostic(
        "phase 2 length certificate: %s, %zu table bytes\n",
        stats.certificate.skipping() ? "active" : "shadow",
        stats.certificate.table_bytes);
    dfs_diagnostic(
        "phase 2   %llu group tests, %llu rejected, "
        "%llu class scans kept, %llu skipped\n",
        (unsigned long long) stats.certificate.counters.group_tests,
        (unsigned long long) stats.certificate.counters.group_rejects,
        (unsigned long long) stats.certificate.counters.scans_kept,
        (unsigned long long) stats.certificate.counters.scans_skipped);
  }
  if (stats.execution.search_threads > 1)
    dfs_diagnostic(
        "phase 2 search parallelism: %zu requested, %zu used, "
        "%llu tasks\n",
        search_threads, stats.execution.search_threads,
        (unsigned long long) stats.execution.search_tasks);
  dfs_diagnostic(
      "phase 2 score cache: %zu bound entries, %zu bound bytes\n",
      stats.bounds.entries, stats.bounds.bytes_charged);
  dfs_diagnostic(
      "phase 2 complete: %lld nodes, %lld solutions, "
      "%zu spellings expanded, %zu retained\n",
      (long long) stats.all_solutions.nodes,
      (long long) stats.all_solutions.solutions,
      output.spellings_expanded(), output.size());
  fflush(stderr);

  std::vector<DfsSpelling> const results = output.take_sorted_results();
  if (args.segments) {
    report_segments(results, args.weighted);
  } else {
    if (!dfs_print_results(
            stdout, results, args.show_bonus,
            args.common.hide_solo_words
                ? NULL : prepared.solo_words.get()))
      return 1;
  }
  return 0;
}
