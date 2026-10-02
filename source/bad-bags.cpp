// bad-bags.cpp - List the sub-bags of a letter bag that no combination of
// dictionary words spells exactly.

#include <stdint.h>
#include <stdio.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "bad-bag-bitmap.h"
#include "dfs-cli-args.h"
#include "dfs-cli-help.h"
#include "dfs-class-list.h"
#include "letter-bag.h"
#include "optparse.h"
#include "sub-bag-index.h"
#include "workflow-paths.h"

namespace {

int const DEFAULT_MIN_WORD_LENGTH = 3;
int const OPT_BITMAP = 256;
int const OPT_THREADS = 257;

struct Args {
  std::string letters;
  int min_word_length = DEFAULT_MIN_WORD_LENGTH;
  int max_letters = 0;
  char const* dictionary = NULL;
  bool bitmap = false;
  int threads = 1;
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
  fputs("usage: bad-bags [-m N] [-x N] [--dict PATH] [--bitmap] "
        "[--threads N] LETTERS\n", out);
  if (out != stdout) return;

  fputs("  for each sub-bag of LETTERS holding N..X letters, decide whether\n"
        "  dictionary words of at least N letters spell it exactly; print\n"
        "  the sorted letters of every sub-bag they do not, as each is found\n"
        "\noptions:\n", stdout);
  dfs_help_option("-m, --min-word-length N",
      "use only words of at least N letters, and only sub-bags of at least "
      "N letters (default: %d)", DEFAULT_MIN_WORD_LENGTH);
  dfs_help_option("-x, --max-letters X",
      "check only sub-bags of at most X letters; X must be at least N "
      "(default: all of LETTERS)");
  dfs_help_option("--dict PATH",
      "read words from PATH (default: $WFROOT/%s)", WORKFLOW_DICT_PATH);
  dfs_help_option("--bitmap",
      "print, instead of the letters, a bitmap of which sub-bags words "
      "spell (see bad-bag-bitmap.h)");
  dfs_help_option("--threads N",
      "decide the sub-bags of each length on N threads, at most the "
      "hardware threads, 0 for all of them; "
      "with more than 1, bad sub-bags print shortest first, in no fixed "
      "order within a length (default: 1)");
  dfs_help_option("-h, --help", "show this help");
}

bool parse_args(char* argv[], Args* out, bool* help) {
  static struct optparse_long const long_options[] = {
    { "min-word-length", 'm', OPTPARSE_REQUIRED },
    { "max-letters", 'x', OPTPARSE_REQUIRED },
    { "dict", 'd', OPTPARSE_REQUIRED },
    { "bitmap", OPT_BITMAP, OPTPARSE_NONE },
    { "threads", OPT_THREADS, OPTPARSE_REQUIRED },
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
      case OPT_BITMAP:
        out->bitmap = true;
        break;
      case OPT_THREADS:
        if (!parse_count(options.optarg, "--threads", &out->threads))
          return false;
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
  BadBags(SubBagIndex const& index, size_t min_length, bool print)
      : index(index),
        min_length(min_length),
        max_length(index.max_length()),
        symbols(index.symbols()),
        counts(index.counts()),
        print(print) {}

  size_t checked() const { return decided; }
  std::vector<uint64_t> const& good_bits() const { return good; }

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
        int const slot = index.slot_of(ch);
        if (slot == SubBagIndex::SLOT_MISSING || ++need[slot] > counts[slot]) {
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

  void run(size_t threads) {
    size_t const width = symbols.size();
    good.assign(index.total() / 64 + 1, 0);
    good[0] = 1;
    rest.assign(width + 1, 0);
    for (size_t slot = width; slot-- > 0;)
      rest[slot] = rest[slot + 1] + size_t(counts[slot]);

    if (threads <= 1) {
      Walker walker(*this);
      walker.visit(0, 0, 0, width, 0);
      decided = walker.decided;
      return;
    }

    size_t prefix = 0;
    size_t tasks = 1;
    while (prefix < width && tasks < 64 * threads)
      tasks *= size_t(counts[prefix++]) + 1;

    for (size_t target = min_length; target <= max_length; ++target) {
      std::atomic<size_t> next(0);
      std::vector<Walker> walkers(std::min(threads, tasks), Walker(*this));
      std::vector<std::thread> pool;
      for (Walker& walker : walkers)
        pool.push_back(std::thread([&, target] {
          for (size_t task; (task = next++) < tasks;)
            walker.run_task(task, prefix, target);
        }));
      for (std::thread& thread : pool) thread.join();
      for (Walker const& walker : walkers) decided += walker.decided;
    }
  }

 private:
  // One thread's walk over sub-bags; the trie and `good` are shared.
  class Walker {
   public:
    explicit Walker(BadBags& bags)
        : bags(&bags), digits(bags.symbols.size(), 0) {}

    // Decides the sub-bags of `target` letters whose first `prefix` digits
    // spell `task`, read as a mixed-radix number.
    void run_task(size_t task, size_t prefix, size_t target) {
      size_t sum = 0;
      size_t length = 0;
      size_t first = digits.size();
      for (size_t slot = 0; slot < prefix; ++slot) {
        size_t const base = size_t(bags->counts[slot]) + 1;
        size_t const digit = task % base;
        task /= base;
        if (length + digit > target) return;
        digits[slot] = int(digit);
        sum += bags->index.rank(slot, bags->max_length - length, digit);
        length += digit;
        if (digit != 0 && first == digits.size()) first = slot;
      }
      visit(prefix, sum, length, first, target);
    }

    // Visits the sub-bags extending digits[0, slot), only those of `target`
    // letters unless it is 0.
    void visit(size_t slot, size_t sum, size_t length, size_t first,
               size_t target) {
      if (slot == digits.size()) {
        if (length != 0 && (target == 0 || length == target))
          decide(sum, length, first);
        return;
      }
      if (target != 0 && length + bags->rest[slot] < target) return;
      size_t const cap = target == 0 ? bags->max_length : target;
      int const limit = int(std::min(
          size_t(bags->counts[slot]), cap - length));
      for (int digit = 0; digit <= limit; ++digit) {
        digits[slot] = digit;
        visit(slot + 1,
            sum + bags->index.rank(
                slot, bags->max_length - length, size_t(digit)),
            length + size_t(digit),
            digit != 0 && first == digits.size() ? slot : first,
            target);
      }
      digits[slot] = 0;
    }

    size_t decided = 0;

   private:
    bool is_good(size_t number) {
      uint64_t const word =
          std::atomic_ref<uint64_t>(bags->good[number / 64]).load(
              std::memory_order_relaxed);
      return (word >> (number % 64)) & 1;
    }

    bool completes(uint32_t node, size_t first) {
      TrieNode const& trie_node = bags->nodes[node];
      if (trie_node.terminal &&
          is_good(bags->index.number(digits, first)))
        return true;
      TrieEdge const* const begin =
          bags->edges.data() + trie_node.first_edge;
      TrieEdge const* const end = begin + trie_node.edge_count;
      for (TrieEdge const* edge = begin; edge != end; ++edge) {
        if (digits[edge->slot] == 0) continue;
        --digits[edge->slot];
        bool const found = completes(edge->node, first);
        ++digits[edge->slot];
        if (found) return true;
      }
      return false;
    }

    void decide(size_t number, size_t length, size_t first) {
      if (length < bags->min_length) return;
      ++decided;
      if (bags->roots[first] != 0) {
        --digits[first];
        bool const found = completes(bags->roots[first], first);
        ++digits[first];
        if (found) {
          std::atomic_ref<uint64_t>(bags->good[number / 64]).fetch_or(
              uint64_t(1) << (number % 64), std::memory_order_relaxed);
          return;
        }
      }
      if (!bags->print) return;
      text.clear();
      for (size_t i = 0; i < digits.size(); ++i)
        text.append(size_t(digits[i]), bags->symbols[i]);
      text.push_back('\n');
      fwrite(text.data(), 1, text.size(), stdout);
    }

    BadBags* bags;
    std::vector<int> digits;
    std::string text;
  };

  SubBagIndex const& index;
  size_t const min_length;
  size_t const max_length;
  std::string const& symbols;
  std::vector<int> const& counts;
  bool const print;
  size_t decided = 0;
  std::vector<TrieNode> nodes;
  std::vector<TrieEdge> edges;
  // The root's child for each first letter: a decomposition of a sub-bag
  // must spend that sub-bag's first letter, so only words holding it are
  // tried.
  std::vector<uint32_t> roots;
  std::vector<uint64_t> good;
  // How many letters the slots from each one on hold.
  std::vector<size_t> rest;
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

  SubBagIndex const index(args.letters, size_t(args.max_letters));
  if (!index.fits()) {
    fprintf(stderr,
        "bad-bags: \"%s\" has %.0Lf sub-bags of at most %d letters, over "
        "the limit of %zu\n", args.letters.c_str(), index.sub_bag_count(),
        args.max_letters, SubBagIndex::MAX_SUB_BAGS);
    return 1;
  }
  BadBags bags(index, size_t(args.min_word_length), !args.bitmap);
  bags.add_words(dictionary);
  size_t threads = resolve_search_threads(args.threads);
  unsigned int const cores = std::thread::hardware_concurrency();
  if (cores != 0) threads = std::min(threads, size_t(cores));
  bags.run(threads);
  if (args.bitmap)
    write_bad_bag_bitmap(stdout, args.letters, size_t(args.min_word_length),
                         index, bags.good_bits());
  fprintf(stderr, "Checked %zu combinations\n", bags.checked());
  if (fflush(stdout) != 0 || ferror(stdout)) {
    fputs("bad-bags: can't write output\n", stderr);
    return 1;
  }
  return 0;
}
