// Dump a Nutrimatic index file in alphabetic order with frequencies.
// Mainly used for debugging index generation. --top-words N instead prints
// the N single words with the highest aggregate counts, descending, with
// their counts only under --score.

#include "index.h"
#include "optparse.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include <queue>
#include <string>
#include <utility>
#include <vector>

static int const OPT_TOP_WORDS = 256;
static int const OPT_SCORE = 257;

static struct optparse_long const long_options[] = {
  { "top-words", OPT_TOP_WORDS, OPTPARSE_REQUIRED },
  { "score", OPT_SCORE, OPTPARSE_NONE },
  { "help", 'h', OPTPARSE_NONE },
  { NULL, 0, OPTPARSE_NONE },
};

static void usage(char const* program, FILE* out) {
  fprintf(out, "usage: %s [--top-words N [--score]] input.index\n", program);
}

struct Frontier {
  int64_t count;
  bool complete;
  IndexReader::Node node;
  std::string text;
};

// Max-heap on count; ties go to the alphabetically earlier text, and a
// complete word before a prefix with the same text.
struct FrontierLess {
  bool operator()(Frontier const& a, Frontier const& b) const {
    if (a.count != b.count) return a.count < b.count;
    if (a.text != b.text) return a.text > b.text;
    return !a.complete && b.complete;
  }
};

static void print_top_words(IndexReader const& reader, int64_t limit,
                            bool score) {
  IndexReader::CharSet allowed;
  allowed.fill();
  std::priority_queue<Frontier, std::vector<Frontier>, FrontierLess> heap;
  heap.push({reader.count(), false, reader.root(), std::string()});

  std::vector<IndexReader::Choice> choices;
  int64_t printed = 0;
  while (printed < limit && !heap.empty()) {
    Frontier top = heap.top();
    heap.pop();
    if (top.complete) {
      if (score)
        printf("%5" PRId64 " %s\n", top.count, top.text.c_str());
      else
        puts(top.text.c_str());
      ++printed;
      continue;
    }

    choices.clear();
    reader.children(top.node, top.count, allowed, &choices);
    for (IndexReader::Choice const& choice : choices) {
      if (choice.ch == ' ') {
        if (!top.text.empty())
          heap.push({choice.count, true, choice.next, top.text});
      } else {
        heap.push({choice.count, false, choice.next, top.text + choice.ch});
      }
    }
  }
}

static void dump(IndexReader const& reader) {
  IndexWalker walker(&reader, reader.root(), reader.count());
  while (walker.text != NULL) {
    printf("%5" PRId64 " [%s]\n", walker.count, walker.text);
    walker.next();
  }
}

int main(int argc, char *argv[]) {
  (void) argc;
  int64_t top_words = -1;
  bool score = false;

  struct optparse options;
  optparse_init(&options, argv);
  int opt;
  while ((opt = optparse_long(&options, long_options, NULL)) != -1) {
    switch (opt) {
      case OPT_TOP_WORDS: {
        char* end;
        errno = 0;
        long long const value = strtoll(options.optarg, &end, 10);
        if (errno != 0 || end == options.optarg || *end != '\0' ||
            value <= 0) {
          fprintf(stderr, "error: --top-words must be a positive integer\n");
          return 2;
        }
        top_words = value;
        break;
      }
      case OPT_SCORE:
        score = true;
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

  if (score && top_words <= 0) {
    fprintf(stderr, "error: --score requires --top-words\n");
    return 2;
  }

  char const* path = optparse_arg(&options);
  if (path == NULL || optparse_arg(&options) != NULL) {
    usage(argv[0], stderr);
    return 2;
  }

  FILE *fp = fopen(path, "r");
  if (fp == NULL) {
    fprintf(stderr, "error: can't open \"%s\"\n", path);
    return 1;
  }

  IndexReader reader(fp);
  if (top_words > 0)
    print_top_words(reader, top_words, score);
  else
    dump(reader);

  return 0;
}
