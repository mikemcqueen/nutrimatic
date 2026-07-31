#include <stdint.h>
#include <stdio.h>

#include <deque>
#include <set>
#include <string>
#include <vector>

struct SearchFilter {
  typedef int State;
  virtual bool is_accepting(State state) const = 0;
  virtual bool has_transition(State from, char ch, State* to) const = 0;

  // A superset of the characters has_transition() can accept from "state".
  // The driver asks once per step and hands the answer to
  // IndexReader::children(), which then skips decoding the count and node
  // offset of every child outside it -- measured at ~74% of them for an
  // anagram, since most letters of the alphabet are not in the bag or are
  // already spent.  has_transition() still decides, so a filter that
  // overapproximates here is correct, merely slower; the default admits
  // everything, which is exactly the old behaviour.
  virtual void allowed_chars(State, IndexReader::CharSet* out) const {
    out->fill();
  }

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

  // The score of the median entry in the frontier, or 0 if it's empty.  The
  // frontier is bucketed by score, so this walks the bucket sizes to find the
  // one holding the middle entry and reports that bucket's score -- constant
  // work per bucket and none per entry.  Resolution is the bucket width, which
  // is finer than the four digits the progress line prints.
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

  // A monotone bucket queue, in place of the binary heap this used to be.
  //
  // The search never raises a priority -- a character transition holds the
  // scale and can only shrink the count, and a restart adds log2(restart) < 0
  // -- so the maximum only ever falls.  That makes a bucket queue applicable:
  // entries land in a bucket chosen by their quantized log_score, and a cursor
  // walks the buckets downward once, never back.  Push appends to a bucket's
  // tail and pop takes from the current bucket's tail, so both are O(1) and
  // touch memory sequentially, where the heap spent ~24 dependent cache misses
  // sifting the last element back down on every pop -- measured as 29% of
  // total runtime on one compare instruction.  See findings/perf-analysis.md.
  //
  // The order this gives up is *within* a bucket, where entries come out
  // last-in-first-out rather than by exact score.  That is the same class of
  // approximation findings/priority-invariant.md already records, just larger:
  // the arrangement "seen" keeps for a set of words can now be any of those
  // scoring within a bucket width of the best, and printed matches are
  // descending only to that tolerance.
  struct NextQueue {
    // Buckets per log2 unit of score.  Entries inside one bucket come out in
    // LIFO order rather than by exact score, so this sets how far the pop order
    // may depart from exact: 1/16 of a log2 unit is 4.4% in score.
    static constexpr float SCALE = 16.0f;

    std::vector<std::vector<Next> > buckets;
    float top_log;   // log_score of the seed: no entry can exceed it
    size_t cur;      // bucket the next pop comes from; only ever advances
    size_t count;

    NextQueue(): top_log(0), cur(0), count(0) { }

    size_t bucket_of(float log_score) const {
      float const d = (top_log - log_score) * SCALE;
      return d <= 0.0f ? 0 : size_t(d);
    }

    bool empty() const { return count == 0; }
    size_t size() const { return count; }

    void push(Next const& n) {
      // Clamping to "cur" covers the one case where a child can outrank its
      // parent: step() rebuilds log_score by subtracting the parent's count out
      // and adding the child's back in, which can land an ulp high.  Without
      // the clamp that entry would go to a bucket the cursor has passed and
      // never be popped.  The error it introduces is under one bucket.
      size_t b = bucket_of(n.log_score);
      if (b < cur) b = cur;
      if (b >= buckets.size()) buckets.resize(b + 1);
      buckets[b].push_back(n);
      ++count;
    }

    void settle() {
      while (cur < buckets.size() && buckets[cur].empty()) {
        // Hand this bucket's storage back as the cursor leaves it.  A heap
        // holds one allocation that only ever doubles; here the memory behind
        // exhausted scores is released as the search descends past them.
        std::vector<Next>().swap(buckets[cur]);
        ++cur;
      }
    }

    Next const& top() { settle(); return buckets[cur].back(); }
    void pop() { settle(); buckets[cur].pop_back(); --count; }
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
void PrintAll(SearchDriver*, FILE* progress, int64_t progress_factor = 1);
