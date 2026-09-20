#include <stdio.h>
#include <string.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "dfs-cli-args.h"
#include "workflow-paths.h"

namespace {

void usage(FILE* out) {
  fputs("usage: pfilter PAIRS-FILE|-\n"
        "  print pairs whose two words are in $WFROOT/.wf/dict/words.filtered\n"
        "  PAIRS-FILE contains word,word lines; '-' reads standard input\n",
        out);
}

bool filter_pairs(std::istream& input, char const* source,
                  DfsDictionary const& dictionary) {
  std::string line;
  size_t number = 0;
  while (std::getline(input, line)) {
    ++number;
    DfsPairRow row;
    if (!parse_pair_row(
            line, "pair list", source, number, false, &row))
      return false;
    if (dictionary.contains(row.left) && dictionary.contains(row.right)) {
      std::cout << line << '\n';
      if (!std::cout) {
        fprintf(stderr, "pfilter: can't write output\n");
        return false;
      }
    }
  }
  if (!input.eof()) {
    fprintf(stderr, "pfilter: can't read pairs \"%s\"\n", source);
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc == 2 &&
      (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
    usage(stdout);
    return 0;
  }
  if (argc != 2 || (argv[1][0] == '-' && strcmp(argv[1], "-") != 0)) {
    usage(stderr);
    return 2;
  }

  char const* const root = workflow_root_from_env();
  if (root == NULL) {
    fputs("pfilter: WFROOT must be set and nonempty\n", stderr);
    return 1;
  }
  std::filesystem::path const dict_path =
      std::filesystem::path(root) / WORKFLOW_DICT_PATH;
  DfsDictionary dictionary;
  if (!load_dictionary(dict_path.c_str(), &dictionary)) return 1;

  char const* const source = argv[1];
  if (strcmp(source, "-") == 0) {
    if (!filter_pairs(std::cin, source, dictionary)) return 1;
  } else {
    std::ifstream input(source, std::ios::binary);
    if (!input.is_open()) {
      fprintf(stderr, "pfilter: can't open pairs \"%s\"\n", source);
      return 1;
    }
    if (!filter_pairs(input, source, dictionary)) return 1;
  }
  if (!(std::cout << std::flush)) {
    fputs("pfilter: can't write output\n", stderr);
    return 1;
  }
  return 0;
}
