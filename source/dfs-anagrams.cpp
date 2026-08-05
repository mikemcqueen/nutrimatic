#include "dfs-class-list.h"
#include "dfs-cli-args.h"
#include "dfs-diagnostic.h"
#include "dfs-output.h"
#include "dfs-score.h"
#include "dfs-search-stats.h"
#include "dfs-search.h"
#include "index.h"
#include "optparse.h"
#include "segment-report.h"

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
  int max_combine_words;
  int num_segments;
  int64_t progress_factor;
  size_t score_cache_bytes;
  int preprocess_threads;
  int exact_letters;
  bool allow_cache_fallback;
  bool segments;
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
  fprintf(stderr,
      "usage: %s input.index letters"
      " [-u used-letters] [--dict PATH] [-m min-word-length]"
      " [-g num-segments] [-n top]"
      " [-x max-extract-words] [--pairs]"
      " [-p progress-factor] [--cache-size MiB]"
      " [--preprocess-threads N] [--search-threads N]"
      " [-d projection-depth]"
      " [-P segment-penalty] [--segments] [--weighted]"
      " [-F|--allow-cache-fallback] [-v|--verbose]\n"
      "  -m defaults to %d; 0 for no minimum\n"
      "  -n defaults to %d; 0 returns all results\n"
      "  --dict PATH filters entries to words in the dictionary\n"
      "  -g, --num-segments N returns only results using exactly N index"
      " entries; defaults to 0 (any number)\n"
      "    every result then carries the same (corpus-total * P)^(N-1)"
      " divisor, so -P cannot change their order and scores are comparable"
      " only within one -g run\n"
      "  -x, --max-extract-words N explores at most N words inside one index"
      " entry; defaults to 0 (no limit)\n"
      "  --pairs is shorthand for --max-extract-words 2\n"
      "  -C, --cache-size defaults to %zu MiB; 0 disables it with -F\n"
      "  --preprocess-threads defaults to 0: automatic for 26+ letters;"
      " 1 disables it\n"
      "  -S, --search-threads defaults to 1\n"
      "  -d, --projection-depth keeps this many rarest letter types exact in"
      " the projected cache; the default is the largest depth that fits -C\n"
      "  -P, --segment-penalty P divides the score by P for each selected"
      " index entry after the first; P must be at least 1 and defaults to"
      " %.0f\n"
      "    k entries score as product(count) / (corpus-total * P)^(k-1)\n"
      "  --segments prints the index entries used by the results instead of"
      " the results, as best-score, result-count and text, by descending"
      " best score\n"
      "  --weighted sorts and reports each segment by best-score times"
      " result-count instead of best score alone; requires --segments\n"
      "  -F, --allow-cache-fallback allows score-cache fallback when the"
      " requested table does not fit\n"
      "  -v, --verbose reports search task splitting\n",
      program, DFS_DEFAULT_MIN_WORD_LEN, DEFAULT_TOP,
      DFS_DEFAULT_SCORE_CACHE_MIB, DFS_DEFAULT_SEGMENT_PENALTY);
}

static int const OPT_SEGMENTS = 256;
static int const OPT_WEIGHTED = 257;

static struct optparse_long const long_options[] = {
  DFS_COMMON_LONG_OPTIONS,
  { "num-segments", 'g', OPTPARSE_REQUIRED },
  { "progress-factor", 'p', OPTPARSE_REQUIRED },
  { "cache-size", 'C', OPTPARSE_REQUIRED },
  { "preprocess-threads", 'T', OPTPARSE_REQUIRED },
  { "projection-depth", 'd', OPTPARSE_REQUIRED },
  { "segments", OPT_SEGMENTS, OPTPARSE_NONE },
  { "weighted", OPT_WEIGHTED, OPTPARSE_NONE },
  { "allow-cache-fallback", 'F', OPTPARSE_NONE },
  { "verbose", 'v', OPTPARSE_NONE },
  { NULL, 0, OPTPARSE_NONE },
};

static bool parse_args(char* argv[], Args* out) {
  out->common = DfsCommonArgs();
  out->common.top = DEFAULT_TOP;
  out->num_segments = 0;
  out->progress_factor = 1;
  out->score_cache_bytes = DFS_DEFAULT_SCORE_CACHE_MIB * DFS_MIB;
  out->preprocess_threads = 0;
  out->exact_letters = -1;
  out->allow_cache_fallback = false;
  out->segments = false;
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
      case OPT_SEGMENTS:
        out->segments = true;
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

  if (out->weighted && !out->segments) {
    fputs("error: --weighted requires --segments\n", stderr);
    return false;
  }

  if (!dfs_finalize_common_args(&out->common)) return false;

  char const* index_file = optparse_arg(&options);
  char const* letters = optparse_arg(&options);
  if (index_file == NULL || letters == NULL ||
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

  out->index_file = index_file;
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

  size_t const preprocess_threads = resolve_preprocess_threads(
      args.preprocess_threads, args.letters.size());
  dfs_diagnostic(
      "depth %d top %d threads %zu search threads %d cache %zu "
      "segment penalty %.17g\n",
      args.exact_letters, args.common.top, preprocess_threads, args.common.search_threads,
      args.score_cache_bytes / DFS_MIB, args.common.segment_penalty);

  DfsDictionary dictionary;
  DfsDictionary const* dictionary_filter = NULL;
  if (args.common.dictionary_file != NULL) {
    if (!load_dictionary(args.common.dictionary_file, &dictionary)) return 1;
    dictionary_filter = &dictionary;
  }

  FILE* fp = fopen(args.index_file, "rb");
  if (fp == NULL) {
    fprintf(stderr, "error: can't open \"%s\"\n", args.index_file);
    return 1;
  }

  // Both headers carry the segment constraint, since -g is independent of
  // whether a minimum word length narrowed the search.
  char segments_note[64];
  segments_note[0] = '\0';
  if (args.num_segments > 0) {
    snprintf(segments_note, sizeof segments_note, ", exactly %d segment%s",
             args.num_segments, args.num_segments == 1 ? "" : "s");
  }
  if (args.max_combine_words > 0) {
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

  IndexReader reader(fp);
  DfsClassList classes(&reader, args.letters, args.common.min_word_len, true,
                       dictionary_filter, args.common.max_extract_words);
  dfs_diagnostic(
      "phase 1 complete: %zu entries, %zu classes, %lld trie nodes\n",
      classes.entry_count(), classes.classes().size(),
      (long long) classes.nodes_visited());
  fflush(stderr);

  DfsAnagramSearch search(
      &classes, args.letters, args.common.segment_penalty, reader.count(),
      args.score_cache_bytes, preprocess_threads,
      size_t(args.common.search_threads), size_t(args.num_segments));
  DfsTopN output(&classes, size_t(args.common.top));
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
        "phase 2 search parallelism: %d requested, %zu used, "
        "%llu tasks\n",
        args.common.search_threads, stats.execution.search_threads,
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
    for (size_t i = 0; i < results.size(); ++i)
      printf("%#.4g %s\n", exp(results[i].log_score),
             dfs_spelling_entry_list(results[i]).c_str());
  }
  return 0;
}
