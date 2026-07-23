#include <stdint.h>
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
  // The width a frontier entry holds a trie node in.  Nodes are file offsets,
  // so IndexReader::root() -- the index length -- bounds every one of them, and
  // the constructor refuses an index too large to address this way.  Kept
  // separate from IndexReader::Node so the index format stays 64-bit: this is a
  // decision about how the frontier is stored, not about what can be indexed.
  using Node_t = uint32_t;

  // The node IndexReader::children() reports for a childless node, as stored in
  // Next::node.  Narrowing one needs no special case, since truncating all-ones
  // stays all-ones -- but widening does: zero-extending this gives 4294967295,
  // not the -1 that children() tests for, and handing that over would walk into
  // the middle of the index instead of stopping.  Every stored node goes back
  // to the reader through node_of() for that reason.
  static constexpr Node_t LEAF_NODE = Node_t(-1);
  static IndexReader::Node node_of(Node_t n);

  // The width a frontier entry holds an occurrence count in.  Every count
  // stored here is bounded by IndexReader::count(): children() reports node
  // counts, which are sums over a subtree, and the seed and every restart store
  // that total itself.  So the constructor's single check on it covers every
  // count the search can produce, and no count can wrap unnoticed.  Widen this
  // to int64_t if that check ever fires.
  using Count_t = uint32_t;

  // Millions of these live in the frontier queue at once, and the queue is by
  // far the largest thing the search allocates, so each field is cut to the
  // narrowest width its values provably fit in and ordered to leave no padding
  // except the three bytes after "ch".  IndexReader::Choice is unpacked into
  // "ch", "count" and "node" rather than embedded, because its own layout
  // spends 7 bytes padding "ch" out to the alignment of a 64-bit count -- more
  // than everything saved by narrowing the nodes.
  struct Next {
    int crumb;
    SearchFilter::State state;

    // log2 of this entry's priority, which is "scale" (the product of the
    // restart penalties behind it) times "count".  Kept in log space rather
    // than as the scale itself because each restart multiplies by
    // count/total * restart, around 2^-36 with the usual restart of 1e-6: a
    // double scale underflows to zero after ~28 of them, and every path past
    // that depth would tie at zero and lose its ordering silently.  A float
    // holds ~5 parts in 10^5 out at the -1000 the deepest paths reach, far
    // finer than the search can act on.
    float log_score;

    Node_t node;    // the trie node this entry sits on
    Count_t count;  // occurrences of the string ending at "node"

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
    Node_t last_seg;

    char ch;  // the character consumed to arrive at "node"

    // Comparing the logs orders the same as comparing the priorities, since
    // log2 is monotonic -- and it is one float compare rather than a multiply
    // per side, on the hottest path in the search.
    bool operator<(Next const& n) const { return log_score < n.log_score; }
  };

  static_assert(sizeof(Next) == 28,
                "Next has unexpected padding; check the field order");

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
  // shrinks it, and a deque hands whole blocks back instead of reallocating.
  std::deque<Crumb> crumbs;

  // When "crumbs" reaches this size, collect().  Set after each collection from
  // the live count times gc_slack(), which reads the yield just measured -- see
  // search-driver.cpp -- so the cost of collecting is bounded by how much has
  // been allocated since the last one.
  size_t gc_threshold;
  FILE* gc_progress;

  // Matches already reported, keyed by make_seen_key() rather than by the match
  // itself, so a set of words is reported once instead of once per arrangement.
  // Rearrangements are not merely redundant output: for an anagram they are the
  // bulk of it, since the search reaches every ordering of every word set it
  // finds.  The arrangement kept is the best-scoring one, because a path's
  // priority never rises -- a character transition holds the scale and can only
  // shrink the count, and a restart adds log2(restart) < 0 -- so matches are
  // popped in non-increasing score order and the first one wins.
  //
  // In exact arithmetic that ordering is strict.  step() rebuilds a child's
  // log_score by subtracting the parent's count out and adding the child's
  // back in, though, and where the two counts are equal -- which they are
  // whenever children() passes a count through unchanged -- that round trip
  // can land an ulp above where it started.  So a child may outrank its parent
  // by ~1e-3 in log2 after a long path.  All that costs is the arrangement
  // chosen between matches whose scores agree to that precision, which is
  // finer than the four digits PrintAll shows.
  std::set<std::string> seen;

  // Owns what "text" points at; make_seen_key() is what "seen" holds, so the
  // match itself needs storage of its own.
  std::string match;

  const IndexReader* const reader;
  const SearchFilter* const filter;
  const double restart;

  // log2(restart), the whole cost of a restart in log space: the new entry
  // sits at the corpus total, which cancels the /total in the restart factor,
  // so a restart is exactly this much added to the popped entry's log_score.
  // Zero when "restart" is, which would read as a free restart rather than a
  // forbidden one -- safe only because step() gates restarting on "restart"
  // itself, so this is never reached in that case.
  const float log_restart;

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
