#include "dfs-output.h"

#include <assert.h>
#include <math.h>
#include <string.h>

#include <algorithm>
#include <limits>
#include <queue>
#include <utility>

#include "dfs-diagnostic.h"

static bool weaker(HeapSlot const& a, HeapSlot const& b) {
  if (a.log_score != b.log_score) return a.log_score < b.log_score;
  RetainedEntry const& ea = *a.retained;
  RetainedEntry const& eb = *b.retained;
  if (ea.first != eb.first) return ea.first > eb.first;
  return ea.second.text > eb.second.text;
}

static std::string make_word_set_key(std::string const& text) {
  std::vector<std::string> words;
  for (size_t i = 0; i < text.size(); ) {
    size_t const end = text.find(' ', i);
    if (end != i) words.push_back(text.substr(i, end - i));
    if (end == std::string::npos) break;
    i = end + 1;
  }
  std::sort(words.begin(), words.end());

  std::string key;
  for (size_t i = 0; i < words.size(); ++i) {
    if (i != 0) key.push_back(' ');
    key += words[i];
  }
  return key;
}

struct ExpansionCandidate {
  double log_score;
  std::vector<size_t> member_indexes;
};

struct ExpansionOrder {
  bool operator()(ExpansionCandidate const& a,
                  ExpansionCandidate const& b) const {
    if (a.log_score != b.log_score) return a.log_score < b.log_score;
    return a.member_indexes > b.member_indexes;
  }
};

static double spelling_log_score(
    DfsClassList const& classes,
    std::vector<size_t> const& class_indexes,
    std::vector<size_t> const& member_indexes,
    double representative_log_score) {
  double score = representative_log_score;
  for (size_t i = 0; i < class_indexes.size(); ++i) {
    if (member_indexes[i] == 0) continue;
    size_t const class_index = class_indexes[i];
    score +=
        log(double(classes.member(class_index, member_indexes[i]).count)) -
        log(double(classes.member(class_index, 0).count));
  }
  return score;
}

DfsTopN::DfsTopN(DfsClassList const* classes, size_t limit):
    class_list(classes),
    result_limit(limit),
    expanded(0),
    published_floor_bits(0),
    published_full(false),
    floor_announced(false) {
  assert(class_list != NULL);
  // Expansion indexes members by (class_index, member_index) throughout, so a
  // list whose grouping has been dropped cannot be expanded at all.
  assert(!class_list->members_invalidated());
  static_assert(sizeof(double) == sizeof(uint64_t),
                "published score floors must remain eight bytes");
  heap.reserve(result_limit);
  retained.reserve(result_limit);
}

void DfsTopN::publish_floor() {
  if (result_limit == 0 || heap.size() != result_limit) return;
  double const floor = heap[0].log_score;
  if (!floor_announced) {
    floor_announced = true;
    dfs_diagnostic(
        "phase 3: top-%zu queue filled after %zu spellings expanded, "
        "floor %.6f\n",
        result_limit, expanded, floor);
  }
  uint64_t bits;
  memcpy(&bits, &floor, sizeof(bits));
  published_floor_bits.store(bits, std::memory_order_relaxed);
  published_full.store(true, std::memory_order_release);
}

bool DfsTopN::score_floor(double* floor) const {
  if (result_limit == 0 ||
      !published_full.load(std::memory_order_acquire))
    return false;
  uint64_t const bits =
      published_floor_bits.load(std::memory_order_relaxed);
  memcpy(floor, &bits, sizeof(bits));
  return true;
}

void DfsTopN::emit(std::vector<size_t> const& class_indexes,
                   double representative_log_score) {
  if (class_indexes.empty()) return;
  double published;
  if (score_floor(&published) &&
      representative_log_score <= published)
    return;

  ExpansionCandidate first;
  first.log_score = representative_log_score;
  first.member_indexes.assign(class_indexes.size(), 0);

  std::priority_queue<ExpansionCandidate,
                      std::vector<ExpansionCandidate>,
                      ExpansionOrder> pending;
  pending.push(first);

  while (!pending.empty()) {
    ExpansionCandidate const current = pending.top();
    if (score_floor(&published) && current.log_score <= published) break;
    pending.pop();

    DfsSpelling spelling;
    spelling.log_score = current.log_score;
    for (size_t i = 0; i < class_indexes.size(); ++i) {
      DfsMemberView const view = class_list->member(
          class_indexes[i], current.member_indexes[i]);
      if (!spelling.text.empty()) spelling.text.push_back(' ');
      spelling.text.append(view.text, view.text_length);
    }
    spelling.word_set_key = make_word_set_key(spelling.text);
    {
      std::lock_guard<std::mutex> const guard(heap_mutex);
      // The published floor may have strengthened while this spelling was
      // constructed. Since pending is score ordered and descendants cannot
      // improve on their parent, an authoritative cutoff ends this expansion.
      if (result_limit != 0 && heap.size() == result_limit &&
          current.log_score <= floor_log_score())
        break;
      ++expanded;
      if (offer(std::move(spelling))) publish_floor();
    }

    // Every tuple has one canonical parent: decrement its first nonzero
    // dimension. Inverting that relation generates the Cartesian product
    // without a per-solution visited set.
    bool earlier_indexes_are_zero = true;
    for (size_t dimension = 0;
         dimension < class_indexes.size() && earlier_indexes_are_zero;
         ++dimension) {
      size_t const class_index = class_indexes[dimension];
      size_t const old_member = current.member_indexes[dimension];
      size_t const new_member = old_member + 1;
      if (new_member < class_list->member_count(class_index)) {
        ExpansionCandidate next = current;
        next.member_indexes[dimension] = new_member;

        // Repeated occurrences of one class are interchangeable. Keep only
        // nondecreasing member indexes so each member multiset is expanded
        // once instead of once per permutation.
        bool canonical = true;
        for (size_t later = dimension + 1;
             later < class_indexes.size(); ++later)
          if (class_indexes[later] == class_index &&
              next.member_indexes[dimension] >
                  next.member_indexes[later]) {
            canonical = false;
            break;
          }
        if (canonical) {
          next.log_score =
              spelling_log_score(*class_list, class_indexes,
                                 next.member_indexes,
                                 representative_log_score);
          if (!score_floor(&published) || next.log_score > published)
            pending.push(std::move(next));
        }
      }
      earlier_indexes_are_zero = old_member == 0;
    }
  }
}

double DfsTopN::floor_log_score() const {
  if (result_limit == 0 || heap.size() != result_limit)
    return -std::numeric_limits<double>::infinity();
  return heap[0].log_score;
}

bool DfsTopN::offer(DfsSpelling spelling) {
  RetainedMap::iterator found = retained.find(spelling.word_set_key);
  if (found != retained.end()) {
    if (found->second.log_score >= spelling.log_score) return false;
    found->second.text = std::move(spelling.text);
    found->second.log_score = spelling.log_score;
    if (result_limit != 0) {
      size_t const position = found->second.heap_pos;
      heap[position].log_score = spelling.log_score;
      sift_down(position);
    }
    return true;
  }

  if (result_limit == 0 || heap.size() < result_limit) {
    size_t const position = heap.size();
    RetainedSpelling value;
    value.text = std::move(spelling.text);
    value.log_score = spelling.log_score;
    value.heap_pos = position;
    std::pair<RetainedMap::iterator, bool> const inserted =
        retained.emplace(std::move(spelling.word_set_key), std::move(value));
    if (result_limit == 0) return true;
    HeapSlot slot;
    slot.log_score = spelling.log_score;
    slot.retained = &*inserted.first;
    heap.push_back(slot);
    sift_up(position);
    return true;
  }

  if (spelling.log_score <= heap[0].log_score) return false;

  // The incoming key is known absent (the find() above happened under the
  // same lock), so recycle the evicted node instead of freeing and
  // reallocating: extract it, overwrite its key and value, and reinsert.
  RetainedEntry* const evicted = heap[0].retained;
  RetainedMap::node_type node = retained.extract(evicted->first);
  node.key() = std::move(spelling.word_set_key);
  node.mapped().text = std::move(spelling.text);
  node.mapped().log_score = spelling.log_score;
  node.mapped().heap_pos = 0;
  RetainedMap::insert_return_type const reinserted =
      retained.insert(std::move(node));
  heap[0].log_score = spelling.log_score;
  heap[0].retained = &*reinserted.position;
  sift_down(0);
  return true;
}

void DfsTopN::swap_heap_entries(size_t a, size_t b) {
  std::swap(heap[a], heap[b]);
  heap[a].retained->second.heap_pos = a;
  heap[b].retained->second.heap_pos = b;
}

void DfsTopN::sift_up(size_t position) {
  while (position > 0) {
    size_t const parent = (position - 1) / 2;
    if (!weaker(heap[position], heap[parent])) break;
    swap_heap_entries(position, parent);
    position = parent;
  }
}

void DfsTopN::sift_down(size_t position) {
  for (;;) {
    size_t weakest = position;
    size_t const left = position * 2 + 1;
    size_t const right = left + 1;
    if (left < heap.size() && weaker(heap[left], heap[weakest]))
      weakest = left;
    if (right < heap.size() && weaker(heap[right], heap[weakest]))
      weakest = right;
    if (weakest == position) break;
    swap_heap_entries(position, weakest);
    position = weakest;
  }
}

std::vector<DfsSpelling> DfsTopN::take_sorted_results() {
  std::vector<DfsSpelling> results;
  {
    // Draining is a post-search operation. It is not permitted concurrently
    // with emit() or score_floor(), but uses the same mutex so sink reuse
    // resets publication in one coherent state transition.
    std::lock_guard<std::mutex> const guard(heap_mutex);
    published_full.store(false, std::memory_order_release);
    floor_announced = false;
    results.reserve(retained.size());
    for (RetainedMap::iterator entry = retained.begin();
         entry != retained.end(); ++entry) {
      DfsSpelling spelling;
      spelling.log_score = entry->second.log_score;
      spelling.text = std::move(entry->second.text);
      spelling.word_set_key = entry->first;
      results.push_back(std::move(spelling));
    }
    std::vector<HeapSlot>().swap(heap);
    RetainedMap().swap(retained);
  }
  std::sort(results.begin(), results.end(),
            [](DfsSpelling const& a, DfsSpelling const& b) {
    if (a.log_score != b.log_score) return a.log_score > b.log_score;
    if (a.word_set_key != b.word_set_key)
      return a.word_set_key < b.word_set_key;
    return a.text < b.text;
  });
  return results;
}
