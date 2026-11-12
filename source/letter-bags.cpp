// letter-bags.cpp - Count the distinct letter bags in word and pair lists.

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <numeric>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "letter-bag.h"
#include "log.h"
#include "mix-bits.h"
#include "optparse.h"
#include "row-input.h"
#include "run-threads.h"

constexpr char PROGRAM[] = "letter-bags";

namespace {

enum class Mode { count, match, near, outliers };

struct Args {
  char** files = NULL;
  Mode mode = Mode::count;
  std::string bag;
  int by = 1;
  bool all = false;
};

struct Bag {
  uint64_t lines = 0;
  uint64_t first = 0;
  std::string example;
};

struct BagWithRows : Bag {
  std::vector<std::string> rows;
};

void usage(FILE* out) {
  fputs("usage: letter-bags [--match BAG | --near BAG [--by N] |\n"
        "                    --outliers [--by N] [--all]] FILE|-...\n", out);
  if (out != stdout) return;

  fputs("  count the lines of each FILE and the distinct letter bags they\n"
        "  form, all of a line's letters making one bag; '-' reads standard\n"
        "  input\n"
        "\n"
        "  a FILE's first line decides its format: word, word,word,\n"
        "  COUNT word or COUNT word word; every other line must match it\n"
        "\n"
        "  prints the line and bag counts and their difference, then the\n"
        "  first line of the longest bag, from its first letter, with the\n"
        "  bag's letter and line counts, and of the largest, with its line\n"
        "  count\n"
        "\noptions:\n", stdout);
  dfs_help_option("--match BAG",
      "print instead each line, from its first letter, whose letters are "
      "those of BAG; BAG's letters are lowercased and all else dropped");
  dfs_help_option("--near BAG",
      "print instead each bag that is BAG with at most N letters added, "
      "removed or replaced, a replaced letter counting once, as its first "
      "line from its first letter; nearest first, then in input order; BAG "
      "as for --match");
  dfs_help_option("--outliers",
      "print instead the line and outlier counts, then each bag with no "
      "other bag within N letters, as for --near, as its first line from "
      "its first letter and its line count; in input order");
  dfs_help_option("--all", "with --outliers, print only every line of each "
      "outlier bag, from its first letter");
  dfs_help_option("--by N", "with --near or --outliers, the letters that "
      "may differ; defaults to 1");
  dfs_help_option("-h, --help", "show this help");
}

bool parse_args(char* argv[], Args* out, bool* help) {
  int const OPT_MATCH = 256;
  int const OPT_NEAR = 257;
  int const OPT_BY = 258;
  int const OPT_OUTLIERS = 259;
  int const OPT_ALL = 260;
  static struct optparse_long const long_options[] = {
    { "outliers", OPT_OUTLIERS, OPTPARSE_NONE },
    { "all", OPT_ALL, OPTPARSE_NONE },
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
          fprintf(stderr, "error: --match, --near and --outliers are "
                  "mutually exclusive and given once\n");
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
      case OPT_OUTLIERS:
        if (out->mode != Mode::count) {
          fprintf(stderr, "error: --match, --near and --outliers are "
                  "mutually exclusive and given once\n");
          return false;
        }
        out->mode = Mode::outliers;
        break;
      case OPT_ALL:
        out->all = true;
        break;
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

  if (by_given && out->mode != Mode::near && out->mode != Mode::outliers) {
    fprintf(stderr, "error: --by requires --near or --outliers\n");
    return false;
  }
  if (out->all && out->mode != Mode::outliers) {
    fprintf(stderr, "error: --all requires --outliers\n");
    return false;
  }
  out->files = argv + options.optind;
  return *out->files != NULL;
}

std::string_view row_text(std::string const& line) {
  size_t const start = std::min(
      line.find_first_of(
          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"),
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

template <typename B = Bag>
using BagMap = std::unordered_map<std::string, B>;

template <typename B>
bool read_bags(Args const& args, BagMap<B>* bags, uint64_t* lines) {
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
              if constexpr (std::is_same_v<B, BagWithRows>)
                entry->second.rows.emplace_back(row_text(line));
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
  BagMap<> bags;
  uint64_t lines = 0;
  if (!read_bags(args, &bags, &lines)) return false;

  struct Near {
    int distance;
    BagMap<>::const_iterator bag;
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
  if (near.empty()) printf("no nearby bags found\n");
  for (Near const& entry : near)
    printf("%s\n", entry.bag->second.example.c_str());
  return true;
}

uint64_t occurrence_hash(char ch, size_t occurrence) {
  return mix_bits(((uint64_t((unsigned char) ch) << 32) | occurrence) +
                  UINT64_C(0x9e3779b97f4a7c15));
}

struct Run {
  uint32_t start;
  uint32_t count;
};

struct SubBags {
  std::vector<Run> runs;
  std::vector<uint64_t> letters;
  std::vector<uint64_t> hashes;
};

void add_sub_bags(SubBags* sub, size_t at, int left, uint64_t hash) {
  if (at == sub->runs.size() || left == 0) {
    sub->hashes.push_back(hash);
    return;
  }
  Run const run = sub->runs[at];
  uint32_t const most = std::min(run.count, uint32_t(left));
  for (uint32_t removed = 0; removed <= most; ++removed) {
    add_sub_bags(sub, at + 1, left - int(removed), hash);
    if (removed < most)
      hash -= sub->letters[run.start + run.count - 1 - removed];
  }
}

void sub_bag_hashes(std::string_view key, int by, SubBags* sub) {
  sub->runs.clear();
  sub->letters.clear();
  sub->hashes.clear();
  uint64_t hash = 0;
  for (size_t i = 0; i < key.size();) {
    size_t j = i;
    for (; j < key.size() && key[j] == key[i]; ++j) {
      sub->letters.push_back(occurrence_hash(key[i], j - i));
      hash += sub->letters.back();
    }
    sub->runs.push_back({uint32_t(i), uint32_t(j - i)});
    i = j;
  }
  add_sub_bags(sub, 0, by, hash);
}

size_t sub_bag_count(std::string_view key, int by, std::vector<size_t>* ways) {
  ways->assign(size_t(by) + 1, 0);
  (*ways)[0] = 1;
  for (size_t i = 0; i < key.size();) {
    size_t j = i;
    while (j < key.size() && key[j] == key[i]) ++j;
    size_t const count = j - i;
    for (size_t removed = size_t(by); removed > 0; --removed) {
      size_t const most = std::min(count, removed);
      for (size_t r = 1; r <= most; ++r)
        (*ways)[removed] += (*ways)[removed - r];
    }
    i = j;
  }
  return std::accumulate(ways->begin(), ways->end(), size_t(0));
}

constexpr uint32_t NO_BAG = UINT32_MAX;
constexpr size_t MAX_TABLE_ENTRIES = size_t(1) << 22;
constexpr size_t STREAM_BLOCK = 256;

struct Slot {
  uint32_t tag;
  uint32_t id;
};

using Keys = std::vector<std::string_view>;
using NearFlags = std::vector<std::atomic<uint8_t>>;

bool is_near(NearFlags const& near, uint32_t id) {
  return near[id].load(std::memory_order_relaxed) != 0;
}

void set_near(NearFlags* near, uint32_t id) {
  (*near)[id].store(1, std::memory_order_relaxed);
}

void mark_near_bags(Keys const& keys, std::vector<Slot> const& table, int by,
                    size_t threads, NearFlags* near) {
  size_t const mask = table.size() - 1;
  std::atomic<size_t> next{0};
  run_threads(threads, [&](size_t) {
    SubBags sub;
    for (;;) {
      size_t const begin = next.fetch_add(STREAM_BLOCK);
      if (begin >= keys.size()) return;
      size_t const end = std::min(begin + STREAM_BLOCK, keys.size());
      for (size_t id = begin; id < end; ++id) {
        sub_bag_hashes(keys[id], by, &sub);
        for (uint64_t const hash : sub.hashes)
          __builtin_prefetch(&table[hash & mask]);
        for (uint64_t const hash : sub.hashes) {
          uint32_t const tag = uint32_t(hash >> 32);
          for (size_t at = hash & mask; table[at].id != NO_BAG;
               at = (at + 1) & mask) {
            Slot const slot = table[at];
            if (slot.tag != tag || slot.id == id || is_near(*near, slot.id))
              continue;
            if (bag_distance(keys[slot.id], keys[id]) > by) continue;
            set_near(near, slot.id);
            set_near(near, uint32_t(id));
          }
        }
      }
    }
  });
}

template <typename B>
bool print_outliers(Args const& args) {
  using Iterator = typename BagMap<B>::const_iterator;
  BagMap<B> bags;
  uint64_t lines = 0;
  if (!read_bags(args, &bags, &lines)) return false;

  std::vector<Iterator> order;
  order.reserve(bags.size());
  for (auto it = bags.cbegin(); it != bags.cend(); ++it) order.push_back(it);
  std::sort(order.begin(), order.end(),
      [](Iterator a, Iterator b) {
        return a->second.first < b->second.first;
      });
  Keys keys;
  keys.reserve(order.size());
  for (Iterator const it : order) keys.push_back(it->first);

  size_t const threads = std::min(resolve_search_threads(0),
                                  keys.size() / STREAM_BLOCK + 1);
  NearFlags near(keys.size());
  std::vector<uint32_t> open(keys.size());
  std::iota(open.begin(), open.end(), uint32_t(0));
  std::vector<Slot> table;
  SubBags sub;
  std::vector<size_t> ways;
  for (int by = 1; by <= args.by && !open.empty(); ++by) {
    for (size_t begin = 0; begin < open.size();) {
      size_t entries = 0;
      size_t end = begin;
      for (; end < open.size(); ++end) {
        if (is_near(near, open[end])) continue;
        size_t const count = sub_bag_count(keys[open[end]], by, &ways);
        if (entries != 0 && entries + count > MAX_TABLE_ENTRIES) break;
        entries += count;
      }
      if (entries != 0) {
        table.assign(std::bit_ceil(entries + entries / 2 + 1),
                     Slot{0, NO_BAG});
        size_t const mask = table.size() - 1;
        for (size_t i = begin; i < end; ++i) {
          if (is_near(near, open[i])) continue;
          sub_bag_hashes(keys[open[i]], by, &sub);
          for (uint64_t const hash : sub.hashes) {
            size_t at = hash & mask;
            while (table[at].id != NO_BAG) at = (at + 1) & mask;
            table[at] = {uint32_t(hash >> 32), open[i]};
          }
        }
        mark_near_bags(keys, table, by, threads, &near);
      }
      begin = end;
    }
    std::erase_if(open, [&](uint32_t id) { return is_near(near, id); });
  }

  if (!args.all)
    printf("lines: %" PRIu64 " outliers: %zu\n", lines, open.size());
  for (uint32_t const id : open) {
    B const& bag = order[id]->second;
    if constexpr (std::is_same_v<B, BagWithRows>) {
      for (std::string const& row : bag.rows) printf("%s\n", row.c_str());
    } else {
      printf("%s (%" PRIu64 ")\n", bag.example.c_str(), bag.lines);
    }
  }
  return true;
}

bool print_counts(Args const& args) {
  BagMap<> bags;
  uint64_t lines = 0;
  if (!read_bags(args, &bags, &lines)) return false;

  printf("lines: %" PRIu64 " bags: %zu diff: %" PRIu64 "\n", lines,
         bags.size(), lines - uint64_t(bags.size()));
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
    case Mode::outliers:
      ok = args.all ? print_outliers<BagWithRows>(args)
                    : print_outliers<Bag>(args);
      break;
  }
  if (!ok) return 1;
  if (fflush(stdout) != 0 || ferror(stdout)) {
    report(PROGRAM, "can't write output");
    return 1;
  }
  return 0;
}
