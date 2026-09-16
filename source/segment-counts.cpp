#include "segment-counts.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <limits>
#include <vector>

static bool any_segment_in(
    std::vector<std::string> const& segments, DfsPairSet const& pairs) {
  for (std::string const& segment : segments)
    if (pairs.find(segment) != pairs.end()) return true;
  return false;
}

static bool any_rejected_segment(
    std::vector<std::string> const& segments, DfsPairSet const& rejected) {
  for (std::string const& segment : segments) {
    if (is_rejected_segment(rejected, segment)) return true;
  }
  return false;
}

static bool any_segment_outside(
    std::vector<std::string> const& segments,
    DfsDictionary const& dictionary) {
  for (std::string const& segment : segments)
    if (!all_words_in_dict(dictionary, segment)) return true;
  return false;
}

static bool any_segment_disallowed(
    std::vector<std::string> const& segments,
    std::optional<DfsPairSet> const& allowed) {
  for (std::string const& segment : segments)
    if (!is_allowed_segment(allowed, segment)) return true;
  return false;
}

static std::string canonical_pair(std::string const& segment) {
  size_t const space = segment.find(' ');
  if (space == std::string::npos) return segment;
  std::string const left = segment.substr(0, space);
  std::string const right = segment.substr(space + 1);
  return left < right ? segment : right + " " + left;
}

static bool selected_segment(
    SegmentSelection selection, std::string const& segment) {
  if (selection == SEGMENT_SELECTION_PAIRS) return is_pair_segment(segment);
  if (selection == SEGMENT_SELECTION_SOLO) return is_solo_segment(segment);
  return true;
}

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
  std::string line;
  uint64_t line_number = 0;
  while (std::getline(*input, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;

    char* score_end;
    (void) strtod(line.c_str(), &score_end);
    if (score_end == line.c_str() || *score_end != ' ' ||
        score_end[1] == '\0') {
      fprintf(stderr,
          "%s: %s:%" PRIu64
          ": expected \"score segment[,segment ...]\"\n",
          data->program, name, line_number);
      return false;
    }

    std::vector<std::string> segments;
    size_t start = size_t(score_end - line.c_str()) + 1;
    while (true) {
      size_t const end = line.find(',', start);
      size_t const length =
          end == std::string::npos ? line.size() - start : end - start;
      if (length == 0) {
        fprintf(stderr,
            "%s: %s:%" PRIu64 ": empty segment\n",
            data->program, name, line_number);
        return false;
      }
      segments.push_back(line.substr(start, length));

      if (end == std::string::npos) break;
      start = end + 1;
    }

    // Virtual filtering pipeline. The first matching rejection owns the row,
    // so each rejected line contributes to exactly one summary counter.
    // Workflow-wide NO is authoritative over target-local NO; explicit
    // rejects, the explicit allowlist, and then the dictionary follow. Only
    // surviving rows reach the per-segment ignore pipeline, where classified
    // YES owns an overlap with an explicit ignore. Keeping this order visible
    // is important because it defines diagnostic attribution even though set
    // union would produce the same selected output.
    if (any_segment_in(segments, filters.sources.classified_no)) {
      ++data->filtered.classified_no_lines;
      continue;
    }
    if (any_segment_in(segments, filters.sources.target_no)) {
      ++data->filtered.target_no_lines;
      continue;
    }
    if (any_rejected_segment(segments, filters.rejected)) {
      ++data->filtered.explicit_reject_lines;
      continue;
    }
    if (any_segment_disallowed(segments, filters.allowed)) {
      ++data->filtered.allow_lines;
      continue;
    }
    if (any_segment_outside(segments, filters.dictionary)) {
      ++data->filtered.dictionary_lines;
      continue;
    }
    uint64_t row = 0;
    if (options.elimination) {
      if (data->surviving_rows == std::numeric_limits<uint64_t>::max()) {
        fprintf(stderr, "%s: result line count overflow\n", data->program);
        return false;
      }
      row = ++data->surviving_rows;
    }
    for (std::string const& segment : segments) {
      if (filters.sources.classified_yes.find(segment) !=
          filters.sources.classified_yes.end()) {
        ++data->filtered.classified_yes_instances;
        data->filtered.classified_yes_pairs.insert(canonical_pair(segment));
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
      if (!selected_segment(options.output.selection, segment)) continue;

      if (options.output.projection == SEGMENT_PROJECTION_SEGMENTS) {
        if (!count_candidate(segment, row, true, data->program,
                &data->segments))
          return false;
        continue;
      }

      bool const count_occurrence =
          options.output.weight == SEGMENT_WEIGHT_OCCURRENCES ||
          data->unique_segments.insert(segment).second;
      std::vector<std::string> const words = split_segment_words(segment);
      for (std::string const& word : words)
        if (!count_candidate(word, row, count_occurrence, data->program,
                &data->segments))
          return false;
    }
  }

  if (input->bad()) {
    fprintf(stderr, "%s: can't read \"%s\"\n", data->program, name);
    return false;
  }
  return true;
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
    if (!selected_segment(selection, entry->first)) continue;
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

  std::vector<SegmentStatsMap::const_iterator> ordered;
  ordered.reserve(rows.size());
  uint64_t largest = 0;
  uint64_t largest_decision_elim = 0;
  uint64_t largest_require_elim = 0;
  uint64_t largest_reject_elim = 0;
  for (SegmentStatsMap::const_iterator entry = rows.begin();
       entry != rows.end(); ++entry) {
    if (output_options.projection == SEGMENT_PROJECTION_SEGMENTS &&
        !selected_segment(output_options.selection, entry->first))
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
