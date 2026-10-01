// bad-bags.cpp - List the sub-bags of a letter bag that no combination of
// dictionary words spells exactly.

#include <stdint.h>
#include <stdio.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "dfs-class-list.h"
#include "optparse.h"
#include "workflow-paths.h"

namespace {

int const DEFAULT_MIN_WORD_LENGTH = 3;

// The sub-bag lattice is one bit per sub-bag; past this it does not fit.
size_t const MAX_SUB_BAGS = size_t(1) << 33;

struct Args {
  std::string letters;
  int min_word_length = DEFAULT_MIN_WORD_LENGTH;
  int max_letters = 0;
  char const* dictionary = NULL;
};

// One node of the word trie, whose paths spell words' letters in slot order.
struct TrieNode {
  uint32_t first_edge;
  uint32_t edge_count;
  bool terminal;
};

struct TrieEdge {
  uint32_t slot;
  uint32_t node;
};

void usage(FILE* out) {
  fputs("usage: bad-bags [-m N] [-x N] [--dict PATH] LETTERS\n", out);
  if (out != stdout) return;

  fputs("  for each sub-bag of LETTERS holding N..X letters, decide whether\n"
        "  dictionary words of at least N letters spell it exactly; print\n"
        "  the sorted letters of every sub-bag they do not, shortest first\n"
        "\noptions:\n", stdout);
  dfs_help_option("-m, --min-word-length N",
      "use only words of at least N letters, and only sub-bags of at least "
      "N letters (default: %d)", DEFAULT_MIN_WORD_LENGTH);
  dfs_help_option("-x, --max-letters X",
      "check only sub-bags of at most X letters; X must be at least N "
      "(default: all of LETTERS)");
  dfs_help_option("--dict PATH",
      "read words from PATH (default: $WFROOT/%s)", WORKFLOW_DICT_PATH);
  dfs_help_option("-h, --help", "show this help");
}

bool parse_args(char* argv[], Args* out, bool* help) {
  static struct optparse_long const long_options[] = {
    { "min-word-length", 'm', OPTPARSE_REQUIRED },
    { "max-letters", 'x', OPTPARSE_REQUIRED },
    { "dict", 'd', OPTPARSE_REQUIRED },
    { "help", 'h', OPTPARSE_NONE },
    { NULL, 0, OPTPARSE_NONE },
  };

  struct optparse options;
  optparse_init(&options, argv);

  int option;
  while ((option = optparse_long(&options, long_options, NULL)) != -1) {
    switch (option) {
      case 'm':
        if (!parse_count(
                options.optarg, "--min-word-length", &out->min_word_length))
          return false;
        if (out->min_word_length == 0) {
          fputs("bad-bags: --min-word-length must be positive\n", stderr);
          return false;
        }
        break;
      case 'x':
        if (!parse_count(options.optarg, "--max-letters", &out->max_letters))
          return false;
        if (out->max_letters == 0) {
          fputs("bad-bags: --max-letters must be positive\n", stderr);
          return false;
        }
        break;
      case 'd':
        out->dictionary = options.optarg;
        break;
      case 'h':
        *help = true;
        return true;
      default:
        fprintf(stderr, "error: %s\n", options.errmsg);
        return false;
    }
  }

  char const* const letters = optparse_arg(&options);
  if (letters == NULL || optparse_arg(&options) != NULL) return false;
  if (!clean_letters(letters, "letters", &out->letters)) return false;
  std::sort(out->letters.begin(), out->letters.end());

  if (out->letters.size() < size_t(out->min_word_length)) {
    fputs("bad-bags: LETTERS is shorter than --min-word-length\n", stderr);
    return false;
  }
  if (out->max_letters == 0) out->max_letters = int(out->letters.size());
  if (out->max_letters < out->min_word_length) {
    fputs("bad-bags: --max-letters must be at least --min-word-length\n",
        stderr);
    return false;
  }
  return true;
}

class BadBags {
 public:
  BadBags(std::string const& letters, size_t min_length, size_t max_length)
      : min_length(min_length),
        max_length(std::min(max_length, letters.size())) {
    for (char ch : letters) {
      if (symbols.empty() || symbols.back() != ch) {
        symbols.push_back(ch);
        counts.push_back(0);
      }
      ++counts.back();
    }
    for (int ch = 0; ch <= UCHAR_MAX; ++ch) slot_of[ch] = SLOT_MISSING;
    for (size_t i = 0; i < symbols.size(); ++i)
      slot_of[(unsigned char) symbols[i]] = int(i);

    size_t const width = symbols.size();
    std::vector<long double> below((width + 1) * (max_length + 1), 1);
    for (size_t i = width; i-- > 0;)
      for (size_t r = 0; r <= max_length; ++r) {
        long double sum = 0;
        for (size_t d = 0; d <= std::min(size_t(counts[i]), r); ++d)
          sum += below[(i + 1) * (max_length + 1) + r - d];
        below[i * (max_length + 1) + r] = sum;
      }
    sub_bags = below[max_length];
    if (!fits()) return;

    max_count = size_t(*std::max_element(counts.begin(), counts.end()));
    ranks.assign(width * (max_length + 1) * (max_count + 1), 0);
    for (size_t i = 0; i < width; ++i)
      for (size_t r = 0; r <= max_length; ++r) {
        size_t sum = 0;
        for (size_t d = 0; d <= std::min(size_t(counts[i]), r); ++d) {
          ranks[rank_slot(i, r, d)] = sum;
          sum += size_t(below[(i + 1) * (max_length + 1) + r - d]);
        }
      }
    total = size_t(sub_bags);
  }

  bool fits() const { return sub_bags <= MAX_SUB_BAGS; }
  long double sub_bag_count() const { return sub_bags; }
  size_t checked() const { return decided; }

  void add_words(DfsDictionary const& dictionary) {
    size_t const width = symbols.size();
    std::vector<uint32_t> children(width, 0);
    std::vector<bool> terminals(1, false);
    std::vector<int> need(width);
    for (std::string const& word : dictionary) {
      if (word.size() < min_length || word.size() > max_length) continue;
      std::fill(need.begin(), need.end(), 0);
      bool fit = true;
      for (char ch : word) {
        int const slot = slot_of[(unsigned char) ch];
        if (slot == SLOT_MISSING || ++need[slot] > counts[slot]) {
          fit = false;
          break;
        }
      }
      if (!fit) continue;

      uint32_t node = 0;
      for (size_t slot = 0; slot < width; ++slot)
        for (int i = 0; i < need[slot]; ++i) {
          size_t const edge = node * width + slot;
          if (children[edge] == 0) {
            children[edge] = uint32_t(terminals.size());
            terminals.push_back(false);
            children.resize(children.size() + width, 0);
          }
          node = children[edge];
        }
      terminals[node] = true;
    }

    nodes.clear();
    edges.clear();
    for (size_t node = 0; node < terminals.size(); ++node) {
      TrieNode trie_node = { uint32_t(edges.size()), 0, terminals[node] };
      for (size_t slot = 0; slot < width; ++slot) {
        uint32_t const child = children[node * width + slot];
        if (child == 0) continue;
        edges.push_back({ uint32_t(slot), child });
        ++trie_node.edge_count;
      }
      nodes.push_back(trie_node);
    }
    roots.assign(children.begin(), children.begin() + width);
  }

  std::vector<std::string> run() {
    good.assign(total / 64 + 1, 0);
    good[0] = 1;
    digits.assign(symbols.size(), 0);
    bad.clear();
    visit(0, 0, 0, symbols.size());
    std::sort(bad.begin(), bad.end(),
        [](std::string const& a, std::string const& b) {
          if (a.size() != b.size()) return a.size() < b.size();
          return a < b;
        });
    return std::move(bad);
  }

 private:
  static int const SLOT_MISSING = -1;

  size_t rank_slot(size_t slot, size_t budget, size_t digit) const {
    return (slot * (max_length + 1) + budget) * (max_count + 1) + digit;
  }

  // The index of the sub-bag `digits` holds, whose letters before `first`
  // are all zero.
  size_t index_of(size_t first) const {
    size_t index = 0;
    size_t budget = max_length;
    for (size_t i = first; i < symbols.size(); ++i) {
      index += ranks[rank_slot(i, budget, size_t(digits[i]))];
      budget -= size_t(digits[i]);
    }
    return index;
  }

  bool is_good(size_t index) const {
    return (good[index / 64] >> (index % 64)) & 1;
  }

  void visit(size_t slot, size_t index, size_t length, size_t first) {
    if (slot == symbols.size()) {
      if (length != 0) decide(index, length, first);
      return;
    }
    int const limit = int(std::min(
        size_t(counts[slot]), max_length - length));
    for (int digit = 0; digit <= limit; ++digit) {
      digits[slot] = digit;
      visit(slot + 1,
          index + ranks[rank_slot(slot, max_length - length, size_t(digit))],
          length + size_t(digit),
          digit != 0 && first == symbols.size() ? slot : first);
    }
    digits[slot] = 0;
  }

  bool completes(uint32_t node, size_t first) {
    TrieNode const& trie_node = nodes[node];
    if (trie_node.terminal && is_good(index_of(first))) return true;
    TrieEdge const* const end =
        edges.data() + trie_node.first_edge + trie_node.edge_count;
    for (TrieEdge const* edge = edges.data() + trie_node.first_edge;
         edge != end; ++edge) {
      if (digits[edge->slot] == 0) continue;
      --digits[edge->slot];
      bool const found = completes(edge->node, first);
      ++digits[edge->slot];
      if (found) return true;
    }
    return false;
  }

  void decide(size_t index, size_t length, size_t first) {
    if (length < min_length) return;
    ++decided;
    if (roots[first] != 0) {
      --digits[first];
      bool const found = completes(roots[first], first);
      ++digits[first];
      if (found) {
        good[index / 64] |= uint64_t(1) << (index % 64);
        return;
      }
    }
    std::string text;
    for (size_t i = 0; i < symbols.size(); ++i)
      text.append(size_t(digits[i]), symbols[i]);
    bad.push_back(std::move(text));
  }

  size_t const min_length;
  size_t const max_length;
  std::string symbols;
  std::vector<int> counts;
  int slot_of[UCHAR_MAX + 1];
  long double sub_bags = 0;
  size_t max_count = 0;
  // Sub-bags of at most max_length letters are indexed in increasing order
  // of their digits, the first letter's most significant. ranks[rank_slot(
  // i, budget, d)] is how many of them put a digit below d at letter i when
  // the letters before it leave `budget` letters to spend.
  std::vector<size_t> ranks;
  size_t total = 0;
  size_t decided = 0;
  std::vector<TrieNode> nodes;
  std::vector<TrieEdge> edges;
  // The root's child for each first letter: a decomposition of a sub-bag
  // must spend that sub-bag's first letter, so only words holding it are
  // tried.
  std::vector<uint32_t> roots;
  std::vector<uint64_t> good;
  std::vector<int> digits;
  std::vector<std::string> bad;
};

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

  std::filesystem::path dict_path;
  if (args.dictionary != NULL) {
    dict_path = args.dictionary;
  } else {
    char const* const root = require_workflow_root("bad-bags");
    if (root == NULL) return 1;
    dict_path = std::filesystem::path(root) / WORKFLOW_DICT_PATH;
  }
  DfsDictionary dictionary;
  if (!load_dictionary(dict_path.c_str(), &dictionary)) return 1;

  BadBags bags(args.letters, size_t(args.min_word_length),
      size_t(args.max_letters));
  if (!bags.fits()) {
    fprintf(stderr,
        "bad-bags: \"%s\" has %.0Lf sub-bags of at most %d letters, over "
        "the limit of %zu\n", args.letters.c_str(), bags.sub_bag_count(),
        args.max_letters, MAX_SUB_BAGS);
    return 1;
  }
  bags.add_words(dictionary);
  for (std::string const& bag : bags.run()) std::cout << bag << '\n';
  fprintf(stderr, "Checked %zu combinations\n", bags.checked());
  if (!(std::cout << std::flush)) {
    fputs("bad-bags: can't write output\n", stderr);
    return 1;
  }
  return 0;
}
