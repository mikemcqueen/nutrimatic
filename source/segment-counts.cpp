#include "segment-counts.h"
#include "dfs-cli-args.h"
#include "segment-rows.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <limits>
#include <vector>

static bool count_candidate(
    std::string const& candidate, uint64_t row, bool count_occurrence,
    char const* program, SegmentStatsMap* counts) {
  SegmentStatsMap::iterator entry = counts->find(candidate);
  if (entry == counts->end()) {
    SegmentStats stats;
    stats.length = segment_nonspace_length(candidate);
    entry = counts->emplace(candidate, stats).first;
  }
  if (count_occurrence) {
    if (entry->second.count == std::numeric_limits<uint64_t>::max()) {
      fprintf(stderr, "%s: segment count overflow\n", program);
      return false;
    }
    ++entry->second.count;
  }
  if (entry->second.last_seen_row != row) {
    if (entry->second.line_count == std::numeric_limits<uint64_t>::max()) {
      fprintf(stderr, "%s: segment line count overflow\n", program);
      return false;
    }
    ++entry->second.line_count;
    entry->second.last_seen_row = row;
  }
  return true;
}

bool segment_counts_read(
    std::istream* input, char const* name, PairFilters const& filters,
    SegmentCountsOptions const& options, SegmentCountsData* data) {
  SegmentRowReader reader = {input, name, data->program};
  SegmentRow row;
  while (segment_rows_next(&reader, &row)) {
    if (options.used_letters && !data->remaining_letters) {
      std::string bag;
      for (std::string const& segment : row.segments)
        for (char ch : segment)
          if (ch != ' ') bag.push_back(ch);
      std::string remaining;
      if (!subtract_letters(bag, *options.used_letters, &remaining))
        return false;
      data->remaining_letters = std::move(remaining);
    }
    // Each segment uses the shared filter precedence. A row is attributed to
    // its earliest layer across every segment, independent of segment order.
    PairFilterLayer rejecting_layer = PAIR_FILTER_NONE;
    for (std::string const& segment : row.segments) {
      PairFilterLayer const layer = filters.first_rejecting_layer(segment);
      if (layer != PAIR_FILTER_NONE &&
          (rejecting_layer == PAIR_FILTER_NONE || layer < rejecting_layer))
        rejecting_layer = layer;
      if (rejecting_layer == PAIR_FILTER_CLASSIFIED_NO) break;
    }
    if (rejecting_layer != PAIR_FILTER_NONE) {
      switch (rejecting_layer) {
        case PAIR_FILTER_CLASSIFIED_NO:
          ++data->filtered.classified_no_lines;
          break;
        case PAIR_FILTER_TARGET_NO:
          ++data->filtered.target_no_lines;
          break;
        case PAIR_FILTER_EXPLICIT_REJECT:
          ++data->filtered.explicit_reject_lines;
          break;
        case PAIR_FILTER_ALLOWLIST:
          ++data->filtered.allow_lines;
          break;
        case PAIR_FILTER_DICTIONARY:
          ++data->filtered.dictionary_lines;
          break;
        case PAIR_FILTER_NONE:
          break;
      }
      continue;
    }
    uint64_t row_number = 0;
    if (options.elimination) {
      if (data->surviving_rows == std::numeric_limits<uint64_t>::max()) {
        fprintf(stderr, "%s: result line count overflow\n", data->program);
        return false;
      }
      row_number = ++data->surviving_rows;
    }
    for (std::string const& segment : row.segments) {
      if (filters.sources.classified_yes.find(segment) !=
          filters.sources.classified_yes.end()) {
        ++data->filtered.classified_yes_instances;
        data->filtered.classified_yes_pairs.insert(
            canonical_pair_segment(segment));
        continue;
      }
      if (filters.ignored.find(segment) != filters.ignored.end()) {
        ++data->filtered.explicit_ignore_instances;
        continue;
      }
      if (!options.elimination) {
        SegmentStatsMap::iterator entry = data->segments.find(segment);
        if (entry == data->segments.end()) {
          SegmentStats stats;
          stats.length = segment_nonspace_length(segment);
          entry = data->segments.emplace(segment, stats).first;
        }
        if (entry->second.count == std::numeric_limits<uint64_t>::max()) {
          fprintf(stderr, "%s: segment count overflow\n", data->program);
          return false;
        }
        ++entry->second.count;
        continue;
      }
      if (!is_selected_segment(options.output.selection, segment)) continue;

      if (options.output.projection == SEGMENT_PROJECTION_SEGMENTS) {
        if (!count_candidate(segment, row_number, true, data->program,
                &data->segments))
          return false;
        continue;
      }

      bool const count_occurrence =
          options.output.weight == SEGMENT_WEIGHT_OCCURRENCES ||
          data->unique_segments.insert(segment).second;
      std::vector<std::string> const words = split_segment_words(segment);
      for (std::string const& word : words)
        if (!count_candidate(word, row_number, count_occurrence, data->program,
                &data->segments))
          return false;
    }
  }

  return !reader.failed;
}

void segment_counts_print_filter_summary(
    SegmentCountsData const& data, PairFilterSources const& sources) {
  PairFilterStats const& stats = data.filtered;
  if (stats.classified_no_lines != 0 || stats.target_no_lines != 0 ||
      stats.explicit_reject_lines != 0 || stats.allow_lines != 0 ||
      stats.dictionary_lines != 0) {
    fprintf(stderr, "%s: Filtered ", data.program);
    bool first = true;
    if (stats.classified_no_lines != 0) {
      fprintf(stderr, "%" PRIu64 " lines from classified/no/no.pairs",
          stats.classified_no_lines);
      first = false;
    }
    if (stats.target_no_lines != 0) {
      if (!first) fputs(", ", stderr);
      fprintf(stderr, "%" PRIu64 " lines from %s/no.pairs",
          stats.target_no_lines, sources.target.c_str());
      first = false;
    }
    if (stats.explicit_reject_lines != 0) {
      if (!first) fputs(", ", stderr);
      fprintf(stderr, "%" PRIu64 " rejected explicitly",
          stats.explicit_reject_lines);
      first = false;
    }
    if (stats.allow_lines != 0) {
      if (!first) fputs(", ", stderr);
      fprintf(stderr, "%" PRIu64 " outside --allow-pairs",
          stats.allow_lines);
      first = false;
    }
    if (stats.dictionary_lines != 0) {
      if (!first) fputs(", ", stderr);
      fprintf(stderr, "%" PRIu64 " rejected by dictionary",
          stats.dictionary_lines);
    }
    fputc('\n', stderr);
  }

  if (stats.classified_yes_instances != 0 ||
      stats.explicit_ignore_instances != 0) {
    fprintf(stderr, "%s: Ignored ", data.program);
    bool first = true;
    if (stats.classified_yes_instances != 0) {
      fprintf(stderr,
          "%" PRIu64 " instances of %zu pairs from classified/yes/yes.pairs",
          stats.classified_yes_instances, stats.classified_yes_pairs.size());
      first = false;
    }
    if (stats.explicit_ignore_instances != 0) {
      if (!first) fputs(", ", stderr);
      fprintf(stderr, "%" PRIu64 " ignored explicitly",
          stats.explicit_ignore_instances);
    }
    fputc('\n', stderr);
  }
}

static bool split_counts(
    SegmentStatsMap const& counts, SegmentSelection selection,
    SegmentWeight weight, char const* program, SegmentStatsMap* words) {
  for (SegmentStatsMap::const_iterator entry = counts.begin();
       entry != counts.end(); ++entry) {
    if (!is_selected_segment(selection, entry->first)) continue;
    std::vector<std::string> const split = split_segment_words(entry->first);
    uint64_t const increment = weight == SEGMENT_WEIGHT_UNIQUE
        ? 1 : entry->second.count;
    for (std::string const& value : split) {
      SegmentStatsMap::iterator word = words->find(value);
      if (word == words->end()) {
        SegmentStats stats;
        stats.length = value.size();
        word = words->emplace(value, stats).first;
      }
      if (word->second.count >
          std::numeric_limits<uint64_t>::max() - increment) {
        fprintf(stderr, "%s: word count overflow\n", program);
        return false;
      }
      word->second.count += increment;
    }
  }
  return true;
}

static bool fits_letters(
    int const (&remaining)[UCHAR_MAX + 1], std::string const& segment) {
  int need[UCHAR_MAX + 1] = { 0 };
  for (char ch : segment) {
    if (ch == ' ') continue;
    unsigned char const index = (unsigned char) ch;
    if (++need[index] > remaining[index]) return false;
  }
  return true;
}

bool segment_counts_print_top(
    SegmentCountsData const& data, SegmentCountsOptions const& options) {
  SegmentOutputOptions const& output_options = options.output;
  bool const elimination = options.elimination;
  uint64_t const surviving_rows = data.surviving_rows;

  SegmentStatsMap split;
  if (!elimination &&
      output_options.projection == SEGMENT_PROJECTION_WORDS &&
      !split_counts(data.segments, output_options.selection,
          output_options.weight, data.program, &split))
    return false;
  SegmentStatsMap const& rows = !elimination &&
          output_options.projection == SEGMENT_PROJECTION_WORDS
      ? split : data.segments;

  int remaining[UCHAR_MAX + 1] = { 0 };
  if (data.remaining_letters)
    for (char ch : *data.remaining_letters) ++remaining[(unsigned char) ch];

  std::vector<SegmentStatsMap::const_iterator> ordered;
  ordered.reserve(rows.size());
  uint64_t largest = 0;
  uint64_t largest_decision_elim = 0;
  uint64_t largest_require_elim = 0;
  uint64_t largest_reject_elim = 0;
  for (SegmentStatsMap::const_iterator entry = rows.begin();
       entry != rows.end(); ++entry) {
    if (output_options.projection == SEGMENT_PROJECTION_SEGMENTS &&
        !is_selected_segment(output_options.selection, entry->first))
      continue;
    if (data.remaining_letters && !fits_letters(remaining, entry->first))
      continue;
    ordered.push_back(entry);
    largest = std::max(largest, entry->second.count);
    uint64_t const require_elim = surviving_rows - entry->second.line_count;
    largest_decision_elim = std::max(largest_decision_elim,
        std::min(entry->second.line_count, require_elim));
    largest_require_elim = std::max(largest_require_elim, require_elim);
    largest_reject_elim =
        std::max(largest_reject_elim, entry->second.line_count);
  }
  size_t const top = output_options.limit != 0 &&
          output_options.limit < ordered.size()
      ? size_t(output_options.limit) : ordered.size();
  std::partial_sort(ordered.begin(), ordered.begin() + top, ordered.end(),
    [&output_options, elimination, surviving_rows](
        SegmentStatsMap::const_iterator a, SegmentStatsMap::const_iterator b) {
      if (elimination) {
        uint64_t const a_decision = std::min(a->second.line_count,
            surviving_rows - a->second.line_count);
        uint64_t const b_decision = std::min(b->second.line_count,
            surviving_rows - b->second.line_count);
        if (a_decision != b_decision) return a_decision > b_decision;
        if (a->second.line_count != b->second.line_count)
          return a->second.line_count > b->second.line_count;
      }
      if (output_options.by_length && a->second.length != b->second.length)
        return a->second.length > b->second.length;
      if (a->second.count != b->second.count)
        return a->second.count > b->second.count;
      return a->first < b->first;
    });

  int width = snprintf(NULL, 0, "%" PRIu64, largest);
  int decision_width =
      snprintf(NULL, 0, "%" PRIu64, largest_decision_elim);
  int require_width =
      snprintf(NULL, 0, "%" PRIu64, largest_require_elim);
  int reject_width =
      snprintf(NULL, 0, "%" PRIu64, largest_reject_elim);
  if (elimination) {
    width = std::max(width, int(strlen("COUNT")));
    decision_width = std::max(decision_width, int(strlen("DECISION")));
    require_width = std::max(require_width, int(strlen("REQUIRE")));
    reject_width = std::max(reject_width, int(strlen("REJECT")));
    printf("%*s %*s %*s %*s %s\n",
        decision_width, "DECISION", require_width, "REQUIRE",
        reject_width, "REJECT", width, "COUNT", "SEGMENT");
  }
  for (size_t i = 0; i < top; ++i) {
    std::string const displayed =
        output_options.selection == SEGMENT_SELECTION_PAIRS &&
            output_options.projection == SEGMENT_PROJECTION_SEGMENTS
        ? format_pair_segment(ordered[i]->first) : ordered[i]->first;
    if (elimination) {
      uint64_t const reject_elim = ordered[i]->second.line_count;
      uint64_t const require_elim = surviving_rows - reject_elim;
      uint64_t const decision_elim = std::min(reject_elim, require_elim);
      printf("%*" PRIu64 " %*" PRIu64 " %*" PRIu64 " %*" PRIu64
             " %s\n",
          decision_width, decision_elim, require_width, require_elim,
          reject_width, reject_elim, width, ordered[i]->second.count,
          displayed.c_str());
      continue;
    }
    if (options.show_counts) {
      printf("%*" PRIu64 " %s\n",
          width, ordered[i]->second.count, displayed.c_str());
    } else {
      printf("%s\n", displayed.c_str());
    }
  }
  return !ferror(stdout);
}
