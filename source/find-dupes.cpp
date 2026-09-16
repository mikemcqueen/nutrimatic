#include <stdio.h>
#include <string.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "dfs-cli-args.h"
#include "segment-output.h"

// The first spelling seen of one canonical pair, and whether a later line has
// already been reported for it, so a pair repeated many times prints once.
struct FirstOccurrence {
  std::string spelling;
  bool reported = false;
};

static void usage(char const* program) {
  fprintf(stdout,
      "usage: %s PAIRS\n"
      "  print each pair of PAIRS that also appears in the opposite word\n"
      "  order, once per pair, spelled as its second occurrence wrote it\n"
      "  PAIRS  word,word lines, as in best.pairs\n",
      program);
}

int main(int argc, char* argv[]) {
  char const* pairs_path = NULL;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      usage(argv[0]);
      return 0;
    }
    if (argv[i][0] == '-' && argv[i][1] != '\0') {
      fprintf(stderr, "find-dupes: unknown option \"%s\"\n", argv[i]);
      usage(argv[0]);
      return 2;
    }
    if (pairs_path != NULL) {
      fprintf(stderr, "find-dupes: too many arguments\n");
      usage(argv[0]);
      return 2;
    }
    pairs_path = argv[i];
  }

  if (pairs_path == NULL) {
    fprintf(stderr, "find-dupes: no PAIRS file\n");
    usage(argv[0]);
    return 2;
  }

  DfsPairSet pairs;
  std::vector<DfsPairRow> rows;
  if (!load_pair_file(pairs_path, "pair list", &pairs, /*quiet=*/true,
                      /*reject_hyphens=*/true, /*allow_single_words=*/true,
                      /*diagnostic_source=*/NULL, &rows))
    return 1;

  std::unordered_map<std::string, FirstOccurrence> seen;
  for (size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].right.empty()) continue;

    std::string const spelling = rows[i].entry();
    auto const inserted = seen.emplace(
        canonical_pair_segment(spelling), FirstOccurrence{spelling, false});
    FirstOccurrence& first = inserted.first->second;
    if (inserted.second || first.reported || first.spelling == spelling)
      continue;

    first.reported = true;
    printf("%s\n", format_pair_segment(spelling).c_str());
    if (ferror(stdout)) return 1;
  }

  return fflush(stdout) == 0 ? 0 : 1;
}
