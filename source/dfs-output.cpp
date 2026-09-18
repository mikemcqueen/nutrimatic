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

std::string dfs_spelling_entry_list(
    DfsSpelling const& spelling, DfsSoloWords const* solo_words) {
  assert(spelling.solo_word_indexes.empty() ||
         spelling.solo_word_indexes.size() == spelling.segment_lengths.size());
  std::string list;
  list.reserve(spelling.text.size());
  size_t offset = 0;
  for (size_t s = 0; s < spelling.segment_lengths.size(); ++s) {
    if (s != 0) list.push_back(',');
    size_t const length = spelling.segment_lengths[s];
    list.append(spelling.text, offset, length);
    if (solo_words != NULL && !spelling.solo_word_indexes.empty() &&
        spelling.solo_word_indexes[s] != DFS_NO_SOLO_WORD) {
      list += " (";
      list += solo_words->word(spelling.solo_word_indexes[s]);
      list.push_back(')');
    }
    offset += length + 1;
  }
  return list;
}

static DfsPairBonusKind segment_pair_bonus_kind(uint8_t flags) {
  uint8_t const encoded =
      (flags & DFS_SEGMENT_PAIR_BONUS_MASK) >>
      DFS_SEGMENT_PAIR_BONUS_SHIFT;
  assert(encoded <= DFS_PAIR_BONUS_BEST);
  return DfsPairBonusKind(encoded);
}

static void set_segment_pair_bonus(
    uint8_t* flags, DfsPairBonusKind kind) {
  assert(flags != NULL);
  assert(kind >= DFS_PAIR_BONUS_NONE && kind <= DFS_PAIR_BONUS_BEST);
  if (kind <= segment_pair_bonus_kind(*flags)) return;
  *flags &= ~DFS_SEGMENT_PAIR_BONUS_MASK;
  *flags |= uint8_t(kind) << DFS_SEGMENT_PAIR_BONUS_SHIFT;
}

std::string dfs_spelling_bonus_list(DfsSpelling const& spelling) {
  assert(spelling.segment_bonus_flags.size() ==
         spelling.segment_lengths.size());
  std::string list;
  list.reserve(spelling.segment_bonus_flags.size() * 3);
  for (size_t i = 0; i < spelling.segment_bonus_flags.size(); ++i) {
    if (i != 0) list.push_back(',');
    uint8_t const flags = spelling.segment_bonus_flags[i];
    assert((flags & ~(DFS_SEGMENT_WORD_BONUS |
                      DFS_SEGMENT_PAIR_BONUS_MASK)) == 0);
    if ((flags & DFS_SEGMENT_WORD_BONUS) != 0) list.push_back('W');
    switch (segment_pair_bonus_kind(flags)) {
      case DFS_PAIR_BONUS_NONE:
        break;
      case DFS_PAIR_BONUS_LEGACY:
        list.push_back('P');
        break;
      case DFS_PAIR_BONUS_SEED:
        list.push_back('S');
        break;
      case DFS_PAIR_BONUS_YES:
        list.push_back('Y');
        break;
      case DFS_PAIR_BONUS_BEST:
        list.push_back('B');
        break;
      default:
        assert(false);
    }
    if (flags == 0) list.push_back('-');
  }
  return list;
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

bool dfs_index_members(
    DfsClassList const& classes, DfsMemberIndex* members,
    std::string* duplicate) {
  members->reserve(classes.entry_count());
  for (size_t ci = 0; ci < classes.classes().size(); ++ci) {
    for (size_t mi = 0; mi < classes.member_count(ci); ++mi) {
      DfsMemberView const member = classes.member(ci, mi);
      DfsMemberAddress const address = {ci, mi};
      std::string const text(member.text, member.text_length);
      if (!members->emplace(text, address).second) {
        if (duplicate != NULL) *duplicate = text;
        return false;
      }
    }
  }
  return true;
}

double dfs_representative_upper_log_score(
    DfsClassList const& classes, DfsScoreModel const& model,
    std::vector<size_t> const& class_indexes) {
  double representative = 0.0;
  for (size_t i = 0; i < class_indexes.size(); ++i) {
    DfsMemberView const member = classes.member(class_indexes[i], 0);
    double const score = model.member_upper_log_score(
        member.count, member.word_count > 1, member.score_flags);
    representative = i == 0
        ? score : model.append_log_score(representative, score);
  }
  return representative;
}

struct ExpansionCandidate {
  double upper_log_score;
  std::vector<size_t> member_indexes;
};

struct ExpansionOrder {
  bool operator()(ExpansionCandidate const& a,
                  ExpansionCandidate const& b) const {
    if (a.upper_log_score != b.upper_log_score)
      return a.upper_log_score < b.upper_log_score;
    return a.member_indexes > b.member_indexes;
  }
};

static double spelling_upper_log_score(
    DfsClassList const& classes,
    DfsScoreModel const& model,
    std::vector<size_t> const& class_indexes,
    std::vector<size_t> const& member_indexes,
    double representative_upper_log_score) {
  double score = representative_upper_log_score;
  for (size_t i = 0; i < class_indexes.size(); ++i) {
    if (member_indexes[i] == 0) continue;
    size_t const class_index = class_indexes[i];
    DfsMemberView const chosen =
        classes.member(class_index, member_indexes[i]);
    DfsMemberView const best = classes.member(class_index, 0);
    score += model.member_upper_log_score(
                 chosen.count, chosen.word_count > 1, chosen.score_flags) -
        model.member_upper_log_score(
                 best.count, best.word_count > 1, best.score_flags);
  }
  return score;
}

DfsSpelling dfs_build_spelling(
    DfsClassList const& classes, DfsScoreModel const& model,
    DfsSoloWords const* solo_words,
    std::vector<size_t> const& class_indexes,
    std::vector<size_t> const& member_indexes,
    double representative_upper_log_score,
    bool retain_segment_bonuses) {
  assert(class_indexes.size() == member_indexes.size());
  DfsSpelling spelling;
  spelling.segment_lengths.reserve(class_indexes.size());
  if (retain_segment_bonuses)
    spelling.segment_bonus_flags.reserve(class_indexes.size());
  std::vector<DfsSoloMasks> profiles;
  std::vector<size_t> profile_segments;
  std::vector<bool> profile_direct_best;
  size_t direct_best_segments = 0;
  if (solo_words != NULL) {
    profiles.reserve(class_indexes.size());
    profile_segments.reserve(class_indexes.size());
    profile_direct_best.reserve(class_indexes.size());
  }
  for (size_t i = 0; i < class_indexes.size(); ++i) {
    DfsMemberView const view =
        classes.member(class_indexes[i], member_indexes[i]);
    if (!spelling.text.empty()) spelling.text.push_back(' ');
    spelling.text.append(view.text, view.text_length);
    spelling.segment_lengths.push_back(uint8_t(view.text_length));
    DfsPairBonusKind const direct_pair_kind =
        dfs_member_pair_bonus_kind(view.score_flags);
    if (direct_pair_kind == DFS_PAIR_BONUS_BEST)
      ++direct_best_segments;
    if (retain_segment_bonuses) {
      uint8_t bonus_flags = 0;
      if (view.word_count > 1 && model.multi_word_log_bonus() != 0.0)
        bonus_flags |= DFS_SEGMENT_WORD_BONUS;
      if (model.pair_log_bonus(direct_pair_kind) != 0.0)
        set_segment_pair_bonus(&bonus_flags, direct_pair_kind);
      spelling.segment_bonus_flags.push_back(bonus_flags);
    }
    if (solo_words != NULL && view.word_count == 1 &&
        (view.score_flags & DFS_MEMBER_SOLO_WORD_EDGE) != 0) {
      DfsSoloMasks const profile = solo_words->lookup(
          std::string_view(view.text, view.text_length));
      uint16_t const solo_flags = view.score_flags &
          (DFS_MEMBER_SOLO_WORD_EDGE | DFS_MEMBER_SOLO_PAIR_EDGE |
           DFS_MEMBER_SOLO_PAIR_KIND_MASK);
      assert(dfs_solo_score_flags(profile) == solo_flags);
      (void) solo_flags;
      profiles.push_back(profile);
      profile_segments.push_back(i);
      profile_direct_best.push_back(
          direct_pair_kind == DFS_PAIR_BONUS_BEST);
    }
  }
  DfsExactResultMatching exact = dfs_exact_result_matching(
      profiles, profile_direct_best, direct_best_segments, model);
  std::vector<uint8_t>& profile_matches = exact.solo_word_indexes;
  spelling.log_score = spelling_upper_log_score(
      classes, model, class_indexes, member_indexes,
      representative_upper_log_score) + exact.correction;
  if (retain_segment_bonuses) {
    for (size_t i = 0; i < profile_matches.size(); ++i) {
      uint8_t const match = profile_matches[i];
      if (match == DFS_NO_SOLO_WORD) continue;
      uint8_t& bonus_flags =
          spelling.segment_bonus_flags[profile_segments[i]];
      if (model.multi_word_log_bonus() != 0.0)
        bonus_flags |= DFS_SEGMENT_WORD_BONUS;
      DfsPairBonusKind pair_kind = profiles[i].pair_kinds[match];
      if (pair_kind == DFS_PAIR_BONUS_NONE &&
          (profiles[i].pair_mask & (uint16_t(1) << match)) != 0)
        pair_kind = DFS_PAIR_BONUS_LEGACY;
      if (model.pair_log_bonus(pair_kind) != 0.0)
        set_segment_pair_bonus(&bonus_flags, pair_kind);
    }
  }
  if (std::find_if(profile_matches.begin(), profile_matches.end(),
                   [](uint8_t match) {
                     return match != DFS_NO_SOLO_WORD;
                   }) != profile_matches.end()) {
    spelling.solo_word_indexes.assign(
        spelling.segment_lengths.size(), DFS_NO_SOLO_WORD);
    for (size_t i = 0; i < profile_matches.size(); ++i)
      spelling.solo_word_indexes[profile_segments[i]] = profile_matches[i];
  }
  spelling.word_set_key = make_word_set_key(spelling.text);
  return spelling;
}

DfsTopN::DfsTopN(DfsClassList const* classes, DfsScoreModel const* model,
                 size_t limit, DfsSoloWords const* solo_words,
                 bool retain_segment_bonuses,
                 DfsRepeatPolicy const* repeats):
    class_list(classes),
    score_model(model),
    solo_words(solo_words),
    repeat_policy(
        repeats != NULL && !repeats->admits_everything() ? repeats : NULL),
    result_limit(limit),
    retain_segment_bonuses(retain_segment_bonuses),
    expanded(0),
    published_floor_bits(0),
    published_full(false),
    floor_announced(false) {
  assert(class_list != NULL);
  assert(score_model != NULL);
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

struct MemberWord {
  char const* text;
  size_t length;
};

static bool member_text_equals(
    DfsMemberView const& view, std::string const& value) {
  return value.size() == view.text_length &&
      memcmp(view.text, value.data(), value.size()) == 0;
}

static size_t count_word_in_member(
    DfsMemberView const& view, std::string const& word) {
  size_t found = 0;
  size_t start = 0;
  while (start <= view.text_length) {
    size_t end = start;
    while (end < view.text_length && view.text[end] != ' ') ++end;
    if (end - start == word.size() &&
        memcmp(view.text + start, word.data(), word.size()) == 0)
      ++found;
    start = end + 1;
  }
  return found;
}

bool DfsTopN::admits(std::vector<size_t> const& class_indexes,
                     std::vector<size_t> const& member_indexes) const {
  DfsRepeatPolicy const& policy = *repeat_policy;
  size_t const positions = class_indexes.size();

  if (policy.disable_repeats) {
    MemberWord seen[DFS_MAX_BAG_LETTERS];
    size_t count = 0;
    for (size_t i = 0; i < positions; ++i) {
      DfsMemberView const view =
          class_list->member(class_indexes[i], member_indexes[i]);
      size_t start = 0;
      while (start < view.text_length) {
        size_t end = start;
        while (end < view.text_length && view.text[end] != ' ') ++end;
        size_t const length = end - start;
        for (size_t w = 0; w < count; ++w)
          if (seen[w].length == length &&
              memcmp(seen[w].text, view.text + start, length) == 0)
            return false;
        assert(count < DFS_MAX_BAG_LETTERS);
        seen[count].text = view.text + start;
        seen[count].length = length;
        ++count;
        start = end + 1;
      }
    }
  }

  if (!policy.pairs.empty())
    for (size_t i = 1; i < positions; ++i) {
      if (class_indexes[i] != class_indexes[i - 1] ||
          member_indexes[i] != member_indexes[i - 1])
        continue;
      DfsMemberView const view =
          class_list->member(class_indexes[i], member_indexes[i]);
      for (size_t p = 0; p < policy.pairs.size(); ++p)
        if (member_text_equals(view, policy.pairs[p])) return false;
    }

  for (size_t w = 0; w < policy.words.size(); ++w) {
    size_t seen = 0;
    for (size_t i = 0; i < positions; ++i) {
      seen += count_word_in_member(
          class_list->member(class_indexes[i], member_indexes[i]),
          policy.words[w]);
      if (seen > 1) return false;
    }
  }
  return true;
}

void DfsTopN::emit(std::vector<size_t> const& class_indexes,
                   double representative_upper_log_score) {
  if (class_indexes.empty()) return;
  double published;
  if (score_floor(&published) &&
      representative_upper_log_score <= published)
    return;

  ExpansionCandidate first;
  first.upper_log_score = representative_upper_log_score;
  first.member_indexes.assign(class_indexes.size(), 0);

  std::priority_queue<ExpansionCandidate,
                      std::vector<ExpansionCandidate>,
                      ExpansionOrder> pending;
  pending.push(first);

  while (!pending.empty()) {
    ExpansionCandidate const current = pending.top();
    if (score_floor(&published) && current.upper_log_score <= published) break;
    pending.pop();

    // A rejected candidate is skipped, not pruned: its successors still go on
    // the queue, since a repeat at this member can vanish at the next one.
    if (repeat_policy == NULL ||
        admits(class_indexes, current.member_indexes)) {
      DfsSpelling spelling = dfs_build_spelling(
          *class_list, *score_model, solo_words, class_indexes,
          current.member_indexes, representative_upper_log_score,
          retain_segment_bonuses);
      assert(spelling.log_score <= current.upper_log_score);
      std::lock_guard<std::mutex> const guard(heap_mutex);
      // The published floor may have strengthened while this spelling was
      // constructed. Since pending is score ordered and descendants cannot
      // improve on their parent, an authoritative cutoff ends this expansion.
      if (result_limit != 0 && heap.size() == result_limit &&
          current.upper_log_score <= floor_log_score())
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
          next.upper_log_score = spelling_upper_log_score(
              *class_list, *score_model, class_indexes,
              next.member_indexes, representative_upper_log_score);
          if (!score_floor(&published) || next.upper_log_score > published)
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
    found->second.segment_lengths = std::move(spelling.segment_lengths);
    found->second.solo_word_indexes =
        std::move(spelling.solo_word_indexes);
    found->second.segment_bonus_flags =
        std::move(spelling.segment_bonus_flags);
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
    value.segment_lengths = std::move(spelling.segment_lengths);
    value.solo_word_indexes = std::move(spelling.solo_word_indexes);
    value.segment_bonus_flags = std::move(spelling.segment_bonus_flags);
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
  node.mapped().segment_lengths = std::move(spelling.segment_lengths);
  node.mapped().solo_word_indexes = std::move(spelling.solo_word_indexes);
  node.mapped().segment_bonus_flags =
      std::move(spelling.segment_bonus_flags);
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
      spelling.segment_lengths = std::move(entry->second.segment_lengths);
      spelling.solo_word_indexes =
          std::move(entry->second.solo_word_indexes);
      spelling.segment_bonus_flags =
          std::move(entry->second.segment_bonus_flags);
      spelling.word_set_key = entry->first;
      results.push_back(std::move(spelling));
    }
    std::vector<HeapSlot>().swap(heap);
    RetainedMap().swap(retained);
  }
  std::sort(results.begin(), results.end(), dfs_spelling_better);
  return results;
}

bool dfs_spelling_better(DfsSpelling const& a, DfsSpelling const& b) {
  if (a.log_score != b.log_score) return a.log_score > b.log_score;
  if (a.word_set_key != b.word_set_key)
    return a.word_set_key < b.word_set_key;
  return a.text < b.text;
}

bool dfs_print_results(
    FILE* output, std::vector<DfsSpelling> const& results,
    bool show_score, bool show_bonus, DfsSoloWords const* solo_words) {
  for (size_t i = 0; i < results.size(); ++i) {
    if (show_score &&
        fprintf(output, "%#.7g ", exp(results[i].log_score)) < 0)
      return false;
    if (show_bonus &&
        fprintf(output, "%s ", dfs_spelling_bonus_list(results[i]).c_str()) < 0)
      return false;
    if (fprintf(output, "%s\n",
                dfs_spelling_entry_list(results[i], solo_words).c_str()) < 0)
      return false;
  }
  return fflush(output) == 0;
}
