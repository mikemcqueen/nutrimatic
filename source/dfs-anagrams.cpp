#include "dfs-class-list.h"
#include "dfs-output.h"
#include "dfs-search.h"
#include "index.h"
#include "optparse.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <string>
#include <thread>
#include <vector>

static bool clean_letters(char const* in, char const* what, std::string* out) {
  for (; *in != '\0'; ++in) {
    if (*in == ' ') continue;
    if ((*in < 'a' || *in > 'z') && (*in < '0' || *in > '9')) {
      fprintf(stderr, "error: bad character '%c' in %s\n", *in, what);
      return false;
    }
    out->push_back(*in);
  }
  return true;
}

static bool subtract_letters(std::string const& bag,
                             std::string const& used,
                             std::string* out) {
  int have[UCHAR_MAX + 1] = { 0 };
  for (size_t i = 0; i < bag.size(); ++i)
    ++have[(unsigned char) bag[i]];

  for (size_t i = 0; i < used.size(); ++i) {
    unsigned char const ch = (unsigned char) used[i];
    if (have[ch] == 0) {
      fprintf(stderr, "error: no '%c' left in \"%s\" to use\n",
              ch, bag.c_str());
      return false;
    }
    --have[ch];
  }

  out->clear();
  for (int ch = 0; ch <= UCHAR_MAX; ++ch)
    out->append(size_t(have[ch]), char(ch));

  if (out->empty()) {
    fputs("error: no letters left after removing used letters\n", stderr);
    return false;
  }
  return true;
}

static bool parse_count(char const* in, char const* what, int* out) {
  char* end;
  long const value = strtol(in, &end, 10);
  if (*in == '\0' || *end != '\0' || value < 0 || value > INT_MAX) {
    fprintf(stderr, "error: %s needs a count, not \"%s\"\n", what, in);
    return false;
  }
  *out = int(value);
  return true;
}

static int const DEFAULT_MIN_WORD_LEN = 4;
static int const DEFAULT_TOP = 10000;
static size_t const DEFAULT_SCORE_CACHE_MIB = 64;
static unsigned int const DEFAULT_MAX_PREPROCESS_THREADS = 20;
static size_t const MIB = size_t(1024) * size_t(1024);

static bool parse_mib(char const* in, char const* what, size_t* out) {
  if (*in == '\0' || *in == '-') {
    fprintf(stderr, "error: %s needs a count, not \"%s\"\n", what, in);
    return false;
  }
  errno = 0;
  char* end;
  unsigned long long const value = strtoull(in, &end, 10);
  if (*end != '\0' || errno == ERANGE ||
      value > static_cast<unsigned long long>(SIZE_MAX / MIB)) {
    fprintf(stderr, "error: %s needs a count, not \"%s\"\n", what, in);
    return false;
  }
  *out = size_t(value) * MIB;
  return true;
}

struct Args {
  char const* index_file;
  std::string letters;
  int min_word_len;
  int max_words;
  int top;
  int progress_factor;
  size_t score_cache_bytes;
  int preprocess_threads;
  bool allow_cache_fallback;
};

static void usage(char const* program) {
  fprintf(stderr,
      "usage: %s input.index letters"
      " [-u used-letters] [-m min-word-length] [-n top]"
      " [-p progress-factor] [--cache-size MiB]"
      " [--preprocess-threads N] [-F|--allow-cache-fallback]\n"
      "  -m defaults to %d; 0 for no minimum\n"
      "  -n defaults to %d\n"
      "  -C, --cache-size defaults to %zu MiB; 0 disables it with -F\n"
      "  --preprocess-threads defaults to 0: automatic for 26+ letters;"
      " 1 disables it\n"
      "  -F, --allow-cache-fallback allows score-cache fallback when the"
      " dense table does not fit\n",
      program, DEFAULT_MIN_WORD_LEN, DEFAULT_TOP,
      DEFAULT_SCORE_CACHE_MIB);
}

static struct optparse_long const long_options[] = {
  { "used-letters", 'u', OPTPARSE_REQUIRED },
  { "min-word-length", 'm', OPTPARSE_REQUIRED },
  { "top", 'n', OPTPARSE_REQUIRED },
  { "progress-factor", 'p', OPTPARSE_REQUIRED },
  { "cache-size", 'C', OPTPARSE_REQUIRED },
  { "preprocess-threads", 'T', OPTPARSE_REQUIRED },
  { "allow-cache-fallback", 'F', OPTPARSE_NONE },
  { NULL, 0, OPTPARSE_NONE },
};

static bool parse_args(char* argv[], Args* out) {
  out->min_word_len = DEFAULT_MIN_WORD_LEN;
  out->top = DEFAULT_TOP;
  out->progress_factor = 1;
  out->score_cache_bytes = DEFAULT_SCORE_CACHE_MIB * MIB;
  out->preprocess_threads = 0;
  out->allow_cache_fallback = false;

  struct optparse options;
  optparse_init(&options, argv);

  std::string used;
  bool min_word_len_given = false;
  int opt;
  while ((opt = optparse_long(&options, long_options, NULL)) != -1) {
    switch (opt) {
      case 'u':
        used += options.optarg;
        break;
      case 'm':
        if (!parse_count(options.optarg, "--min-word-length",
                         &out->min_word_len))
          return false;
        min_word_len_given = true;
        break;
      case 'n':
        if (!parse_count(options.optarg, "--top", &out->top))
          return false;
        break;
      case 'p':
        if (!parse_count(options.optarg, "--progress-factor",
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
      case 'F':
        out->allow_cache_fallback = true;
        break;
      default:
        fprintf(stderr, "error: %s\n", options.errmsg);
        usage(argv[0]);
        return false;
    }
  }

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
  if (!clean_letters(used.c_str(), "used letters", &remove)) return false;

  out->index_file = index_file;
  if (!subtract_letters(bag, remove, &out->letters)) return false;

  if (!min_word_len_given &&
      out->min_word_len > int(out->letters.size()))
    out->min_word_len = int(out->letters.size());

  if (out->min_word_len > int(out->letters.size())) {
    fprintf(stderr,
        "error: no word of %d letters fits in the %zu left in \"%s\"\n",
        out->min_word_len, out->letters.size(), out->letters.c_str());
    return false;
  }

  out->max_words = out->min_word_len > 1
      ? int(out->letters.size()) / out->min_word_len
      : 0;
  return true;
}

int main(int argc, char* argv[]) {
  Args args;
  if (!parse_args(argv, &args)) return 2;

  FILE* fp = fopen(args.index_file, "rb");
  if (fp == NULL) {
    fprintf(stderr, "error: can't open \"%s\"\n", args.index_file);
    return 1;
  }

  if (args.max_words > 0) {
    fprintf(stderr, "# %zu letters \"%s\", words of %d+, at most %d word%s\n",
            args.letters.size(), args.letters.c_str(), args.min_word_len,
            args.max_words, args.max_words == 1 ? "" : "s");
  } else {
    fprintf(stderr, "# %zu letters \"%s\", no minimum word length\n",
            args.letters.size(), args.letters.c_str());
  }

  IndexReader reader(fp);
  DfsClassList classes(&reader, args.letters, args.min_word_len);
  fprintf(stderr,
          "# phase 1 complete: %zu entries, %zu classes, %lld trie nodes\n",
          classes.entry_count(), classes.classes().size(),
          (long long) classes.nodes_visited());
  fflush(stderr);

  double const restart = 1e-6;
  size_t preprocess_threads = size_t(args.preprocess_threads);
  if (preprocess_threads == 0) {
    preprocess_threads = 1;
    if (args.letters.size() >= 26) {
      unsigned int const available = std::thread::hardware_concurrency();
      if (available > 1) {
        preprocess_threads = size_t(std::min(
            available, DEFAULT_MAX_PREPROCESS_THREADS));
      }
    }
  }
  DfsAnagramSearch search(
      &classes, args.letters, restart, reader.count(),
      args.score_cache_bytes, preprocess_threads);
  DfsTopN output(&classes, size_t(args.top));
  if (!search.run(args.top == 0 ? NULL : &output, stderr,
                  args.progress_factor, args.allow_cache_fallback))
    return 2;
  fprintf(stderr,
          "# phase 2 timing: %.6f s setup, %.6f s search, "
          "%llu successful bound transitions, %llu nextafter calls\n",
          search.phase_two_setup_seconds(),
          search.phase_two_search_seconds(),
          (unsigned long long) search.score_bound_transitions(),
          (unsigned long long) search.score_bound_nextafter_calls());
  if (search.projected_diagnostics_enabled()) {
    DfsAnagramSearch::ProjectedDiagnostics const& diagnostics =
        search.projected_diagnostics();
    fprintf(stderr,
            "# phase 2 projected edges: %llu scans, %llu wild-length "
            "rejects, %llu support rejects, %llu multiplicity rejects, "
            "%llu fitting, %llu dead-child\n",
            (unsigned long long) diagnostics.action_scans,
            (unsigned long long) diagnostics.wild_length_rejects,
            (unsigned long long) diagnostics.support_rejects,
            (unsigned long long) diagnostics.multiplicity_rejects,
            (unsigned long long) diagnostics.fitting_edges,
            (unsigned long long) diagnostics.dead_child_edges);
    fprintf(stderr,
            "# phase 2 projected cache: %llu ready-child hits, %llu states "
            "claimed, %llu ownership conflicts, %llu dependency spins, "
            "%llu finite states, %llu dead states\n",
            (unsigned long long) diagnostics.ready_child_hits,
            (unsigned long long) diagnostics.states_claimed,
            (unsigned long long) diagnostics.ownership_conflicts,
            (unsigned long long) diagnostics.dependency_spins,
            (unsigned long long) diagnostics.finite_states,
            (unsigned long long) diagnostics.dead_states);
    std::vector<DfsAnagramSearch::ProjectedLayerDiagnostics> const&
        layers = search.projected_layer_diagnostics();
    for (size_t letters_left = 0;
         letters_left < layers.size(); ++letters_left) {
      DfsAnagramSearch::ProjectedLayerDiagnostics const& layer =
          layers[letters_left];
      if (layer.outgoing_fitting_edges == 0 &&
          layer.incoming_dead_child_edges == 0 &&
          layer.finite_states == 0 &&
          layer.dead_states == 0)
        continue;
      fprintf(stderr,
              "# phase 2 projected layer %zu: %llu outgoing fitting, "
              "%llu incoming dead-child, %llu finite states, "
              "%llu dead states\n",
              letters_left,
              (unsigned long long) layer.outgoing_fitting_edges,
              (unsigned long long) layer.incoming_dead_child_edges,
              (unsigned long long) layer.finite_states,
              (unsigned long long) layer.dead_states);
    }
    fprintf(stderr,
            "# phase 2 projected coarse certificates: %llu checks, "
            "%llu fitting edges certified in traversal order, "
            "%llu skipped\n",
            (unsigned long long)
                diagnostics.coarse_certificate_checks,
            (unsigned long long)
                diagnostics.coarse_certificate_edges,
            (unsigned long long)
                diagnostics.coarse_certificate_skips);
  }
  if (search.length_certificate_enabled()) {
    unsigned long long const tests =
        (unsigned long long) search.length_certificate_group_tests();
    unsigned long long const rejects =
        (unsigned long long) search.length_certificate_group_rejects();
    unsigned long long const skipped =
        (unsigned long long) search.length_certificate_scans_skipped();
    unsigned long long const kept =
        (unsigned long long) search.length_certificate_scans_kept();
    unsigned long long const suffix =
        (unsigned long long) search.length_certificate_suffix_skips();
    // Group skips and suffix skips are disjoint, so both belong in the
    // denominator; otherwise the percentages are not comparable between modes.
    unsigned long long const total = skipped + suffix + kept;
    fprintf(stderr,
            "# phase 2 length certificate: %s, %zu table bytes, "
            "%.6f s prepare\n"
            "# phase 2 length certificate: %llu group tests, "
            "%llu rejected (%.2f%%), %llu class scans skipped of %llu "
            "(%.2f%%)\n",
            search.length_certificate_skipping() ? "skipping" : "shadow",
            search.length_certificate_table_bytes(),
            search.length_certificate_prepare_seconds(),
            tests, rejects,
            tests == 0 ? 0.0 : 100.0 * double(rejects) / double(tests),
            skipped, total,
            total == 0 ? 0.0 : 100.0 * double(skipped) / double(total));
    if (search.length_certificate_suffix_enabled())
      fprintf(stderr,
              "# phase 2 length certificate: %llu further class scans "
              "skipped by score-descending suffix rejection; %llu scanned "
              "(%.2f%% of all candidates removed)\n",
              suffix, kept,
              total == 0
                  ? 0.0
                  : 100.0 * double(skipped + suffix) / double(total));
  }
  if (search.projected_query_diagnostics_enabled()) {
    DfsAnagramSearch::ProjectedDiagnostics const& diagnostics =
        search.projected_diagnostics();
    fprintf(stderr,
            "# phase 2 projected final queries: %llu before score floor, "
            "%llu bound lookups, %llu unique keys, %llu pruned\n",
            (unsigned long long)
                diagnostics.final_queries_without_floor,
            (unsigned long long) diagnostics.final_bound_queries,
            (unsigned long long)
                diagnostics.final_unique_bound_keys,
            (unsigned long long) diagnostics.final_bound_prunes);
    fprintf(stderr,
            "# phase 2 projected certificate fallback: %llu lookups, "
            "%llu unique keys, %llu pruned\n",
            (unsigned long long)
                diagnostics.certificate_fallback_queries,
            (unsigned long long)
                diagnostics.certificate_fallback_unique_keys,
            (unsigned long long)
                diagnostics.certificate_fallback_prunes);
    fprintf(stderr,
            "# phase 2 projected length-only fallback: %llu pruned, "
            "%llu rich-only pruned, %llu length-only pruned\n",
            (unsigned long long)
                diagnostics.final_length_bound_prunes,
            (unsigned long long)
                diagnostics.final_rich_only_vs_length_prunes,
            (unsigned long long)
                diagnostics.final_length_only_prunes);
    fprintf(stderr,
            "# phase 2 projected modular fallback: %zu-bit signatures, "
            "%zu tables, seed %u, %zu table bytes, %zu delta bytes, "
            "prepared in %.6fs\n",
            search.projected_modular_bound_bits(),
            search.projected_modular_bound_count(),
            unsigned(search.projected_modular_bound_seed()),
            search.projected_modular_bound_table_bytes(),
            search.projected_modular_bound_delta_bytes(),
            search.projected_modular_bound_prepare_seconds());
    for (size_t i = 0;
         i < search.projected_modular_bound_count(); ++i) {
      fprintf(stderr,
              "# phase 2 projected modular prefix %zu: %llu pruned, "
              "%llu rich-only pruned, %llu modular-only pruned, "
              "%zu actions, %llu candidate scans\n",
              i + 1,
              (unsigned long long)
                  diagnostics.final_modular_prefix_bound_prunes[i],
              (unsigned long long)
                  diagnostics.final_modular_prefix_rich_only_prunes[i],
              (unsigned long long)
                  diagnostics.final_modular_prefix_only_prunes[i],
              search.projected_modular_bound_actions(i),
              (unsigned long long)
                  search.projected_modular_bound_candidate_scans(i));
    }
    fprintf(stderr,
            "# phase 2 projected modular fallback total: %llu pruned, "
            "%llu rich-only pruned, %llu modular-only pruned\n",
            (unsigned long long)
                diagnostics.final_modular_bound_prunes,
            (unsigned long long)
                diagnostics.final_modular_rich_only_prunes,
            (unsigned long long)
                diagnostics.final_modular_only_prunes);
  }
  fprintf(stderr,
          "# phase 2 score cache: %zu bound entries, %zu bound bytes\n",
          search.score_bound_entries(),
          search.score_bound_bytes_charged());
  fprintf(stderr,
          "# phase 2 complete: %lld nodes, %lld solutions, "
          "%zu spellings expanded, %zu retained\n",
          (long long) search.nodes_visited(),
          (long long) search.solutions_found(),
          output.spellings_expanded(), output.size());
  fflush(stderr);

  std::vector<DfsSpelling> const results = output.take_sorted_results();
  for (size_t i = 0; i < results.size(); ++i)
    printf("%#.4g %s\n", exp(results[i].log_score),
           results[i].text.c_str());
  return 0;
}
