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

  SearchDriver(const IndexReader*,
               const SearchFilter*,
               SearchFilter::State start,
               double restart);

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
    bool operator<(Next const& n) const {
      return choice.count * scale < n.choice.count * n.scale;
    }
  };

  static_assert(sizeof(Next) == 16 + sizeof(IndexReader::Choice),
                "Next has padding; check the field order");

  struct Crumb {
    int parent;
    char ch;
  };

  std::priority_queue<Next> nexts;
  std::deque<Crumb> crumbs;
  std::vector<IndexReader::Choice> tmp;
  std::set<std::string> seen;
  const IndexReader* const reader;
  const SearchFilter* const filter;
  const double restart;
};

// Prints "score text" for every match to stdout, and a "# <steps>" progress
// line every 100k * progress_factor steps to "progress".  cgi-search.py parses
// the progress lines out of find-expr's stdout to enforce its computation
// limit, so that tool must keep them there (and must keep the interval it
// expects); tools meant to be used in a shell pipeline can pass stderr instead
// to keep stdout clean, and can space the lines out with progress_factor.
void PrintAll(SearchDriver*, FILE* progress, int progress_factor = 1);
