#include "dfs-output.h"

#include <assert.h>
#include <math.h>
#include <string.h>

#include <algorithm>
#include <limits>
#include <queue>
#include <utility>

static bool weaker(DfsSpelling const& a, DfsSpelling const& b) {
  if (a.log_score != b.log_score) return a.log_score < b.log_score;
  if (a.word_set_key != b.word_set_key)
    return a.word_set_key > b.word_set_key;
  return a.text > b.text;
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
    std::vector<DfsAnagramClass> const& classes,
    std::vector<size_t> const& class_indexes,
    std::vector<size_t> const& member_indexes,
    double representative_log_score) {
  double score = representative_log_score;
  for (size_t i = 0; i < class_indexes.size(); ++i) {
    if (member_indexes[i] == 0) continue;
    DfsAnagramClass const& anagram_class = classes[class_indexes[i]];
    score += log(double(anagram_class.members[member_indexes[i]].count)) -
             log(double(anagram_class.members[0].count));
  }
  return score;
}

DfsTopN::DfsTopN(DfsClassList const* classes, size_t limit):
    class_list(classes),
    result_limit(limit),
    expanded(0),
    published_floor_bits(0),
    published_full(false) {
  assert(class_list != NULL);
  static_assert(sizeof(double) == sizeof(uint64_t),
                "published score floors must remain eight bytes");
  heap.reserve(result_limit);
  positions.reserve(result_limit);
}

void DfsTopN::publish_floor() {
  if (heap.size() != result_limit) return;
  double const floor = heap[0].log_score;
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
  if (result_limit == 0 || class_indexes.empty()) return;
  double published;
  if (score_floor(&published) &&
      representative_log_score <= published)
    return;

  std::vector<DfsAnagramClass> const& classes = class_list->classes();
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
      DfsAnagramClass const& anagram_class =
          classes[class_indexes[i]];
      assert(current.member_indexes[i] < anagram_class.members.size());
      if (!spelling.text.empty()) spelling.text.push_back(' ');
      spelling.text +=
          anagram_class.members[current.member_indexes[i]].text;
    }
    spelling.word_set_key = make_word_set_key(spelling.text);
    {
      std::lock_guard<std::mutex> const guard(heap_mutex);
      // The published floor may have strengthened while this spelling was
      // constructed. Since pending is score ordered and descendants cannot
      // improve on their parent, an authoritative cutoff ends this expansion.
      if (heap.size() == result_limit &&
          current.log_score <= floor_log_score())
        break;
      ++expanded;
      if (offer(spelling)) publish_floor();
    }

    // Every tuple has one canonical parent: decrement its first nonzero
    // dimension. Inverting that relation generates the Cartesian product
    // without a per-solution visited set.
    bool earlier_indexes_are_zero = true;
    for (size_t dimension = 0;
         dimension < class_indexes.size() && earlier_indexes_are_zero;
         ++dimension) {
      size_t const class_index = class_indexes[dimension];
      DfsAnagramClass const& anagram_class = classes[class_index];
      size_t const old_member = current.member_indexes[dimension];
      size_t const new_member = old_member + 1;
      if (new_member < anagram_class.members.size()) {
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
              spelling_log_score(classes, class_indexes, next.member_indexes,
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
  if (heap.size() != result_limit)
    return -std::numeric_limits<double>::infinity();
  return heap[0].log_score;
}

bool DfsTopN::offer(DfsSpelling const& spelling) {
  std::unordered_map<std::string, size_t>::iterator found =
      positions.find(spelling.word_set_key);
  if (found != positions.end()) {
    size_t const position = found->second;
    if (heap[position].log_score >= spelling.log_score) return false;
    heap[position] = spelling;
    sift_down(position);
    return true;
  }

  if (heap.size() < result_limit) {
    size_t const position = heap.size();
    heap.push_back(spelling);
    positions[spelling.word_set_key] = position;
    sift_up(position);
    return true;
  }

  if (spelling.log_score <= heap[0].log_score) return false;
  positions.erase(heap[0].word_set_key);
  heap[0] = spelling;
  positions[spelling.word_set_key] = 0;
  sift_down(0);
  return true;
}

void DfsTopN::swap_heap_entries(size_t a, size_t b) {
  std::swap(heap[a], heap[b]);
  positions[heap[a].word_set_key] = a;
  positions[heap[b].word_set_key] = b;
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
    results.swap(heap);
    std::unordered_map<std::string, size_t>().swap(positions);
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
