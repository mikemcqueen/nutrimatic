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
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

static int const DEFAULT_TOP = 10000;

struct Args {
  char const* index_file;
  std::string letters;
  DfsCommonArgs common;
  DfsRepeatPolicy repeats;
  std::vector<std::string> exclude_pair_files;
  int max_combine_words;
  int num_segments;
  int64_t progress_factor;
  size_t score_cache_bytes;
  int preprocess_threads;
  int exact_letters;
  bool allow_cache_fallback;
  bool exact_remaining_depth;
  bool segments;
  bool show_score;
  bool show_bonus;
  bool weighted;
  bool ptm;
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

static size_t const HELP_DESCRIPTION_COLUMN = 38;
static size_t const HELP_LINE_WIDTH = 100;

static void help_spaces(size_t count) {
  while (count-- > 0) fputc(' ', stdout);
}

static void help_option(char const* option, char const* format, ...) {
  char description[4096];
  va_list args;
  va_start(args, format);
  vsnprintf(description, sizeof description, format, args);
  va_end(args);

  fputs("  ", stdout);
  fputs(option, stdout);
  size_t column = 2 + strlen(option);
  if (column >= HELP_DESCRIPTION_COLUMN) {
    fputc('\n', stdout);
    help_spaces(HELP_DESCRIPTION_COLUMN);
  } else {
    help_spaces(HELP_DESCRIPTION_COLUMN - column);
  }
  column = HELP_DESCRIPTION_COLUMN;

  char const* p = description;
  while (*p != '\0') {
    while (*p == ' ') ++p;
    char const* end = p;
    while (*end != '\0' && *end != ' ') ++end;
    size_t const word_length = size_t(end - p);
    if (column > HELP_DESCRIPTION_COLUMN &&
        column + 1 + word_length > HELP_LINE_WIDTH) {
      fputc('\n', stdout);
      help_spaces(HELP_DESCRIPTION_COLUMN);
      column = HELP_DESCRIPTION_COLUMN;
    } else if (column > HELP_DESCRIPTION_COLUMN) {
      fputc(' ', stdout);
      ++column;
    }
    fwrite(p, 1, word_length, stdout);
    column += word_length;
    p = end;
  }
  fputs("\n\n", stdout);
}

static void usage(char const* program) {
  fprintf(stdout, "usage: %s [-i INDEX] [options] letters\n\noptions:\n",
          program);
  help_option("-i, --idx INDEX",
      "read the completed Nutrimatic index from INDEX; workflow mode "
      "defaults to DIR/%s; required otherwise",
      WORKFLOW_INDEX_PATH);
  help_option("-u, --used-letters LETTERS",
      "subtract letters already used from the input letters before "
      "searching");
  help_option("--dict PATH",
      "filter entries to words in the dictionary; workflow mode defaults "
      "to DIR/%s", WORKFLOW_DICT_PATH);
  help_option("-m, --min-word-length N",
      "minimum word length (default: %d; 0 for no minimum)",
      DFS_DEFAULT_MIN_WORD_LEN);
  help_option("-g, --num-segments N",
      "return only results using exactly N index entries (default: 0, any "
      "number); every result then carries the same (corpus-total * "
      "P)^(N-1) divisor, so -P cannot change their order and scores are "
      "comparable only within one -g run");
  help_option("-n, --top N",
      "maximum number of results (default: %d; 0 returns all results)",
      DEFAULT_TOP);
  help_option("-x, --max-extract-words N",
      "explore at most N words inside one index entry (default: 2; 0 means "
      "no limit)");
  help_option("--pairs FILE",
      "load one \"word\" or \"word,word\" entry per line; a pair containing "
      "a word shorter than -m is matched only in written order during "
      "extraction, while other pairs match in either order; every loaded "
      "entry must contain at least -m normalized non-space characters in "
      "total; eligible listed pairs absent from the index are admitted with "
      "corpus count 1, but standalone entries are not; dictionary, bag, -m, "
      "-x, and exclusion rules still apply");
  help_option("--seed-pairs FILE",
      "load a repeatable fixed pair-bonus tier of %.2f",
      DFS_SEED_PAIR_BONUS);
  help_option("--yes-pairs FILE",
      "load a repeatable fixed pair-bonus tier of %.2f",
      DFS_YES_PAIR_BONUS);
  help_option("--best-pairs FILE",
      "mark BEST entries; may be given once");
  help_option("--more-best-pairs FILE",
      "mark additional BEST entries; may be repeated; with -g N, BEST-marked "
      "segments receive descending exponents from N through 1, counted once "
      "per segment; without -g the exponent remains fixed at %.2f; duplicate "
      "and reversed pairs retain the strongest tier; weighted pair options "
      "cannot be combined with legacy --pairs",
      DFS_BEST_PAIR_BONUS);
  help_option("--wf",
      "use the nonempty WFROOT environment variable as the workflow root");
  help_option("--wfroot DIR",
      "use DIR as a workflow root; loads DIR/%s as YES pairs and requires "
      "either --seed-pairs or -t beginning with sN; also excludes DIR/%s and "
      "a complete target's %s as if each were an --exclude-pairs file, "
      "skipping either when absent; BEST pair words missing from the "
      "dictionary are added to it, with a stderr notice for each",
      WORKFLOW_YES_PAIRS_PATH, WORKFLOW_NO_PAIRS_PATH,
      WORKFLOW_TARGET_NO_PAIRS_NAME);
  help_option("-t, --target TARGET",
      "select a prefix of sN/[ou]-letters/mN/gN; its sentence seed is "
      "auto-loaded, and a complete target also loads its optional %s unless "
      "--best-pairs replaces it", WORKFLOW_TARGET_BEST_PAIRS_NAME);
  help_option("--exclude-pairs FILE|WORKFLOW-DIR",
      "load word pairs, one \"word,word\" line each, and drop every index "
      "entry spelled exactly like one in either order, so no result can "
      "contain it; may be repeated, but only one argument may be a directory; "
      "the test is whole-entry equality, so a longer entry containing the "
      "pair is kept, and -x 2 is what confines entries to the two words this "
      "compares; a directory resolves to DIR/%s and must hold a .wf "
      "subdirectory", WORKFLOW_NO_PAIRS_PATH);
  help_option("--solo-words WORD[,WORD...]",
      "supply up to 16 unique lowercase external words; they consume no "
      "letters and matched partners are printed in parentheses; a selected "
      "single-word entry earns --word-bonus when either phrase order is an "
      "aggregate index phrase or is asserted by a pair input, and an asserted "
      "pair also earns its source's pair bonus; each solo word can be used "
      "once per answer; both bonuses must be non-negative, and the aggregate "
      "phrase test matches phase 1");
  help_option("--hide-solo-words",
      "omit parenthesized solo partners from output");
  help_option("--no-repeat WORD|PAIR",
      "limit one entry to a single use per result; may be repeated; a value "
      "with no space is a word and is counted wherever it falls, including "
      "inside a multi-word segment; a value with a space is a whole segment "
      "matched in written order, and naming the other order takes a second "
      "value");
  help_option("--disable-repeats",
      "apply the same test to every word: no word may occur twice in a "
      "result, so \"hot dog\" beside \"dog house\" is rejected; occurrences "
      "need not be in different entries, so a self-repeating entry like "
      "\"step by step\" is rejected on its own; only whole words count, so "
      "\"dog\" beside \"god\" or \"dogma\" is not a repeat");
  help_option("-p, --progress-factor N",
      "report search progress every 100000 * N operations (default: 1; must "
      "be at least 1)");
  help_option("-C, --cache-size MiB",
      "set the projected-score cache size (default: %zu MiB; 0 disables it "
      "with -F)", DFS_DEFAULT_SCORE_CACHE_MIB);
  help_option("-T, --preprocess-threads N",
      "set preprocessing threads (default: 0, automatic for 26+ letters; 1 "
      "disables threaded preprocessing)");
  help_option("-S, --search-threads N",
      "set search threads (default: 0, hardware threads)");
  help_option("-d, --projection-depth N",
      "keep this many rarest letter types exact in the projected cache; the "
      "default is the largest depth that fits -C");
  help_option("--exact",
      "index projected score bounds by the exact number of segments "
      "remaining; requires -g N and uses N-1 values per projected state for "
      "N greater than 1, instead of one");
  help_option("-P, --segment-penalty P",
      "divide the score by P for each selected index entry after the first; "
      "P must be at least 1 (default: %.0f); k entries score as product(count) "
      "/ (corpus-total * P)^(k-1)", DFS_DEFAULT_SEGMENT_PENALTY);
  help_option("--word-bonus N",
      "multiply each multi-word index entry by %.0f^N (default: %.1f); at N=1 "
      "a multi-word entry earns back the default -P it costs, so adding one "
      "as a further entry is free",
      DFS_WORD_BONUS_BASE, DFS_DEFAULT_WORD_BONUS);
  help_option("--pair-bonus N",
      "multiply each index entry found in --pairs by %.0f^N (default: %.1f)",
      DFS_PAIR_BONUS_BASE, DFS_DEFAULT_PAIR_BONUS);
  help_option("--segments",
      "print the index entries used by the results instead of the results, as "
      "best-score, result-count and text, by descending best score");
  help_option("--show-bonus",
      "add one marker per segment between the score and anagram: W for word "
      "bonus, P for legacy pair bonus, S/Y/B for pair sources, and - for none; "
      "cannot be combined with --segments");
  help_option("--no-score",
      "omit the leading score from each result; the output cannot be read by "
      "filter-segments or rerank-anagrams; cannot be combined with "
      "--segments");
  help_option("--weighted",
      "sort and report each segment by best-score times result-count instead "
      "of best score alone; requires --segments");
  help_option("--ptm",
      "recalibrate the base count of every index entry, before any bonus, "
      "onto a scale whose upper tail is normal rather than exponential; phase "
      "1 fits the spread of the counts this bag reaches and replaces each "
      "log(count) with the log count carrying the same deviation in a normal "
      "batch, so one very frequent entry no longer dominates a result; the "
      "segment penalty and every bonus keep their meaning in the same "
      "log-count units; the fit covers only entries this bag reaches, so "
      "scores are comparable only within one run");
  help_option("-F, --allow-cache-fallback",
      "allow score-cache fallback when the requested table does not fit");
  help_option("-v, --verbose", "report search task splitting");
}

static int const OPT_SEGMENTS = 256;
static int const OPT_WEIGHTED = 257;
static int const OPT_EXCLUDE_PAIRS = 258;
static int const OPT_SHOW_BONUS = 259;
static int const OPT_NO_SCORE = 260;
static int const OPT_PTM = 261;
static int const OPT_NO_REPEAT = 262;
static int const OPT_DISABLE_REPEATS = 263;
static int const OPT_EXACT = 264;

// Normalizes a --no-repeat value the way the dictionary loader normalizes a
// line, keeping the word boundaries load_dictionary() has no use for.
static std::string normalize_repeat_value(char const* value) {
  std::string out;
  bool separated = false;
  for (char const* p = value; *p != '\0'; ++p) {
    unsigned char const ch = (unsigned char) *p;
    char kept;
    if (ch >= 'A' && ch <= 'Z') {
      kept = char(ch - 'A' + 'a');
    } else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
      kept = char(ch);
    } else {
      separated = separated || (ch == ' ' || ch == '\t');
      continue;
    }
    if (separated && !out.empty()) out.push_back(' ');
    separated = false;
    out.push_back(kept);
  }
  return out;
}

static struct optparse_long const long_options[] = {
  DFS_COMMON_LONG_OPTIONS,
  { "idx", 'i', OPTPARSE_REQUIRED },
  { "exclude-pairs", OPT_EXCLUDE_PAIRS, OPTPARSE_REQUIRED },
  { "num-segments", 'g', OPTPARSE_REQUIRED },
  { "progress-factor", 'p', OPTPARSE_REQUIRED },
  { "cache-size", 'C', OPTPARSE_REQUIRED },
  { "preprocess-threads", 'T', OPTPARSE_REQUIRED },
  { "projection-depth", 'd', OPTPARSE_REQUIRED },
  { "exact", OPT_EXACT, OPTPARSE_NONE },
  { "segments", OPT_SEGMENTS, OPTPARSE_NONE },
  { "show-bonus", OPT_SHOW_BONUS, OPTPARSE_NONE },
  { "no-score", OPT_NO_SCORE, OPTPARSE_NONE },
  { "weighted", OPT_WEIGHTED, OPTPARSE_NONE },
  { "ptm", OPT_PTM, OPTPARSE_NONE },
  { "no-repeat", OPT_NO_REPEAT, OPTPARSE_REQUIRED },
  { "disable-repeats", OPT_DISABLE_REPEATS, OPTPARSE_NONE },
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
  out->exact_remaining_depth = false;
  out->segments = false;
  out->show_score = true;
  out->show_bonus = false;
  out->weighted = false;
  out->ptm = false;
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
      case OPT_NO_SCORE:
        out->show_score = false;
        break;
      case OPT_WEIGHTED:
        out->weighted = true;
        break;
      case OPT_PTM:
        out->ptm = true;
        break;
      case OPT_NO_REPEAT: {
        if (strchr(options.optarg, ',') != NULL) {
          fprintf(stderr,
              "error: --no-repeat does not take a comma-separated list: %s\n",
              options.optarg);
          return false;
        }
        std::string const value = normalize_repeat_value(options.optarg);
        if (value.empty()) {
          fprintf(stderr,
              "error: --no-repeat value is empty after normalization: %s\n",
              options.optarg);
          return false;
        }
        std::vector<std::string>& named =
            value.find(' ') == std::string::npos
                ? out->repeats.words : out->repeats.pairs;
        if (std::find(named.begin(), named.end(), value) == named.end())
          named.push_back(value);
        break;
      }
      case OPT_DISABLE_REPEATS:
        out->repeats.disable_repeats = true;
        break;
      case OPT_EXACT:
        out->exact_remaining_depth = true;
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

  if (out->exact_remaining_depth && out->num_segments == 0) {
    fputs("error: --exact requires -g N\n", stderr);
    return false;
  }

  if (!finalize_dfs_common_args(
          &out->common, argv[0], &out->index_file,
          &out->exclude_pair_files))
    return false;

  if (out->weighted && !out->segments) {
    fputs("error: --weighted requires --segments\n", stderr);
    return false;
  }
  if (out->show_bonus && out->segments) {
    fputs("error: --show-bonus cannot be combined with --segments\n", stderr);
    return false;
  }
  if (!out->show_score && out->segments) {
    fputs("error: --no-score cannot be combined with --segments\n", stderr);
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
          size_t(args.num_segments), &prepared, args.ptm))
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

  DfsAnagramSearch search(
      prepared.classes.get(), args.letters, *prepared.model,
      args.score_cache_bytes, preprocess_threads,
      search_threads, size_t(args.num_segments),
      args.exact_remaining_depth);
  DfsTopN output(
      prepared.classes.get(), prepared.model.get(), size_t(args.common.top),
      prepared.solo_words.get(),
      args.show_bonus, &args.repeats);
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
            stdout, results, args.show_score, args.show_bonus,
            args.common.hide_solo_words
                ? NULL : prepared.solo_words.get()))
      return 1;
  }
  return 0;
}
