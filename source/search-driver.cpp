#include "index.h"
#include "search.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>

#include <algorithm>
#include <limits>

using namespace std;

// The node IndexReader::children() reports for a childless node; such nodes
// are indistinguishable from each other, so they can't order anything.
static const IndexReader::Node LEAF = (IndexReader::Node) -1;

IndexReader::Node SearchDriver::node_of(Node_t n) {
  return n == LEAF_NODE ? LEAF : IndexReader::Node(n);
}

// Never collect a history smaller than this.  A Crumb is 8 bytes, so this says
// the history isn't worth reclaiming below ~240 MB, which is noise next to the
// multi-gigabyte frontier that motivates collecting in the first place.  It is
// not a performance guard: a collection costs a pass over the history, and the
// threshold below never lets one follow another without real growth between.
static const size_t MIN_CRUMBS = 30000000;

// The multiplier from the live count to the next collection threshold, as a
// function of how much of the history the collection just reclaimed.  The two
// branches meet at 1.25, so this is continuous.
//
// Above the 25% knee, (1 + freed) * live is roughly the size the history was
// when this collection ran: a collection that keeps paying for itself is
// allowed to keep running at the same size rather than at a shrinking one, so
// its cost stays flat instead of falling off with the live count.
//
// Below the knee, the multiplier grows as the yield drops -- to 1.5 at zero
// yield -- so a history that really is mostly live isn't rescanned to
// rediscover that.  The growth is bounded and per-collection, not compounding:
// a stretch of barren collections can't ratchet the threshold up and leave the
// first collectable moment arriving many times too late.
static double gc_slack(double freed) {
  return freed < 0.25 ? 1.5 - freed : 1.0 + freed;
}

namespace {

// A live/dead bit per crumb, plus enough summary to answer "how many live
// crumbs are below index i" in constant time.  That answer *is* the crumb's
// index after compaction -- compaction slides the live ones down in order --
// so no separate remap table is needed.  A remap array would cost 4 bytes per
// crumb, which is 144 MB at 36M crumbs; this is ~1.06 bits per crumb, or ~5 MB,
// and it's allocated at exactly the moment memory is tightest.
class Marks {
 public:
  explicit Marks(size_t n): words((n + 63) / 64), bits(words, 0),
      blocks(words / WORDS_PER_BLOCK + 1, 0), live(0) { }

  // Marks "i" and returns false if it was already marked.  Callers walking a
  // parent chain can stop on false: everything above is marked already.
  bool mark(size_t i) {
    uint64_t const bit = uint64_t(1) << (i % 64);
    if (bits[i / 64] & bit) return false;
    bits[i / 64] |= bit;
    return true;
  }

  bool marked(size_t i) const { return bits[i / 64] >> (i % 64) & 1; }

  // Must be called once after the last mark() and before the first rank().
  void summarize() {
    uint32_t total = 0;
    for (size_t w = 0; w < words; ++w) {
      if (w % WORDS_PER_BLOCK == 0) blocks[w / WORDS_PER_BLOCK] = total;
      total += __builtin_popcountll(bits[w]);
    }
    live = total;
  }

  // The number of marked indexes strictly below "i".
  size_t rank(size_t i) const {
    size_t const w = i / 64;
    size_t const b = w / WORDS_PER_BLOCK;
    uint32_t r = blocks[b];
    for (size_t j = b * WORDS_PER_BLOCK; j < w; ++j)
      r += __builtin_popcountll(bits[j]);
    // Shifting by 64 would be undefined, but i % 64 < 64 always, and at 0 this
    // correctly masks everything off.
    return r + __builtin_popcountll(bits[w] & ((uint64_t(1) << (i % 64)) - 1));
  }

  size_t live_count() const { return live; }

 private:
  static constexpr size_t WORDS_PER_BLOCK = 8;  // 512 bits per summary entry

  size_t words;
  vector<uint64_t> bits;
  vector<uint32_t> blocks;
  size_t live;
};

}  // namespace

SearchDriver::SearchDriver(const IndexReader* r,
                           const SearchFilter* f,
                           SearchFilter::State start,
                           double rp,
                           bool canonical_order):
    reader(r), filter(f), restart(rp),
    log_restart(rp > 0.0 ? log2f(float(rp)) : 0.0f),
    canonical(canonical_order) {
  // A frontier entry holds counts and nodes narrowed to 32 bits (see Count_t
  // and Node_t).  Both bounds are knowable here and nowhere else, and neither
  // failure is detectable downstream: a wrapped count would ride into a
  // reported score as a plausible number, and a wrapped node would name a real
  // but wrong place in the index.  So this is a hard check, not an assert --
  // it has to survive an NDEBUG build.
  if (r->count() > int64_t(numeric_limits<Count_t>::max())) {
    fprintf(stderr,
            "error: index total %lld exceeds the %u a frontier entry holds;"
            " widen SearchDriver::Count_t in search.h\n",
            (long long) r->count(),
            (unsigned) numeric_limits<Count_t>::max());
    exit(1);
  }

  // ">=" rather than ">": the top of the range is spoken for by LEAF_NODE.
  if (uint64_t(r->root()) >= uint64_t(LEAF_NODE)) {
    fprintf(stderr,
            "error: index length %lld exceeds the %u a frontier entry holds;"
            " widen SearchDriver::Node_t in search.h\n",
            (long long) r->root(), (unsigned) LEAF_NODE);
    exit(1);
  }

  Next seed;
  seed.crumb = -1;
  seed.ch = '\0';
  seed.node = Node_t(reader->root());
  seed.count = Count_t(reader->count());
  seed.log_score = log2f(float(reader->count()));  // scale is 1 at the root
  seed.state = start;
  seed.last_seg = 0;

  // The seed sits at the corpus total with a scale of 1, so nothing the search
  // reaches can score above it; that makes it the queue's bucket-zero datum.
  nexts.top_log = seed.log_score;
  nexts.push(seed);

  gc_threshold = MIN_CRUMBS;
  gc_progress = NULL;

  text = NULL;
  score = 0;
}

void SearchDriver::collect() {
  size_t const before = crumbs.size();

  Marks marks(before);
  for (size_t b = nexts.cur; b < nexts.buckets.size(); ++b)
    for (size_t i = 0; i < nexts.buckets[b].size(); ++i)
      for (int c = nexts.buckets[b][i].crumb; c >= 0; c = crumbs[c].parent)
        if (!marks.mark(c)) break;
  marks.summarize();

  // A crumb is always appended after its parent (only a surviving child causes
  // the push), so parent < index everywhere and one forward pass suffices: by
  // the time a crumb moves, its parent's new index is already known.  The
  // destination is just the number of live crumbs seen so far, which is what
  // marks.rank() would return for this index.
  size_t out = 0;
  for (size_t i = 0; i < before; ++i) {
    if (!marks.marked(i)) continue;
    Crumb c = crumbs[i];
    if (c.parent >= 0) c.parent = int(marks.rank(c.parent));
    crumbs[out++] = c;
  }
  assert(out == marks.live_count());
  crumbs.resize(out);

  // Renumbering the frontier can't disturb the queue's order: an entry's
  // bucket is a function of log_score alone.  The seed's -1 means "no history"
  // and stays put.
  for (size_t b = nexts.cur; b < nexts.buckets.size(); ++b)
    for (size_t i = 0; i < nexts.buckets[b].size(); ++i) {
      int& c = nexts.buckets[b][i].crumb;
      if (c >= 0) c = int(marks.rank(c));
    }

  double const reclaimed = before > 0 ? 1.0 - double(out) / double(before) : 0.0;
  double const slack = gc_slack(reclaimed);
  gc_threshold = max(MIN_CRUMBS, size_t(double(out) * slack));

  if (gc_progress != NULL) {
    fprintf(gc_progress,
            "# collect: freed %zu of %zu crumbs (%.1f%%), %zu live,"
            " next at %zu (slack %.3g)\n",
            before - out, before, reclaimed * 100.0, out, gc_threshold, slack);
    fflush(gc_progress);
  }
}

bool SearchDriver::out_of_order(Next const& n) const {
  return canonical &&
      n.ch == ' ' &&  // only a space ends a segment
      n.node != LEAF_NODE &&
      n.node < n.last_seg;
}

std::string SearchDriver::make_seen_key(std::string const& match) {
  vector<string> words;
  for (size_t i = 0; i < match.size(); ) {
    size_t const end = match.find(' ', i);
    if (end != i) words.push_back(match.substr(i, end - i));
    if (end == string::npos) break;
    i = end + 1;
  }

  sort(words.begin(), words.end());

  string key;
  for (size_t i = 0; i < words.size(); ++i) {
    if (i > 0) key += ' ';
    key += words[i];
  }
  return key;
}

double SearchDriver::queue_median_score() const {
  if (nexts.empty()) return 0.0;

  // The buckets are already ordered by score, so the median is the score at
  // the bucket holding the middle entry -- no selection pass at all.  Buckets
  // are 1/16 of a log2 unit wide, which is finer than the four digits the
  // progress line shows.
  size_t half = nexts.size() / 2, seen_so_far = 0;
  for (size_t b = nexts.cur; b < nexts.buckets.size(); ++b) {
    seen_so_far += nexts.buckets[b].size();
    if (seen_so_far > half)
      return exp2(double(nexts.top_log) - double(b) / NextQueue::SCALE);
  }
  return 0.0;
}


bool SearchDriver::step() {
  if (nexts.empty()) {
    text = NULL;
    score = 0;
    return true;
  }

  // Before the pop, while the frontier is the whole root set.  See collect().
  if (crumbs.size() >= gc_threshold) collect();

  const Next next = nexts.top(); nexts.pop();

  Next new_next;
  new_next.crumb = crumbs.size();
  // Inherited by every character transition; only a restart advances it.
  new_next.last_seg = next.last_seg;

  // The children share this path's scale but not its count, and log_score folds
  // the two together, so strip the count out once here rather than per child.
  float const log_scale = next.log_score - log2f(float(next.count));

  tmp.clear();
  reader->children(node_of(next.node), next.count, CHAR_MIN, CHAR_MAX, &tmp);
  for (size_t i = 0; i < tmp.size(); ++i) {
    assert(tmp[i].count > 0);
    if (filter->has_transition(next.state, tmp[i].ch, &new_next.state)) {
      if (int(crumbs.size()) == new_next.crumb) {
        Crumb new_crumb;
        new_crumb.parent = next.crumb;
        new_crumb.ch = next.ch;
        crumbs.push_back(new_crumb);
      }
      new_next.ch = tmp[i].ch;
      new_next.count = Count_t(tmp[i].count);
      new_next.node = Node_t(tmp[i].next);
      new_next.log_score = log_scale + log2f(float(tmp[i].count));
      nexts.push(new_next);
    }
  }

  // A match's last segment never restarts -- for an anagram the accepting state
  // is terminal and has no transitions out -- so this is the only place its
  // order gets checked.  Without it the last segment would be unconstrained and
  // every set of k segments would still be reported in k arrangements.
  // Note a suppressed match is deliberately kept out of "seen": it shares its
  // key with the canonical arrangement, which must stay free to be reported.
  if (filter->is_accepting(next.state) && next.crumb != -1 &&
      !out_of_order(next)) {
    size_t len = 0;
    for (int i = next.crumb; i >= 0; i = crumbs[i].parent)
      ++len;

    match.assign(len--, next.ch);
    for (int i = next.crumb; i >= 0 && len > 0; i = crumbs[i].parent)
      match[--len] = crumbs[i].ch;
    assert(len == 0);

    if (seen.insert(make_seen_key(match)).second) {
      text = match.c_str();
      // Reconstructed from the log, so this carries the float's ~5 parts in
      // 10^5 at the depths where scores get small.  PrintAll shows four
      // significant digits, so the last one can differ from what an exact
      // product would have printed; nothing consumes the score as an exact
      // quantity, and the ordering it came from is unaffected.
      score = exp2(double(next.log_score));
      return true;
    }
  }

  // Suppressing the restart doesn't kill the path: the ' ' child pushed above
  // carries it on contiguously, so all that's forbidden is *starting* a new
  // segment out of order.
  if (restart > 0.0 &&
      next.ch == ' ' &&
      next.node != Node_t(reader->root()) &&
      !out_of_order(next)) {
    new_next.crumb = next.crumb;
    new_next.ch = next.ch;
    new_next.count = Count_t(reader->count());
    new_next.node = Node_t(reader->root());
    // The restart scales by count/total * restart and then sits at a count of
    // total again, so the two totals cancel and the whole step is a constant
    // in log space -- no division, and exactly the old arithmetic.
    new_next.log_score = next.log_score + log_restart;
    new_next.state = next.state;
    // The segment just finished becomes the floor for the next one.  A leaf
    // can't be named, so it orders nothing and the old floor stands.
    if (next.node != LEAF_NODE) new_next.last_seg = next.node;
    nexts.push(new_next);
  }

  return false;
}
