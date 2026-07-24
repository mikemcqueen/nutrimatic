#include "dfs-search.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <new>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#if defined(__GNUC__) || defined(__clang__)
#define DFS_LIKELY(value) __builtin_expect(!!(value), 1)
#define DFS_UNLIKELY(value) __builtin_expect(!!(value), 0)
#else
#define DFS_LIKELY(value) (value)
#define DFS_UNLIKELY(value) (value)
#endif

namespace {

uint64_t const CACHE_UNSEEN = UINT64_MAX;
uint64_t const CACHE_BYPASSED = UINT64_MAX - 1;
size_t const CACHE_ALIGNMENT = 64;

static double make_restart_log_rate(double restart, int64_t corpus_total) {
  assert(restart > 0.0);
  assert(corpus_total > 0);
  return log(restart) - log(double(corpus_total));
}

static size_t derived_max_depth(DfsClassList const* classes,
                                size_t letter_count) {
  assert(classes != NULL);
  return letter_count / size_t(classes->min_word_length());
}

static bool checked_multiply_u64(uint64_t a, uint64_t b, uint64_t* out) {
  if (a != 0 && b > UINT64_MAX / a) return false;
  *out = a * b;
  return true;
}

static bool checked_add_u64(uint64_t a, uint64_t b, uint64_t* out) {
  if (b > UINT64_MAX - a) return false;
  *out = a + b;
  return true;
}

static bool round_up_alignment(size_t bytes, size_t* rounded) {
  if (bytes > SIZE_MAX - (CACHE_ALIGNMENT - 1)) return false;
  *rounded = (bytes + CACHE_ALIGNMENT - 1) &
      ~(CACHE_ALIGNMENT - 1);
  return true;
}

static void* allocate_aligned(size_t bytes) {
  if (bytes == 0) return NULL;
  size_t rounded;
  if (!round_up_alignment(bytes, &rounded)) return NULL;
  return aligned_alloc(CACHE_ALIGNMENT, rounded);
}

static uint64_t mix_key(uint64_t value) {
  value ^= value >> 30;
  value *= UINT64_C(0xbf58476d1ce4e5b9);
  value ^= value >> 27;
  value *= UINT64_C(0x94d049bb133111eb);
  value ^= value >> 31;
  return value;
}

static size_t largest_power_of_two(size_t value) {
  if (value == 0) return 0;
  size_t result = 1;
  while (result <= value / 2) result *= 2;
  return result;
}

static size_t next_power_of_two_at_least(size_t value) {
  size_t result = 1;
  while (result < value && result <= SIZE_MAX / 2) result *= 2;
  return result < value ? 0 : result;
}

static uint64_t pack_entry(uint32_t offset, uint32_t count) {
  return (uint64_t(count) << 32) | uint64_t(offset);
}

static uint32_t entry_offset(uint64_t metadata) {
  return uint32_t(metadata);
}

static uint32_t entry_count(uint64_t metadata) {
  return uint32_t(metadata >> 32);
}

static uint32_t packed_rank(uint32_t requirement) {
  return requirement & 63U;
}

static uint32_t packed_count(uint32_t requirement) {
  return requirement >> 6;
}

static uint32_t hot_letter_length(uint32_t packed) {
  return packed & 0xffffU;
}

static uint32_t hot_requirement_count(uint32_t packed) {
  return (packed >> 16) & 0xffU;
}

static uint32_t hot_repeated_count(uint32_t packed) {
  return packed >> 24;
}

}  // namespace

void DfsAnagramSearch::AlignedFree::operator()(void* pointer) const {
  free(pointer);
}

DfsAnagramSearch::DfsAnagramSearch(DfsClassList const* classes,
                                   std::string const& letters,
                                   double restart, int64_t corpus_total,
                                   size_t candidate_cache_bytes):
    class_list(classes),
    letters(letters),
    restart_log_rate(make_restart_log_rate(restart, corpus_total)),
    max_depth(derived_max_depth(classes, letters.size())),
    candidate_cache_budget(candidate_cache_bytes),
    bag_mask(0),
    current_bag_key(0),
    hot_classes_ready(false),
    cache_mode(CANDIDATE_CACHE_OFF),
    cache_capacity(0),
    sparse_max_entries(0),
    sparse_filled(0),
    candidate_capacity(0),
    candidate_used(0),
    admitted_entries(0),
    charged_bytes(0),
    progress_stream(NULL),
    progress_interval(0),
    next_progress(0),
    nodes(0),
    solutions(0) {
  assert(class_list != NULL);

  std::vector<DfsAnagramClass> const& all_classes = class_list->classes();
  best_member_log_scores.reserve(all_classes.size());
  for (size_t i = 0; i < all_classes.size(); ++i) {
    assert(!all_classes[i].members.empty());
    assert(all_classes[i].members[0].count > 0);
    best_member_log_scores.push_back(
        log(double(all_classes[i].members[0].count)));
  }
}

bool DfsAnagramSearch::prepare_hot_classes() {
  static_assert(sizeof(HotClass) == 32,
                "HotClass must remain two per cache line");
  hot_classes.reset();
  packed_letters.reset();

  std::vector<DfsAnagramClass> const& classes = class_list->classes();
  if (classes.empty() || classes.size() > UINT32_MAX ||
      classes.size() > SIZE_MAX / sizeof(HotClass) ||
      letters.size() > UINT16_MAX)
    return false;

  size_t requirements = 0;
  for (size_t ci = 0; ci < classes.size(); ++ci) {
    if (classes[ci].letters.size() > UINT8_MAX ||
        requirements > UINT32_MAX - classes[ci].letters.size())
      return false;
    requirements += classes[ci].letters.size();
  }
  if (requirements > SIZE_MAX / sizeof(uint32_t)) return false;

  HotClass* hot = static_cast<HotClass*>(
      allocate_aligned(classes.size() * sizeof(HotClass)));
  if (hot == NULL) return false;
  std::unique_ptr<HotClass, AlignedFree> new_hot(hot);

  uint32_t* packed = NULL;
  if (requirements != 0) {
    packed = static_cast<uint32_t*>(
        allocate_aligned(requirements * sizeof(uint32_t)));
    if (packed == NULL) return false;
  }
  std::unique_ptr<uint32_t, AlignedFree> new_packed(packed);

  size_t offset = 0;
  std::array<int, DFS_SYMBOL_COUNT> const& symbol_to_rank =
      class_list->symbol_to_rank();
  for (size_t ci = 0; ci < classes.size(); ++ci) {
    DfsAnagramClass const& source = classes[ci];
    uint64_t delta = 0;
    uint64_t support = 0;
    uint32_t repeated = 0;

    for (size_t i = 0; i < source.letters.size(); ++i) {
      uint32_t const count = source.letters[i].second;
      if (count > (UINT32_MAX >> 6)) return false;
      if (count > 1) ++repeated;
    }

    size_t write = offset;
    for (int repeated_pass = 1; repeated_pass >= 0; --repeated_pass) {
      for (size_t i = 0; i < source.letters.size(); ++i) {
        uint32_t const count = source.letters[i].second;
        if ((count > 1) != (repeated_pass != 0)) continue;
        uint32_t const rank = uint32_t(symbol_to_rank[
            size_t(source.letters[i].first)]);
        packed[write++] = (count << 6) | rank;
      }
    }
    assert(write == offset + source.letters.size());

    for (size_t i = 0; i < source.letters.size(); ++i) {
      uint32_t const count = source.letters[i].second;
      uint32_t const rank = uint32_t(symbol_to_rank[
          size_t(source.letters[i].first)]);
      support |= UINT64_C(1) << rank;
      uint64_t term;
      uint64_t next;
      if (!checked_multiply_u64(count, multipliers[rank], &term) ||
          !checked_add_u64(delta, term, &next))
        return false;
      delta = next;
    }

    hot[ci].best_member_log_score = best_member_log_scores[ci];
    hot[ci].bag_key_delta = delta;
    hot[ci].support_mask = support;
    hot[ci].letters_offset = uint32_t(offset);
    hot[ci].packed_length_and_count =
        uint32_t(source.key.size()) |
        (uint32_t(source.letters.size()) << 16) |
        (repeated << 24);
    offset = write;
  }

  hot_classes = std::move(new_hot);
  packed_letters = std::move(new_packed);
  return true;
}

void DfsAnagramSearch::clear_cache() {
  cache_mode = CANDIDATE_CACHE_OFF;
  cache_metadata.reset();
  sparse_keys.reset();
  candidate_ids.reset();
  cache_capacity = 0;
  sparse_max_entries = 0;
  sparse_filled = 0;
  candidate_capacity = 0;
  candidate_used = 0;
  admitted_entries = 0;
  charged_bytes = 0;
  candidate_build_buffer.clear();
}

void DfsAnagramSearch::prepare_cache(uint64_t state_count) {
  clear_cache();
  if (!hot_classes_ready || candidate_cache_budget < CACHE_ALIGNMENT)
    return;

  size_t dense_bytes = 0;
  bool const dense_size_ok =
      state_count <= SIZE_MAX / sizeof(uint64_t) &&
      round_up_alignment(size_t(state_count) * sizeof(uint64_t),
                         &dense_bytes);
  if (dense_size_ok && dense_bytes <= candidate_cache_budget / 2) {
    uint64_t* metadata = static_cast<uint64_t*>(
        allocate_aligned(dense_bytes));
    if (metadata == NULL) return;
    std::fill(metadata, metadata + size_t(state_count), CACHE_UNSEEN);
    cache_metadata.reset(metadata);
    cache_capacity = size_t(state_count);
    cache_mode = CANDIDATE_CACHE_DENSE;
    charged_bytes = dense_bytes;
  } else {
    size_t const metadata_share = candidate_cache_budget / 2;
    size_t capacity = largest_power_of_two(
        metadata_share / (2 * sizeof(uint64_t)));
    if (state_count <= SIZE_MAX / 2) {
      size_t const desired =
          next_power_of_two_at_least(size_t(state_count) * 2);
      if (desired != 0) capacity = std::min(capacity, desired);
    }
    if (capacity < 2) return;

    size_t array_bytes;
    if (!round_up_alignment(capacity * sizeof(uint64_t), &array_bytes) ||
        array_bytes > candidate_cache_budget / 2)
      return;
    uint64_t* keys = static_cast<uint64_t*>(
        allocate_aligned(array_bytes));
    if (keys == NULL) return;
    std::unique_ptr<uint64_t, AlignedFree> new_keys(keys);
    uint64_t* metadata = static_cast<uint64_t*>(
        allocate_aligned(array_bytes));
    if (metadata == NULL) return;
    std::fill(keys, keys + capacity, UINT64_MAX);

    sparse_keys = std::move(new_keys);
    cache_metadata.reset(metadata);
    cache_capacity = capacity;
    sparse_max_entries = capacity / 2;
    cache_mode = CANDIDATE_CACHE_SPARSE;
    charged_bytes = array_bytes * 2;
  }

  size_t remaining = candidate_cache_budget - charged_bytes;
  size_t arena_bytes = remaining & ~(CACHE_ALIGNMENT - 1);
  size_t const max_candidate_ids = std::min(
      size_t(UINT32_MAX - 1), SIZE_MAX / sizeof(uint32_t));
  size_t const max_arena_bytes =
      max_candidate_ids * sizeof(uint32_t);
  arena_bytes = std::min(arena_bytes, max_arena_bytes);
  if (arena_bytes != 0) {
    uint32_t* ids = static_cast<uint32_t*>(
        allocate_aligned(arena_bytes));
    if (ids == NULL) {
      clear_cache();
      return;
    }
    candidate_ids.reset(ids);
    candidate_capacity = arena_bytes / sizeof(uint32_t);
  }

  size_t largest_bucket = 0;
  std::array<int, DFS_SYMBOL_COUNT> const& rank_to_symbol =
      class_list->rank_to_symbol();
  for (int rank = 0; rank < DFS_SYMBOL_COUNT; ++rank) {
    int const symbol = rank_to_symbol[size_t(rank)];
    largest_bucket = std::max(
        largest_bucket,
        class_list->candidate_end(symbol) -
            class_list->candidate_begin(symbol));
  }
  try {
    candidate_build_buffer.reserve(
        std::min(largest_bucket, candidate_capacity));
  } catch (std::bad_alloc const&) {
    clear_cache();
  }
}

void DfsAnagramSearch::run(DfsSolutionSink* sink, FILE* progress,
                           int progress_factor) {
  bag.fill(0);
  std::array<int, DFS_SYMBOL_COUNT> const& symbol_to_rank =
      class_list->symbol_to_rank();
  bool encodable = true;
  for (size_t i = 0; i < letters.size(); ++i) {
    int const symbol = dfs_symbol_index((unsigned char) letters[i]);
    assert(symbol >= 0);
    uint32_t& count = bag[size_t(symbol_to_rank[size_t(symbol)])];
    if (count == UINT32_MAX) encodable = false;
    ++count;
  }

  uint64_t state_count = 1;
  if (encodable) {
    for (int rank = 0; rank < DFS_SYMBOL_COUNT; ++rank) {
      multipliers[size_t(rank)] = state_count;
      uint64_t const radix = uint64_t(bag[size_t(rank)]) + 1;
      if (!checked_multiply_u64(state_count, radix, &state_count)) {
        encodable = false;
        break;
      }
    }
  }

  bag_mask = 0;
  for (int rank = 0; rank < DFS_SYMBOL_COUNT; ++rank)
    if (bag[size_t(rank)] != 0)
      bag_mask |= UINT64_C(1) << rank;
  current_bag_key = encodable ? state_count - 1 : 0;
  hot_classes_ready = encodable && prepare_hot_classes();
  prepare_cache(state_count);

  path.clear();
  path.reserve(letters.size());
  progress_stream = progress;
  progress_interval =
      int64_t(100000) * int64_t(std::max(progress_factor, 1));
  next_progress = progress_interval;
  nodes = 0;
  solutions = 0;

  if (!hot_classes_ready) {
    walk_unoptimized(letters.size(), 0, 0, 0.0, sink);
  } else if (cache_mode == CANDIDATE_CACHE_DENSE) {
    walk<WALK_DENSE>(letters.size(), 0, 0.0, sink);
  } else if (cache_mode == CANDIDATE_CACHE_SPARSE) {
    walk<WALK_SPARSE>(letters.size(), 0, 0.0, sink);
  } else {
    walk<WALK_UNCACHED>(letters.size(), 0, 0.0, sink);
  }
}

bool DfsAnagramSearch::hot_class_fits(uint32_t class_index) const {
  HotClass const& candidate = hot_classes.get()[class_index];
  if ((candidate.support_mask & ~bag_mask) != 0) return false;
  uint32_t const* requirements =
      packed_letters.get() + candidate.letters_offset;
  uint32_t const repeated =
      hot_repeated_count(candidate.packed_length_and_count);
  for (uint32_t i = 0; i < repeated; ++i) {
    uint32_t const requirement = requirements[i];
    if (bag[packed_rank(requirement)] < packed_count(requirement))
      return false;
  }
  return true;
}

bool DfsAnagramSearch::build_candidate_entry(
    size_t begin, size_t end, uint64_t* metadata) {
  candidate_build_buffer.clear();
  size_t const available = candidate_capacity - candidate_used;
  for (size_t class_index = begin; class_index < end; ++class_index) {
    uint32_t const id = uint32_t(class_index);
    if (!hot_class_fits(id)) continue;
    if (candidate_build_buffer.size() == available) {
      *metadata = CACHE_BYPASSED;
      return false;
    }
    candidate_build_buffer.push_back(id);
  }

  uint32_t const offset = uint32_t(candidate_used);
  uint32_t const count = uint32_t(candidate_build_buffer.size());
  if (count != 0) {
    memcpy(candidate_ids.get() + candidate_used,
           candidate_build_buffer.data(),
           size_t(count) * sizeof(uint32_t));
  }
  candidate_used += count;
  charged_bytes += size_t(count) * sizeof(uint32_t);
  ++admitted_entries;
  *metadata = pack_entry(offset, count);
  return true;
}

void DfsAnagramSearch::publish_dense(uint64_t key, uint64_t metadata) {
  assert(key < cache_capacity);
  cache_metadata.get()[size_t(key)] = metadata;
}

void DfsAnagramSearch::publish_sparse(
    size_t slot, uint64_t key, uint64_t metadata) {
  cache_metadata.get()[slot] = metadata;
  sparse_keys.get()[slot] = key;
  ++sparse_filled;
}

uint64_t DfsAnagramSearch::sparse_lookup(
    uint64_t key, size_t* slot, bool* may_insert) const {
  size_t const mask = cache_capacity - 1;
  size_t position = size_t(mix_key(key)) & mask;
  for (;;) {
    uint64_t const stored_key = sparse_keys.get()[position];
    if (stored_key == key) {
      *slot = position;
      *may_insert = false;
      return cache_metadata.get()[position];
    }
    if (stored_key == UINT64_MAX) {
      *slot = position;
      *may_insert = sparse_filled < sparse_max_entries;
      return CACHE_UNSEEN;
    }
    position = (position + 1) & mask;
  }
}

template<DfsAnagramSearch::WalkMode mode>
void DfsAnagramSearch::visit_fitting_class(
    uint32_t class_index, size_t letters_left,
    double representative_log_score, DfsSolutionSink* sink) {
  HotClass const& candidate = hot_classes.get()[class_index];
  size_t const candidate_length =
      hot_letter_length(candidate.packed_length_and_count);
  assert(candidate_length <= letters_left);
  size_t const next_letters_left = letters_left - candidate_length;
  bool const first_class = path.empty();

  path.push_back(class_index);
  if (DFS_UNLIKELY(next_letters_left == 0)) {
    double const next_log_score =
        first_class
            ? candidate.best_member_log_score
            : representative_log_score + restart_log_rate +
                  candidate.best_member_log_score;
    ++solutions;
    if (sink != NULL) sink->emit(path, next_log_score);
    path.pop_back();
    return;
  }
  if (DFS_UNLIKELY(path.size() >= max_depth)) {
    path.pop_back();
    return;
  }

  double const next_log_score =
      first_class
          ? candidate.best_member_log_score
          : representative_log_score + restart_log_rate +
                candidate.best_member_log_score;
  uint32_t const* requirements =
      packed_letters.get() + candidate.letters_offset;
  uint32_t const requirement_count =
      hot_requirement_count(candidate.packed_length_and_count);
  uint64_t const parent_bag_mask = bag_mask;
  for (uint32_t i = 0; i < requirement_count; ++i) {
    uint32_t const requirement = requirements[i];
    uint32_t const requirement_rank = packed_rank(requirement);
    bag[requirement_rank] -= packed_count(requirement);
    if (bag[requirement_rank] == 0)
      bag_mask &= ~(UINT64_C(1) << requirement_rank);
  }
  current_bag_key -= candidate.bag_key_delta;
  walk<mode>(next_letters_left, class_index, next_log_score, sink);
  current_bag_key += candidate.bag_key_delta;
  for (uint32_t i = 0; i < requirement_count; ++i) {
    uint32_t const requirement = requirements[i];
    uint32_t const requirement_rank = packed_rank(requirement);
    bag[requirement_rank] += packed_count(requirement);
  }
  bag_mask = parent_bag_mask;
  path.pop_back();
}

template<DfsAnagramSearch::WalkMode mode>
void DfsAnagramSearch::walk(size_t letters_left, size_t entry_point,
                            double representative_log_score,
                            DfsSolutionSink* sink) {
  ++nodes;
  if (DFS_UNLIKELY(progress_stream != NULL && nodes == next_progress)) {
    fprintf(progress_stream,
            "# phase 2: %lld nodes, %lld solutions\n",
            (long long) nodes, (long long) solutions);
    fflush(progress_stream);
    if (next_progress <= INT64_MAX - progress_interval)
      next_progress += progress_interval;
    else
      next_progress = INT64_MAX;
  }

  if (DFS_UNLIKELY(bag_mask == 0)) return;

  uint64_t metadata = CACHE_BYPASSED;
  size_t sparse_slot = 0;
  bool sparse_may_insert = false;
  if (mode == WALK_DENSE) {
    metadata = cache_metadata.get()[size_t(current_bag_key)];
  } else if (mode == WALK_SPARSE) {
    metadata = sparse_lookup(
        current_bag_key, &sparse_slot, &sparse_may_insert);
  }

  if (mode != WALK_UNCACHED && DFS_LIKELY(metadata != CACHE_BYPASSED)) {
    if (DFS_LIKELY(metadata != CACHE_UNSEEN)) {
      uint32_t const count = entry_count(metadata);
      if (count == 0) return;
      uint32_t const* first =
          candidate_ids.get() + entry_offset(metadata);
      uint32_t const* last = first + count;
      while (first != last && *first < uint32_t(entry_point)) ++first;
      for (; first != last; ++first)
        visit_fitting_class<mode>(
            *first, letters_left, representative_log_score, sink);
      return;
    }
  }

  int const rank = __builtin_ctzll(bag_mask);
  int const forced_symbol =
      class_list->rank_to_symbol()[size_t(rank)];
  size_t const begin = class_list->candidate_begin(forced_symbol);
  size_t const end = class_list->candidate_end(forced_symbol);

  if (mode == WALK_DENSE && metadata == CACHE_UNSEEN) {
    build_candidate_entry(begin, end, &metadata);
    publish_dense(current_bag_key, metadata);
  } else if (mode == WALK_SPARSE && metadata == CACHE_UNSEEN) {
    if (sparse_may_insert) {
      build_candidate_entry(begin, end, &metadata);
      publish_sparse(
          sparse_slot, current_bag_key, metadata);
    } else {
      metadata = CACHE_BYPASSED;
    }
  }

  if (mode != WALK_UNCACHED && metadata != CACHE_BYPASSED) {
    uint32_t const count = entry_count(metadata);
    if (count == 0) return;
    uint32_t const* first = candidate_ids.get() + entry_offset(metadata);
    uint32_t const* last = first + count;
    while (first != last && *first < uint32_t(entry_point)) ++first;
    for (; first != last; ++first)
      visit_fitting_class<mode>(
          *first, letters_left, representative_log_score, sink);
    return;
  }

  size_t const start = std::max(begin, entry_point);
  for (size_t class_index = start; class_index < end; ++class_index) {
    uint32_t const id = uint32_t(class_index);
    if (!hot_class_fits(id)) continue;
    visit_fitting_class<mode>(
        id, letters_left, representative_log_score, sink);
  }
}

void DfsAnagramSearch::visit_unoptimized_class(
    size_t class_index, size_t letters_left, int rank,
    double representative_log_score, DfsSolutionSink* sink) {
  DfsAnagramClass const& candidate = class_list->classes()[class_index];
  double const candidate_log_score = best_member_log_scores[class_index];
  double const next_log_score =
      path.empty()
          ? candidate_log_score
          : representative_log_score + restart_log_rate +
                candidate_log_score;
  size_t const candidate_length = candidate.key.size();
  assert(candidate_length <= letters_left);
  size_t const next_letters_left = letters_left - candidate_length;

  path.push_back(class_index);
  if (next_letters_left == 0) {
    ++solutions;
    if (sink != NULL) sink->emit(path, next_log_score);
    path.pop_back();
    return;
  }
  if (path.size() >= max_depth) {
    path.pop_back();
    return;
  }

  std::array<int, DFS_SYMBOL_COUNT> const& symbol_to_rank =
      class_list->symbol_to_rank();
  for (size_t i = 0; i < candidate.letters.size(); ++i) {
    size_t const requirement_rank = size_t(
        symbol_to_rank[size_t(candidate.letters[i].first)]);
    bag[requirement_rank] -= candidate.letters[i].second;
  }
  walk_unoptimized(next_letters_left, rank, class_index,
                   next_log_score, sink);
  for (size_t i = 0; i < candidate.letters.size(); ++i) {
    size_t const requirement_rank = size_t(
        symbol_to_rank[size_t(candidate.letters[i].first)]);
    bag[requirement_rank] += candidate.letters[i].second;
  }
  path.pop_back();
}

void DfsAnagramSearch::walk_unoptimized(
    size_t letters_left, int old_rarest_rank, size_t entry_point,
    double representative_log_score, DfsSolutionSink* sink) {
  ++nodes;
  if (progress_stream != NULL && nodes == next_progress) {
    fprintf(progress_stream,
            "# phase 2: %lld nodes, %lld solutions\n",
            (long long) nodes, (long long) solutions);
    fflush(progress_stream);
    if (next_progress <= INT64_MAX - progress_interval)
      next_progress += progress_interval;
    else
      next_progress = INT64_MAX;
  }

  int rank = old_rarest_rank;
  while (rank < DFS_SYMBOL_COUNT && bag[size_t(rank)] == 0) ++rank;
  if (rank == DFS_SYMBOL_COUNT) return;

  int const forced_symbol =
      class_list->rank_to_symbol()[size_t(rank)];
  size_t start = std::max(
      class_list->candidate_begin(forced_symbol), entry_point);
  size_t const end = class_list->candidate_end(forced_symbol);
  std::array<int, DFS_SYMBOL_COUNT> const& symbol_to_rank =
      class_list->symbol_to_rank();
  std::vector<DfsAnagramClass> const& classes = class_list->classes();
  for (size_t class_index = start; class_index < end; ++class_index) {
    DfsAnagramClass const& candidate = classes[class_index];
    bool fits = true;
    for (size_t i = 0; i < candidate.letters.size(); ++i) {
      size_t const requirement_rank = size_t(
          symbol_to_rank[size_t(candidate.letters[i].first)]);
      if (bag[requirement_rank] < candidate.letters[i].second) {
        fits = false;
        break;
      }
    }
    if (!fits) continue;
    visit_unoptimized_class(class_index, letters_left, rank,
                            representative_log_score, sink);
  }
}
