#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

typedef std::unordered_map<std::string, uint64_t> SegmentCounts;

static void usage(FILE* fp, char const* program) {
  fprintf(fp,
      "usage: %s [--pairs] [FILE ...]\n"
      "  count comma-delimited segments in dfs-anagrams output and print\n"
      "  \"count segment\" rows in descending count order\n"
      "  --pairs prints only multi-word segments as comma-separated words,\n"
      "          without counts\n"
      "  with no FILE, or when FILE is -, read standard input\n",
      program);
}

static bool count_stream(
    std::istream* input, char const* name, SegmentCounts* counts) {
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
          "top-segments: %s:%" PRIu64
          ": expected \"score segment[,segment ...]\"\n",
          name, line_number);
      return false;
    }

    size_t start = size_t(score_end - line.c_str()) + 1;
    while (true) {
      size_t const end = line.find(',', start);
      size_t const length =
          end == std::string::npos ? line.size() - start : end - start;
      if (length == 0) {
        fprintf(stderr,
            "top-segments: %s:%" PRIu64 ": empty segment\n",
            name, line_number);
        return false;
      }

      std::string const segment = line.substr(start, length);
      uint64_t& count = (*counts)[segment];
      if (count == std::numeric_limits<uint64_t>::max()) {
        fprintf(stderr, "top-segments: segment count overflow\n");
        return false;
      }
      ++count;

      if (end == std::string::npos) break;
      start = end + 1;
    }
  }

  if (input->bad()) {
    fprintf(stderr, "top-segments: can't read \"%s\"\n", name);
    return false;
  }
  return true;
}

static bool print_counts(SegmentCounts const& counts, bool pairs) {
  std::vector<SegmentCounts::const_iterator> ordered;
  ordered.reserve(counts.size());
  uint64_t largest = 0;
  for (SegmentCounts::const_iterator entry = counts.begin();
       entry != counts.end(); ++entry) {
    if (pairs && entry->first.find(' ') == std::string::npos) continue;
    ordered.push_back(entry);
    largest = std::max(largest, entry->second);
  }
  std::sort(ordered.begin(), ordered.end(),
      [](SegmentCounts::const_iterator a, SegmentCounts::const_iterator b) {
        if (a->second != b->second) return a->second > b->second;
        return a->first < b->first;
      });

  int const width = snprintf(NULL, 0, "%" PRIu64, largest);
  for (size_t i = 0; i < ordered.size(); ++i) {
    if (pairs) {
      std::string words = ordered[i]->first;
      std::replace(words.begin(), words.end(), ' ', ',');
      printf("%s\n", words.c_str());
    } else {
      printf("%*" PRIu64 " %s\n", width, ordered[i]->second,
          ordered[i]->first.c_str());
    }
  }
  return !ferror(stdout);
}

int main(int argc, char* argv[]) {
  std::vector<char const*> paths;
  bool parse_options = true;
  bool pairs = false;
  for (int i = 1; i < argc; ++i) {
    if (parse_options && strcmp(argv[i], "--") == 0) {
      parse_options = false;
    } else if (parse_options &&
               (strcmp(argv[i], "-h") == 0 ||
                strcmp(argv[i], "--help") == 0)) {
      usage(stdout, argv[0]);
      return 0;
    } else if (parse_options && strcmp(argv[i], "--pairs") == 0) {
      pairs = true;
    } else if (parse_options && argv[i][0] == '-' && argv[i][1] != '\0') {
      fprintf(stderr, "top-segments: unknown option \"%s\"\n", argv[i]);
      usage(stderr, argv[0]);
      return 2;
    } else {
      paths.push_back(argv[i]);
    }
  }

  SegmentCounts counts;
  if (paths.empty()) {
    if (!count_stream(&std::cin, "-", &counts)) return 1;
  } else {
    for (size_t i = 0; i < paths.size(); ++i) {
      if (strcmp(paths[i], "-") == 0) {
        if (!count_stream(&std::cin, "-", &counts)) return 1;
        continue;
      }

      errno = 0;
      std::ifstream input(paths[i]);
      if (!input.is_open()) {
        fprintf(stderr, "top-segments: can't open \"%s\": %s\n",
            paths[i], strerror(errno));
        return 1;
      }
      if (!count_stream(&input, paths[i], &counts)) return 1;
    }
  }

  return print_counts(counts, pairs) ? 0 : 1;
}
