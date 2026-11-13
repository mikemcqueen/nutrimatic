// Walk the nodes in a Nutrimatic index file from a specified starting point,
// descending recursively to the highest-frequency child nodes first.
// Mostly used for debugging and exploring the index contents.
// --next-words N instead lists the N-word continuations of "path", taken as
// a phrase, highest count first.

#include "index.h"
#include "log.h"
#include "optparse.h"
#include "parse-number.h"

#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <string.h>

#include <algorithm>
#include <queue>
#include <string>
#include <string_view>
#include <vector>

using namespace std;

bool by_count(IndexReader::Choice const& a, IndexReader::Choice const& b) {
  return a.count > b.count;
}

static void walk(IndexReader const& reader, off_t node, int64_t count,
                 const char *path, int depth, std::string *sofar) {
  if (depth == 0) return;

  std::vector<IndexReader::Choice> children;
  if (*path != '\0') {
    reader.children(node, count, *path, *path, &children);
    ++path;
  } else {
    reader.children(node, count, CHAR_MIN, CHAR_MAX, &children);
  }

  sort(children.begin(), children.end(), by_count);

  for (size_t i = 0; i < children.size(); ++i) {
    sofar->push_back(children[i].ch);
    printf("%s (%" PRId64 ") @%lld\n", sofar->c_str(),
        children[i].count, static_cast<long long>(children[i].next));
    walk(reader, children[i].next, children[i].count, path, depth - 1, sofar);
    sofar->resize(sofar->size() - 1);
  }
}

static int const OPT_NEXT_WORDS = 256;
static int const OPT_TOP = 257;

static struct optparse_long const long_options[] = {
  { "next-words", OPT_NEXT_WORDS, OPTPARSE_REQUIRED },
  { "top", OPT_TOP, OPTPARSE_REQUIRED },
  { "help", 'h', OPTPARSE_NONE },
  { NULL, 0, OPTPARSE_NONE },
};

static void usage(std::string_view program, FILE* out) {
  fprintf(out,
          "usage: %.*s input.index \"path\" [depth]\n"
          "       %.*s --next-words N [--top K] input.index \"phrase\"\n",
          SV_ARG(program), SV_ARG(program));
}

struct Continuation {
  int64_t count;
  int words;
  bool complete;
  IndexReader::Node node;
  std::string text;
};

struct ContinuationLess {
  bool operator()(Continuation const& a, Continuation const& b) const {
    if (a.count != b.count) return a.count < b.count;
    if (a.text != b.text) return a.text > b.text;
    return !a.complete && b.complete;
  }
};

static void print_next_words(IndexReader const& reader, std::string phrase,
                             int next_words, int64_t top) {
  if (!phrase.empty() && phrase.back() != ' ') phrase.push_back(' ');

  IndexReader::Node node = reader.root();
  int64_t count = reader.count();
  std::vector<IndexReader::Choice> choices;
  for (char const c : phrase) {
    choices.clear();
    reader.children(node, count, c, c, &choices);
    if (choices.empty()) return;
    node = choices[0].next;
    count = choices[0].count;
  }

  IndexReader::CharSet allowed;
  allowed.fill();
  std::priority_queue<Continuation, std::vector<Continuation>,
                      ContinuationLess> heap;
  heap.push({count, 0, false, node, phrase});

  int64_t printed = 0;
  while (printed < top && !heap.empty()) {
    Continuation top_entry = heap.top();
    heap.pop();
    if (top_entry.complete) {
      printf("%5" PRId64 " %s\n", top_entry.count, top_entry.text.c_str());
      ++printed;
      continue;
    }

    choices.clear();
    reader.children(top_entry.node, top_entry.count, allowed, &choices);
    for (IndexReader::Choice const& choice : choices) {
      if (choice.ch != ' ') {
        heap.push({choice.count, top_entry.words, false, choice.next,
                   top_entry.text + choice.ch});
      } else if (!top_entry.text.empty() && top_entry.text.back() != ' ') {
        int const words = top_entry.words + 1;
        if (words == next_words)
          heap.push({choice.count, words, true, choice.next, top_entry.text});
        else
          heap.push({choice.count, words, false, choice.next,
                     top_entry.text + ' '});
      }
    }
  }
}

int main(int argc, char *argv[]) {
  (void) argc;
  int next_words = 0;
  int64_t top = -1;

  struct optparse options;
  optparse_init(&options, argv);
  int opt;
  while ((opt = optparse_long(&options, long_options, NULL)) != -1) {
    switch (opt) {
      case OPT_NEXT_WORDS:
        if (!parse_number(options.optarg, &next_words) || next_words <= 0) {
          fprintf(stderr, "error: --next-words must be a positive integer\n");
          return 2;
        }
        break;
      case OPT_TOP:
        if (!parse_number(options.optarg, &top) || top <= 0) {
          fprintf(stderr, "error: --top must be a positive integer\n");
          return 2;
        }
        break;
      case 'h':
        usage(argv[0], stdout);
        return 0;
      default:
        fprintf(stderr, "error: %s\n", options.errmsg);
        usage(argv[0], stderr);
        return 2;
    }
  }

  if (top > 0 && next_words == 0) {
    fprintf(stderr, "error: --top requires --next-words\n");
    return 2;
  }

  char const* const index_path = optparse_arg(&options);
  char const* const path = optparse_arg(&options);
  char const* const depth_arg = optparse_arg(&options);
  if (index_path == NULL || path == NULL ||
      (depth_arg != NULL && next_words > 0) || optparse_arg(&options) != NULL) {
    usage(argv[0], stderr);
    return 2;
  }

  FILE *input = fopen(index_path, "rb");
  if (input == NULL) {
    fprintf(stderr, "error: can't open \"%s\"\n", index_path);
    return 1;
  }

  IndexReader reader(input);

  if (next_words > 0) {
    print_next_words(reader, path, next_words,
                     top > 0 ? top : INT64_MAX);
    return 0;
  }

  printf("Root (%" PRId64 ") @%lld\n", reader.count(),
      static_cast<long long>(reader.root()));

  int depth = strlen(path);
  if (depth_arg != NULL) {
    depth = atoi(depth_arg);
    if (depth == 0) {
      fprintf(stderr, "error: invalid depth \"%s\"\n", depth_arg);
      exit(2);
    }
  }

  std::string sofar = "";
  walk(reader, reader.root(), reader.count(), path, depth, &sofar);
  return 0;
}
