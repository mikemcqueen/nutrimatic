#include "index.h"
#include "search.h"

#include <assert.h>
#include <limits.h>

#include <algorithm>

using namespace std;

// The node IndexReader::children() reports for a childless node; such nodes
// are indistinguishable from each other, so they can't order anything.
static const IndexReader::Node LEAF = (IndexReader::Node) -1;

SearchDriver::SearchDriver(const IndexReader* r,
                           const SearchFilter* f,
                           SearchFilter::State start,
                           double rp,
                           bool canonical_order):
    reader(r), filter(f), restart(rp), canonical(canonical_order) {
  Next seed;
  seed.crumb = -1;
  seed.scale = 1.0;
  seed.choice.ch = '\0';
  seed.choice.next = reader->root();
  seed.choice.count = reader->count();
  seed.state = start;
  seed.last_seg = 0;
  nexts.push(seed);

  text = NULL;
  score = 0;
}

bool SearchDriver::out_of_order(Next const& n) const {
  return canonical &&
      n.choice.ch == ' ' &&  // only a space ends a segment
      n.choice.next != LEAF &&
      n.choice.next < n.last_seg;
}

bool SearchDriver::step() {
  if (nexts.empty()) {
    text = NULL;
    score = 0;
    return true;
  }

  const Next next = nexts.top(); nexts.pop();

  Next new_next;
  new_next.crumb = crumbs.size();
  new_next.scale = next.scale;
  // Inherited by every character transition; only a restart advances it.
  new_next.last_seg = next.last_seg;

  tmp.clear();
  reader->children(next.choice.next, next.choice.count, CHAR_MIN, CHAR_MAX, &tmp);
  for (size_t i = 0; i < tmp.size(); ++i) {
    assert(tmp[i].count > 0);
    if (filter->has_transition(next.state, tmp[i].ch, &new_next.state)) {
      if (int(crumbs.size()) == new_next.crumb) {
        Crumb new_crumb;
        new_crumb.parent = next.crumb;
        new_crumb.ch = next.choice.ch;
        crumbs.push_back(new_crumb);
      }
      new_next.choice = tmp[i];
      nexts.push(new_next);
    }
  }

  // A match's last segment never restarts -- for an anagram the accepting state
  // is terminal and has no transitions out -- so this is the only place its
  // order gets checked.  Without it the last segment would be unconstrained and
  // every set of k segments would still be reported in k arrangements.
  // Note a suppressed match is deliberately kept out of "seen": the canonical
  // arrangement is a different string and must stay free to be reported.
  if (filter->is_accepting(next.state) && next.crumb != -1 &&
      !out_of_order(next)) {
    size_t len = 0;
    for (int i = next.crumb; i >= 0; i = crumbs[i].parent)
      ++len;

    std::string buffer(len--, next.choice.ch);
    for (int i = next.crumb; i >= 0 && len > 0; i = crumbs[i].parent)
      buffer[--len] = crumbs[i].ch;
    assert(len == 0);

    pair<set<string>::iterator, bool> ib = seen.insert(buffer);
    if (ib.second) {
      text = ib.first->c_str();
      score = next.scale * next.choice.count;
      return true;
    }
  }

  // Suppressing the restart doesn't kill the path: the ' ' child pushed above
  // carries it on contiguously, so all that's forbidden is *starting* a new
  // segment out of order.
  if (restart > 0.0 &&
      next.choice.ch == ' ' &&
      next.choice.next != reader->root() &&
      !out_of_order(next)) {
    new_next.crumb = next.crumb;
    new_next.scale = next.scale * next.choice.count / reader->count() * restart;
    new_next.choice.ch = next.choice.ch;
    new_next.choice.count = reader->count();
    new_next.choice.next = reader->root();
    new_next.state = next.state;
    // The segment just finished becomes the floor for the next one.  A leaf
    // can't be named, so it orders nothing and the old floor stands.
    if (next.choice.next != LEAF) new_next.last_seg = next.choice.next;
    nexts.push(new_next);
  }

  return false;
}
