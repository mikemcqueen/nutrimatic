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

  // Progress reporting: how many distinct matches have been reported so far,
  // how big the frontier is, and how much never-released path history is behind
  // it.  Note "text" points at storage owned by the driver and reused by the
  // next step(), so copy it if it must outlive that.
  size_t seen_size() const { return seen.size(); }
  size_t queue_size() const { return nexts.size(); }
  size_t crumbs_size() const { return crumbs.size(); }

  // Report each collection of path history to this stream, or NULL (the
  // default) for silence.  Deliberately not tied to PrintAll's progress stream:
  // find-expr points that at stdout, where cgi-search.py parses "# <steps>" and
  // reads the whole rest of the line as the step count, so an extra line there
  // would be taken for a step count.  Callers that send progress to stderr can
  // safely pass it here.  Lines start with '#', like the progress lines, so one
  // filter drops both.
  void report_collections(FILE* fp) { gc_progress = fp; }

  // The score of the median entry in the frontier, or 0 if it's empty.  A heap
  // is unordered below its top, so this is a linear-time selection over a copy
  // of the scores -- cheap enough once per progress line, not per step.
  double queue_median_score() const;

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

  // Drop the crumbs no path can still reach and renumber what's left.  A crumb
  // is live only if it's an ancestor of some frontier entry's crumb, so the
  // frontier is the root set and this may only run with the queue whole -- at
  // the top of step(), before the pop, since afterwards the popped entry (and
  // later the half-built child crumb) are roots too.  Reported matches pin
  // nothing: "text" points into "match", which is a copy.
  void collect();

  // std::priority_queue keeps its backing container as a protected member, so
  // a trivial subclass is the sanctioned way to reach the whole frontier --
  // needed to sample scores below the top for queue_median_score().
  struct NextQueue : std::priority_queue<Next> {
    using std::priority_queue<Next>::c;
  };

  // The words of a match, sorted and rejoined: the key a match is deduplicated
  // under.  See "seen".
  static std::string make_seen_key(std::string const& match);

  NextQueue nexts;
  std::vector<IndexReader::Choice> tmp;

  // Path history: every reported match is reconstructed by walking "parent"
  // links back to the root.  A deque rather than a vector because collect()
  // shrinks it, and a deque hands whole blocks back instead of reallocating --
  // a doubling-sized spike is the failure this whole exercise is about.
  std::deque<Crumb> crumbs;

  // When "crumbs" reaches this size, collect().  Set from the live count after
  // each collection, so the cost of collecting is bounded by how much has been
  // allocated since the last one.  "gc_slack" is the multiplier: it doubles
  // (up to MAX_GC_SLACK) whenever a collection reclaims almost nothing, so a
  // search whose history really is all live stops paying to rediscover that.
  size_t gc_threshold;
  double gc_slack;
  FILE* gc_progress;

  // Matches already reported, keyed by make_seen_key() rather than by the match
  // itself, so a set of words is reported once instead of once per arrangement.
  // Rearrangements are not merely redundant output: for an anagram they are the
  // bulk of it, since the search reaches every ordering of every word set it
  // finds.  The arrangement kept is the best-scoring one, because a path's
  // priority never rises -- a character transition holds "scale" and can only
  // shrink "count", and a restart multiplies by "restart" < 1 -- so matches are
  // popped in non-increasing score order and the first one wins.
  std::set<std::string> seen;

  // Owns what "text" points at; make_seen_key() is what "seen" holds, so the
  // match itself needs storage of its own.
  std::string match;

  const IndexReader* const reader;
  const SearchFilter* const filter;
  const double restart;
  const bool canonical;
};

// Prints "score text" for every match to stdout, and a progress line
//
//   # <steps> seen(<matches>) crumbs(<path history>) queue(<frontier>)
//     median(<frontier median score>)
//
// every 100k * progress_factor steps to "progress".  cgi-search.py parses the
// progress lines out of find-expr's stdout to enforce its computation limit,
// so that tool must keep them there (and must keep the interval it expects);
// note it reads the whole rest of the line as the step count, so it needs
// updating for the fields after <steps>.  Tools meant to be used in a shell
// pipeline can pass stderr instead to keep stdout clean, and can space the
// lines out with progress_factor.
void PrintAll(SearchDriver*, FILE* progress, int progress_factor = 1);
