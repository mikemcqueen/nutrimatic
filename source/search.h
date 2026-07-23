#include <stdio.h>

#include <deque>
#include <queue>
#include <set>
#include <string>
#include <vector>

struct SearchFilter {
  typedef int State;
  virtual bool is_accepting(State state) const = 0;
  virtual bool has_transition(State from, char ch, State* to) const = 0;
  virtual ~SearchFilter() { }
};

class SearchDriver {
 public:
  const char* text;
  double score;

  // With "canonical_order" set, a path may only start a new segment whose trie
  // node is >= the last one it started, which keeps one arrangement of each set
  // of segments instead of all of them.  See Next::last_seg.
  SearchDriver(const IndexReader*,
               const SearchFilter*,
               SearchFilter::State start,
               double restart,
               bool canonical_order = false);

  bool step();
  void next() { while (!step()) ; }

 private:
  // Millions of these live in the frontier queue at once, so the field order
  // matters: the two 4-byte members are paired up front to share one 8-byte
  // slot rather than each costing 8 with padding.
  struct Next {
    int crumb;
    SearchFilter::State state;
    double scale;
    IndexReader::Choice choice;

    // A "segment" is the text consumed between two restarts -- usually one
    // word, but a phrase found contiguously in the corpus is a single segment
    // spanning several.  A trie node names the whole string from the root down
    // to it, so the node a segment ends on identifies that segment exactly,
    // for free, and ordering by it is an arbitrary but total order over
    // segments; that is all a canonical order needs to be.  This holds only
    // because IndexWriter never coalesces identical subtrees into one node.
    //
    // Leaves are the exception: they all report a node of -1 (see
    // IndexReader::children), so they can't be told apart and are left
    // unordered, carrying the previous value forward.  0 means "no segment
    // finished yet" -- IndexWriter always writes a byte before taking a node's
    // offset, so no real node lands there.
    IndexReader::Node last_seg;

    bool operator<(Next const& n) const {
      return choice.count * scale < n.choice.count * n.scale;
    }
  };

  static_assert(sizeof(Next) == 16 + sizeof(IndexReader::Choice) +
                               sizeof(IndexReader::Node),
                "Next has padding; check the field order");

  struct Crumb {
    int parent;
    char ch;
  };

  // True if "n" sits at the end of a segment that sorts before the segment the
  // path most recently started, meaning some other path is walking the same set
  // of segments in canonical order and this one is a redundant permutation.
  bool out_of_order(Next const& n) const;

  std::priority_queue<Next> nexts;
  std::deque<Crumb> crumbs;
  std::vector<IndexReader::Choice> tmp;
  std::set<std::string> seen;
  const IndexReader* const reader;
  const SearchFilter* const filter;
  const double restart;
  const bool canonical;
};

// Prints "score text" for every match to stdout, and a "# <steps>" progress
// line every 100k * progress_factor steps to "progress".  cgi-search.py parses
// the progress lines out of find-expr's stdout to enforce its computation
// limit, so that tool must keep them there (and must keep the interval it
// expects); tools meant to be used in a shell pipeline can pass stderr instead
// to keep stdout clean, and can space the lines out with progress_factor.
void PrintAll(SearchDriver*, FILE* progress, int progress_factor = 1);
