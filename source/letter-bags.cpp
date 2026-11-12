// letter-bags.cpp - Count the distinct letter bags in word and pair lists.

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "letter-bag.h"
#include "log.h"
#include "optparse.h"
#include "row-input.h"

constexpr char PROGRAM[] = "letter-bags";

namespace {

enum class Mode { count, match, near };

struct Args {
  char** files = NULL;
  Mode mode = Mode::count;
  std::string bag;
  int by = 1;
};

struct Bag {
  uint64_t lines = 0;
  uint64_t first = 0;
  std::string example;
};

void usage(FILE* out) {
  fputs("usage: letter-bags [--match BAG | --near BAG [--by N]] FILE|-...\n",
        out);
  if (out != stdout) return;

  fputs("  count the lines of each FILE and the distinct letter bags they\n"
        "  form, all of a line's letters making one bag; '-' reads standard\n"
        "  input\n"
        "\n"
        "  a FILE's first line decides its format: word, word,word,\n"
        "  COUNT word or COUNT word word; every other line must match it\n"
        "\n"
        "  prints the line and bag counts, then the first line of the\n"
        "  longest bag, from its first letter, with the bag's letter and\n"
        "  line counts, and of the largest, with its line count\n"
        "\noptions:\n", stdout);
  dfs_help_option("--match BAG",
      "print instead each line, from its first letter, whose letters are "
      "those of BAG; BAG's letters are lowercased and all else dropped");
  dfs_help_option("--near BAG",
      "print instead each bag that is BAG with at most N letters added, "
      "removed or replaced, a replaced letter counting once, as its first "
      "line from its first letter; nearest first, then in input order; BAG "
      "as for --match");
  dfs_help_option("--by N", "with --near, the letters that may differ; "
      "defaults to 1");
  dfs_help_option("-h, --help", "show this help");
}

bool parse_args(char* argv[], Args* out, bool* help) {
  int const OPT_MATCH = 256;
  int const OPT_NEAR = 257;
  int const OPT_BY = 258;
  static struct optparse_long const long_options[] = {
    { "match", OPT_MATCH, OPTPARSE_REQUIRED },
    { "near", OPT_NEAR, OPTPARSE_REQUIRED },
    { "by", OPT_BY, OPTPARSE_REQUIRED },
    { "help", 'h', OPTPARSE_NONE },
    { NULL, 0, OPTPARSE_NONE },
  };

  struct optparse options;
  optparse_init(&options, argv);

  bool by_given = false;
  int option;
  while ((option = optparse_long(&options, long_options, NULL)) != -1) {
    switch (option) {
      case OPT_MATCH:
      case OPT_NEAR: {
        char const* const name = option == OPT_MATCH ? "--match" : "--near";
        if (out->mode != Mode::count) {
          fprintf(stderr, "error: --match and --near are mutually exclusive"
                  " and given once\n");
          return false;
        }
        out->mode = option == OPT_MATCH ? Mode::match : Mode::near;
        out->bag = az_letters(options.optarg);
        std::sort(out->bag.begin(), out->bag.end());
        if (out->bag.empty()) {
          fprintf(stderr, "error: %s BAG has no letters\n", name);
          return false;
        }
        break;
      }
      case OPT_BY:
        if (!parse_count(options.optarg, "--by", &out->by)) return false;
        if (out->by == 0) {
          fprintf(stderr, "error: --by must be at least 1\n");
          return false;
        }
        by_given = true;
        break;
      case 'h':
        *help = true;
        return true;
      default:
        fprintf(stderr, "error: %s\n", options.errmsg);
        return false;
    }
  }

  if (by_given && out->mode != Mode::near) {
    fprintf(stderr, "error: --by requires --near\n");
    return false;
  }
  out->files = argv + options.optind;
  return *out->files != NULL;
}

std::string_view row_text(std::string const& line) {
  size_t const start = std::min(
      line.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"),
      line.size());
  return std::string_view(line).substr(start);
}

void bag_key(DfsPairRow const& row, std::string* key) {
  *key = row.left;
  *key += row.right;
  std::sort(key->begin(), key->end());
}

int bag_distance(std::string_view a, std::string_view b) {
  int only_a = 0;
  int only_b = 0;
  size_t i = 0;
  size_t j = 0;
  while (i < a.size() || j < b.size()) {
    if (j == b.size() || (i < a.size() && a[i] < b[j])) {
      ++only_a;
      ++i;
    } else if (i == a.size() || b[j] < a[i]) {
      ++only_b;
      ++j;
    } else {
      ++i;
      ++j;
    }
  }
  return std::max(only_a, only_b);
}

using BagMap = std::unordered_map<std::string, Bag>;

bool read_bags(Args const& args, BagMap* bags, uint64_t* lines) {
  std::string key;
  for (char** file = args.files; *file != NULL; ++file) {
    if (!read_rows(PROGRAM, *file, "input", RowFormat::detect,
            [&](uint64_t, DfsPairRow const& row, std::string const& line) {
              bag_key(row, &key);
              auto const [entry, added] = bags->try_emplace(key);
              if (added) {
                entry->second.first = *lines;
                entry->second.example = row_text(line);
              }
              ++entry->second.lines;
              ++*lines;
              return true;
            }))
      return false;
  }
  return true;
}

bool print_matches(Args const& args) {
  std::string key;
  bool found = false;
  for (char** file = args.files; *file != NULL; ++file) {
    if (!read_rows(PROGRAM, *file, "input", RowFormat::detect,
            [&](uint64_t, DfsPairRow const& row, std::string const& line) {
              bag_key(row, &key);
              if (key == args.bag) {
                found = true;
                std::string_view const text = row_text(line);
                printf("%.*s\n", SV_ARG(text));
              }
              return true;
            }))
      return false;
  }
  if (!found) printf("bag not found\n");
  return true;
}

bool print_near(Args const& args) {
  BagMap bags;
  uint64_t lines = 0;
  if (!read_bags(args, &bags, &lines)) return false;
  if (bags.find(args.bag) == bags.end()) printf("bag not found\n");

  struct Near {
    int distance;
    BagMap::const_iterator bag;
  };
  std::vector<Near> near;
  for (auto it = bags.cbegin(); it != bags.cend(); ++it) {
    size_t const length = it->first.size();
    size_t const gap = length > args.bag.size()
        ? length - args.bag.size() : args.bag.size() - length;
    if (gap > size_t(args.by)) continue;
    int const distance = bag_distance(it->first, args.bag);
    if (distance <= args.by) near.push_back({distance, it});
  }
  std::sort(near.begin(), near.end(), [](Near const& a, Near const& b) {
    return a.distance != b.distance
        ? a.distance < b.distance
        : a.bag->second.first < b.bag->second.first;
  });
  for (Near const& entry : near)
    printf("%s\n", entry.bag->second.example.c_str());
  return true;
}

bool print_counts(Args const& args) {
  BagMap bags;
  uint64_t lines = 0;
  if (!read_bags(args, &bags, &lines)) return false;

  printf("lines: %" PRIu64 "\nbags: %zu\n", lines, bags.size());
  if (bags.empty()) return true;
  auto longest = bags.cbegin();
  auto largest = bags.cbegin();
  for (auto it = bags.cbegin(); it != bags.cend(); ++it) {
    size_t const length = it->first.size();
    size_t const best_length = longest->first.size();
    if (length > best_length ||
        (length == best_length && it->second.first < longest->second.first))
      longest = it;
    if (it->second.lines > largest->second.lines ||
        (it->second.lines == largest->second.lines &&
         it->second.first < largest->second.first))
      largest = it;
  }
  printf("longest (%zu): %s (%" PRIu64 ")\nlargest (%" PRIu64 "): %s\n",
         longest->first.size(), longest->second.example.c_str(),
         longest->second.lines, largest->second.lines,
         largest->second.example.c_str());
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  (void) argc;
  Args args;
  bool help = false;
  if (!parse_args(argv, &args, &help)) {
    usage(stderr);
    return 2;
  }
  if (help) {
    usage(stdout);
    return 0;
  }

  bool ok = false;
  switch (args.mode) {
    case Mode::count:
      ok = print_counts(args);
      break;
    case Mode::match:
      ok = print_matches(args);
      break;
    case Mode::near:
      ok = print_near(args);
      break;
  }
  if (!ok) return 1;
  if (fflush(stdout) != 0 || ferror(stdout)) {
    report(PROGRAM, "can't write output");
    return 1;
  }
  return 0;
}
