#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "dfs-cli-args.h"
#include "pair-exclusions.h"
#include "segment-output.h"
#include "row-input.h"
#include "segment-rows.h"

static constexpr PairFilterSupport kSupport = {
    .ignore = true, .allow = true, .workflow_yes = true};

enum OutputMode {
  OUTPUT_SEGMENTS,
  OUTPUT_ROWS,
  OUTPUT_COMBOS,
  OUTPUT_PAIRS,
};

// One combination of two distinct pairs, canonically spelled and ordered, and
// the number of holding rows that held both.
typedef std::map<std::pair<std::string, std::string>, uint64_t>
    PairCombinationCounts;

// One pair and the number of holding rows that held it, which is the union of
// that pair's combinations rather than the sum of their counts.
typedef std::map<std::string, uint64_t> PairCounts;

// Each pair's spelling as the PAIRS file wrote it, keyed by its canonical
// form, so output repeats that word order whichever way a row spelled it.
typedef std::unordered_map<std::string, std::string> PairSpellings;

// What one pass over the rows produced, in the mode that pass ran in.
struct CommonOutput {
  std::set<std::string> segments;
  PairCombinationCounts combinations;
  PairCounts pair_counts;
  uint64_t holding_rows = 0;
};

struct Args {
  char const* results_path = NULL;
  char const* pairs_path = NULL;
  OutputMode mode = OUTPUT_SEGMENTS;
  PairFilterOptions filter_options;
};

static void usage(char const* program) {
  fprintf(stdout,
      "usage: %s [-i FILE | --ignore FILE]...\n"
      "          [-r FILE | --reject FILE]...\n"
      "          [-a FILE]...\n"
      "          [-d PATH]\n"
      "          [--wf | --wfroot DIR] [-t TARGET] [-y]\n"
      "          [--results | --combos | --pairs]\n"
      "          RESULTS PAIRS\n"
      "  print the segments of the result rows that hold two or more PAIRS,\n"
      "  one per line in ascending order, in pair-file format\n"
      "  RESULTS             dfs-anagrams result rows; - reads standard\n"
      "                      input\n"
      "  PAIRS               word,word lines, as in best.pairs\n"
      "  -i, --ignore FILE   do not print pairs listed in FILE; may be\n"
      "                      repeated\n"
      "  --results           print the result rows holding two or more\n"
      "                      pairs, as read, instead of their segments\n"
      "  --combos            print COUNT PAIR1 PAIR2 by descending count,\n"
      "                      one line per combination of two pairs held by\n"
      "                      the same row\n"
      "  --pairs             print COUNT PAIR by descending count, one line\n"
      "                      per pair, counting the rows that held it with\n"
      "                      any other pair; honors --ignore and --yes\n",
      program);
  print_reject_option_help(stdout, 22);
  print_allow_pairs_option_help(stdout, 22);
  fprintf(stdout,
      "  -d, --dict PATH     discard rows with any word not in PATH; with\n"
      "                      --wf or --wfroot, defaults to DIR/%s\n"
      "  --wfroot DIR        implies -r DIR/%s; also implies\n"
      "                      -r on the selected target's\n"
      "                      DIR/%s\n"
      "  --wf                shortcut for --wfroot $WFROOT\n"
      "  -t, --target TARGET with --wf or --wfroot, the target selected by\n"
      "                      DIR/.wf/best/TARGET; defaults to the target\n"
      "                      RESULTS names of its own, by its directory or\n"
      "                      by its name, and otherwise to %s; an explicit\n"
      "                      TARGET must be that same target: %s\n"
      "  -y, --yes           with --wf or --wfroot, ignore pairs in the\n"
      "                      selected root's %s\n",
      WORKFLOW_DICT_PATH, WORKFLOW_NO_PAIRS_PATH, WORKFLOW_TARGET_NO_PAIRS_PATH,
      WORKFLOW_DEFAULT_TARGET, WORKFLOW_RESULTS_NAME,
      WORKFLOW_YES_PAIRS_PATH);
}

static char const* mode_flag(OutputMode mode) {
  switch (mode) {
    case OUTPUT_ROWS: return "--results";
    case OUTPUT_COMBOS: return "--combos";
    case OUTPUT_PAIRS: return "--pairs";
    case OUTPUT_SEGMENTS: break;
  }
  return "";
}

static bool parse_args(
    int argc, char* argv[], Args* out, bool* requested_help) {
  *requested_help = false;
  std::vector<char const*> paths;
  PairFilterOptions filter_options;
  OutputMode mode = OUTPUT_SEGMENTS;
  bool mode_chosen = false;
  bool parse_options = true;
  for (int i = 1; i < argc; ++i) {
    if (parse_options) {
      PairFilterOptionResult const filter_result = parse_pair_filter_option(
          argc, argv, &i, "common-segments", kSupport, &filter_options);
      if (filter_result == PAIR_FILTER_OPTION_ERROR) {
        usage(argv[0]);
        return false;
      }
      if (filter_result == PAIR_FILTER_OPTION_HANDLED) continue;
    }

    if (parse_options && strcmp(argv[i], "--") == 0) {
      parse_options = false;
    } else if (parse_options &&
               (strcmp(argv[i], "--results") == 0 ||
                strcmp(argv[i], "--combos") == 0 ||
                strcmp(argv[i], "--pairs") == 0)) {
      OutputMode const chosen = strcmp(argv[i], "--results") == 0 ? OUTPUT_ROWS
          : strcmp(argv[i], "--combos") == 0                     ? OUTPUT_COMBOS
                                                                 : OUTPUT_PAIRS;
      if (mode_chosen && mode != chosen) {
        fprintf(stderr, "common-segments: %s and %s are mutually exclusive\n",
            mode_flag(mode), argv[i]);
        usage(argv[0]);
        return false;
      }
      mode = chosen;
      mode_chosen = true;
    } else if (parse_options &&
               (strcmp(argv[i], "-h") == 0 ||
                strcmp(argv[i], "--help") == 0)) {
      *requested_help = true;
      return true;
    } else if (parse_options && argv[i][0] == '-' && argv[i][1] != '\0') {
      fprintf(stderr, "common-segments: unknown option \"%s\"\n", argv[i]);
      usage(argv[0]);
      return false;
    } else {
      paths.push_back(argv[i]);
    }
  }

  if (!check_pair_filter_options(filter_options, "common-segments")) {
    usage(argv[0]);
    return false;
  }
  if (paths.size() != 2) {
    fputs("common-segments: RESULTS and PAIRS are both required\n", stderr);
    usage(argv[0]);
    return false;
  }

  if (strcmp(paths[0], "-") != 0) filter_options.input_path = paths[0];

  out->results_path = paths[0];
  out->pairs_path = paths[1];
  out->mode = mode;
  out->filter_options = std::move(filter_options);
  return true;
}

static bool any_segment_in(
    std::vector<std::string> const& segments, DfsPairSet const& pairs) {
  for (std::string const& segment : segments)
    if (pairs.find(segment) != pairs.end()) return true;
  return false;
}

static bool any_rejected_segment(
    std::vector<std::string> const& segments, DfsPairSet const& rejected) {
  for (std::string const& segment : segments)
    if (is_rejected_segment(rejected, segment)) return true;
  return false;
}

static bool any_segment_disallowed(
    std::vector<std::string> const& segments,
    std::optional<DfsPairSet> const& allowed) {
  for (std::string const& segment : segments)
    if (!is_allowed_segment(allowed, segment)) return true;
  return false;
}

static bool any_segment_outside(
    std::vector<std::string> const& segments,
    DfsDictionary const& dictionary) {
  for (std::string const& segment : segments)
    if (!all_words_in_dict(dictionary, segment)) return true;
  return false;
}

static std::string written_pair_segment(
    PairSpellings const& spellings, std::string const& segment) {
  std::string const canonical = canonical_pair_segment(segment);
  PairSpellings::const_iterator const entry = spellings.find(canonical);
  return entry == spellings.end() ? canonical : entry->second;
}

static bool collect_common(
    std::istream* input, char const* name, PairFilters const& filters,
    DfsPairSet const& pairs, PairSpellings const& spellings, OutputMode mode,
    CommonOutput* out) {
  SegmentRowReader reader = {input, name, "common-segments"};
  SegmentRow row;
  std::set<std::string> held;
  while (segment_rows_next(&reader, &row)) {
    // The row filters in segment-counts' precedence. Nothing is attributed
    // here, so the first match simply drops the row.
    if (any_segment_in(row.segments, filters.sources.classified_no)) continue;
    if (any_segment_in(row.segments, filters.sources.target_no)) continue;
    if (any_rejected_segment(row.segments, filters.rejected)) continue;
    if (any_segment_disallowed(row.segments, filters.allowed)) continue;
    if (any_segment_outside(row.segments, filters.dictionary)) continue;

    held.clear();
    for (std::string const& segment : row.segments)
      if (pairs.find(segment) != pairs.end())
        held.insert(written_pair_segment(spellings, segment));
    if (held.size() < 2) continue;
    ++out->holding_rows;

    if (mode == OUTPUT_ROWS) {
      printf("%s\n", row.line.c_str());
      continue;
    }

    // Every combination of two held pairs has this row in common.
    if (mode == OUTPUT_COMBOS) {
      for (std::set<std::string>::const_iterator first = held.begin();
           first != held.end(); ++first) {
        std::set<std::string>::const_iterator second = first;
        for (++second; second != held.end(); ++second)
          ++out->combinations[std::make_pair(*first, *second)];
      }
      continue;
    }

    // Each held pair has this row in common with every other pair held here,
    // so the row counts once for it however many others there are.
    if (mode == OUTPUT_PAIRS) {
      for (std::string const& pair : held) ++out->pair_counts[pair];
      continue;
    }

    // A row holding exactly two pairs lies in one intersection, and that
    // intersection excludes both of them; a third pair puts every pair in
    // some other pair's intersection.
    bool const hold_back_pairs = held.size() == 2;
    for (std::string const& segment : row.segments) {
      if (hold_back_pairs && pairs.find(segment) != pairs.end()) continue;
      if (filters.sources.classified_yes.find(segment) !=
          filters.sources.classified_yes.end())
        continue;
      if (filters.ignored.find(segment) != filters.ignored.end()) continue;
      out->segments.insert(segment);
    }
  }

  return !reader.failed;
}

static bool print_combination_counts(PairCombinationCounts const& counts) {
  std::vector<PairCombinationCounts::const_iterator> ordered;
  ordered.reserve(counts.size());
  uint64_t largest = 0;
  for (PairCombinationCounts::const_iterator entry = counts.begin();
       entry != counts.end(); ++entry) {
    ordered.push_back(entry);
    largest = std::max(largest, entry->second);
  }
  std::sort(ordered.begin(), ordered.end(),
      [](PairCombinationCounts::const_iterator a,
          PairCombinationCounts::const_iterator b) {
        if (a->second != b->second) return a->second > b->second;
        return a->first < b->first;
      });

  int const count_width = snprintf(NULL, 0, "%" PRIu64, largest);
  int first_width = 0;
  for (PairCombinationCounts::const_iterator entry : ordered)
    first_width = std::max(first_width, int(entry->first.first.size()));

  for (PairCombinationCounts::const_iterator entry : ordered)
    printf("%*" PRIu64 " %-*s %s\n", count_width, entry->second, first_width,
        format_pair_segment(entry->first.first).c_str(),
        format_pair_segment(entry->first.second).c_str());
  return !ferror(stdout);
}

static bool print_pair_counts(
    PairCounts const& counts, PairFilters const& filters) {
  std::vector<PairCounts::const_iterator> ordered;
  ordered.reserve(counts.size());
  uint64_t largest = 0;
  for (PairCounts::const_iterator entry = counts.begin(); entry != counts.end();
       ++entry) {
    // Counted above with every other held pair, suppressed only here, so
    // hiding a classified pair leaves its neighbors' counts intact.
    if (filters.sources.classified_yes.find(entry->first) !=
        filters.sources.classified_yes.end())
      continue;
    if (filters.ignored.find(entry->first) != filters.ignored.end()) continue;
    ordered.push_back(entry);
    largest = std::max(largest, entry->second);
  }
  std::sort(ordered.begin(), ordered.end(),
      [](PairCounts::const_iterator a, PairCounts::const_iterator b) {
        if (a->second != b->second) return a->second > b->second;
        return a->first < b->first;
      });

  int const count_width = snprintf(NULL, 0, "%" PRIu64, largest);
  for (PairCounts::const_iterator entry : ordered)
    printf("%*" PRIu64 " %s\n", count_width, entry->second,
        format_pair_segment(entry->first).c_str());
  return !ferror(stdout);
}

int main(int argc, char* argv[]) {
  Args args;
  bool requested_help;
  if (!parse_args(argc, argv, &args, &requested_help)) return 2;
  if (requested_help) {
    usage(argv[0]);
    return 0;
  }

  PairFilters filters;
  if (!load_pair_filters(
          args.filter_options, "common-segments", kSupport, &filters))
    return 1;

  DfsPairSet pairs;
  std::vector<DfsPairRow> pair_rows;
  if (!load_pair_file(args.pairs_path, "pair list", &pairs, true, true, false,
          NULL, &pair_rows))
    return 1;

  PairSpellings spellings;
  for (DfsPairRow const& row : pair_rows) {
    std::string written = row.left + " " + row.right;
    spellings.emplace(canonical_pair_segment(written), std::move(written));
  }

  CommonOutput output;
  if (!read_input_file("common-segments", args.results_path,
          [&](std::istream& input) {
            return collect_common(&input, args.results_path, filters, pairs,
                spellings, args.mode, &output);
          }))
    return 1;

  if (output.holding_rows == 0) {
    fputs("common-segments: no result row holds two or more pairs\n", stderr);
    return 0;
  }

  if (args.mode == OUTPUT_COMBOS)
    return print_combination_counts(output.combinations) ? 0 : 1;
  if (args.mode == OUTPUT_PAIRS)
    return print_pair_counts(output.pair_counts, filters) ? 0 : 1;

  for (std::string const& segment : output.segments)
    printf("%s\n", format_pair_segment(segment).c_str());
  return ferror(stdout) ? 1 : 0;
}
