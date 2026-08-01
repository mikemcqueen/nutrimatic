#include "dfs-any-runner.h"

#include "dfs-diagnostic.h"
#include "dfs-search-internal.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <new>
#include <thread>
#include <type_traits>
#include <vector>

#include <immintrin.h>

namespace {

size_t const EXACT_MEMO_LOOKAHEAD_MAX = 64;

// Returns the first index in [begin, end) whose support mask fits in the bag,
// or end. A candidate fits when it needs no letter the bag has run out of,
// which is exactly (support & ~bag_mask) == 0. Callers depend on this
// returning the *first* such index, so the scan order is unchanged.
static size_t next_support_fit_scalar(
    uint64_t const* supports, size_t begin, size_t end, uint64_t absent) {
  for (size_t i = begin; i < end; ++i)
    if ((supports[i] & absent) == 0) return i;
  return end;
}

#if defined(__x86_64__)
__attribute__((target("avx2"))) static size_t next_support_fit_avx2(
    uint64_t const* supports, size_t begin, size_t end, uint64_t absent) {
  __m256i const absent_lanes = _mm256_set1_epi64x(int64_t(absent));
  __m256i const zero = _mm256_setzero_si256();
  size_t i = begin;
  for (; i + 16 <= end; i += 16) {
    __m256i const* const block =
        reinterpret_cast<__m256i const*>(supports + i);
    int mask = 0;
    for (int lane = 0; lane < 4; ++lane) {
      __m256i const needed = _mm256_and_si256(
          _mm256_loadu_si256(block + lane), absent_lanes);
      mask |= _mm256_movemask_pd(_mm256_castsi256_pd(
          _mm256_cmpeq_epi64(needed, zero))) << (lane * 4);
    }
    if (mask != 0) return i + size_t(__builtin_ctz(unsigned(mask)));
  }
  for (; i + 4 <= end; i += 4) {
    __m256i const needed = _mm256_and_si256(
        _mm256_loadu_si256(reinterpret_cast<__m256i const*>(supports + i)),
        absent_lanes);
    int const mask = _mm256_movemask_pd(_mm256_castsi256_pd(
        _mm256_cmpeq_epi64(needed, zero)));
    if (mask != 0) return i + size_t(__builtin_ctz(unsigned(mask)));
  }
  return next_support_fit_scalar(supports, i, end, absent);
}
#endif

}  // namespace

bool support_scan_avx2_enabled() {
#if defined(__x86_64__)
  char const* mode = getenv("NUTRIMATIC_SUPPORT_SIMD");
  if (mode != NULL && mode[0] != '\0' && strcmp(mode, "1") != 0)
    return false;
  return __builtin_cpu_supports("avx2");
#else
  return false;
#endif
}

namespace {

static size_t next_support_fit(
    bool vector, uint64_t const* supports, size_t begin, size_t end,
    uint64_t absent) {
#if defined(__x86_64__)
  if (vector) return next_support_fit_avx2(supports, begin, end, absent);
#endif
  return next_support_fit_scalar(supports, begin, end, absent);
}

static size_t exact_memo_lookahead_choice() {
  // This is called once per exact-validation batch, never in the recurrence.
  char const* forced = getenv("NUTRIMATIC_EXACT_MEMO_LOOKAHEAD");
  if (forced == NULL || forced[0] == '\0')
    return EXACT_MEMO_LOOKAHEAD_DEFAULT;
  for (char const* p = forced; *p != '\0'; ++p)
    if (*p < '0' || *p > '9')
      return EXACT_MEMO_LOOKAHEAD_DEFAULT;
  errno = 0;
  char* end = NULL;
  unsigned long long const parsed = strtoull(forced, &end, 10);
  if (errno == 0 && end != forced && *end == '\0' &&
      parsed <= EXACT_MEMO_LOOKAHEAD_MAX)
    return size_t(parsed);
  return EXACT_MEMO_LOOKAHEAD_DEFAULT;
}

}  // namespace

static uint64_t exact_memo_hash(uint64_t value) {
  value ^= value >> 30;
  value *= UINT64_C(0xbf58476d1ce4e5b9);
  value ^= value >> 27;
  value *= UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31);
}

bool DfsAnySolutionRunner::memo_lookup(
    Worker* worker, uint64_t exact_key, bool* value) const {
  assert(memo.slots.get() != NULL);
  size_t slot = size_t(exact_memo_hash(exact_key)) & (memo.capacity - 1);
  for (;;) {
    uint64_t const stored = memo.slots.get()[slot].value.load(
        std::memory_order_relaxed);
    if (stored == 0) return false;
    if ((stored - 1) / 2 == exact_key) {
      *value = (stored & UINT64_C(1)) == 0;
      ++worker->stats.memo.hits;
      return true;
    }
    slot = (slot + 1) & (memo.capacity - 1);
  }
}

void DfsAnySolutionRunner::memo_store(
    Worker* worker, uint64_t exact_key, bool value) {
  assert(memo.slots.get() != NULL);
  assert(exact_key <= (UINT64_MAX - 2) / 2);
  uint64_t const desired = exact_key * UINT64_C(2) + (value ? 2 : 1);
  size_t slot = size_t(exact_memo_hash(exact_key)) & (memo.capacity - 1);
  for (;;) {
    uint64_t stored = memo.slots.get()[slot].value.load(
        std::memory_order_relaxed);
    if (stored != 0 && (stored - 1) / 2 == exact_key)
      return;
    if (stored == 0) {
      if (memo.entries.load(std::memory_order_relaxed) >= memo.entry_limit)
        return;
      if (memo.slots.get()[slot].value.compare_exchange_strong(
              stored, desired, std::memory_order_relaxed,
              std::memory_order_relaxed)) {
        memo.entries.fetch_add(1, std::memory_order_relaxed);
        ++worker->stats.memo.states;
        return;
      }
      // Recheck the slot after losing the race. The winner may have inserted
      // this exact key; advancing immediately would create a duplicate entry.
      continue;
    }
    slot = (slot + 1) & (memo.capacity - 1);
  }
}

bool DfsAnySolutionRunner::multiplicity_fits(
    uint32_t class_index, Worker const& worker) const {
  FitClassMetadata const metadata = data.fit_classes[class_index].metadata;
  return hot_multiplicity_fits(
      data.packed_letters.get() + metadata.letters_offset,
      metadata.packed_length_and_count, worker.bag.data());
}

bool DfsAnySolutionRunner::class_fits(
    uint32_t class_index, Worker const& worker) const {
  FitClass const& candidate = data.fit_classes[class_index];
  if ((candidate.support_mask & ~worker.bag_mask) != 0) return false;
  return multiplicity_fits(class_index, worker);
}

void DfsAnySolutionRunner::subtract_class(
    uint32_t class_index, Worker* worker, uint64_t* parent_bag_mask) const {
  *parent_bag_mask = worker->bag_mask;
  worker->exact_key -= data.class_list->classes()[class_index].signature;
  FitClassMetadata const metadata = data.fit_classes[class_index].metadata;
  uint32_t const* requirements =
      data.packed_letters.get() + metadata.letters_offset;
  uint32_t const requirement_count =
      hot_requirement_count(metadata.packed_length_and_count);
  for (uint32_t i = 0; i < requirement_count; ++i) {
    uint32_t const requirement = requirements[i];
    uint32_t const rank = packed_rank(requirement);
    uint32_t& remaining = worker->bag[rank];
    remaining -= packed_count(requirement);
    if (remaining == 0) worker->bag_mask &= ~(UINT64_C(1) << rank);
  }
  if (data.score_bounds_active)
    worker->score_key -= data.score_key_deltas[class_index];
}

void DfsAnySolutionRunner::restore_class(
    uint32_t class_index, Worker* worker, uint64_t parent_bag_mask) const {
  FitClassMetadata const metadata = data.fit_classes[class_index].metadata;
  uint32_t const* requirements =
      data.packed_letters.get() + metadata.letters_offset;
  uint32_t const requirement_count =
      hot_requirement_count(metadata.packed_length_and_count);
  if (data.score_bounds_active)
    worker->score_key += data.score_key_deltas[class_index];
  for (uint32_t i = 0; i < requirement_count; ++i) {
    uint32_t const requirement = requirements[i];
    worker->bag[packed_rank(requirement)] += packed_count(requirement);
  }
  worker->exact_key += data.class_list->classes()[class_index].signature;
  worker->bag_mask = parent_bag_mask;
}

inline DfsAnySolutionRunner::ChildResult
DfsAnySolutionRunner::classify_child(
    Worker* worker, uint32_t class_index, size_t candidate_length,
    size_t letters_left) {
  if (candidate_length == letters_left) return CHILD_TRUE;

  DfsClassSpan const classes = data.class_list->classes();
  uint64_t const child_exact_key =
      worker->exact_key - classes[class_index].signature;
  bool child_result;
  if (memo_lookup(worker, child_exact_key, &child_result))
    return child_result ? CHILD_TRUE : CHILD_FALSE;

  if (!data.score_bounds_active) return CHILD_UNKNOWN;

  uint64_t const child_score_key =
      worker->score_key - data.score_key_deltas[class_index];
  Reachability const child_reachability =
      data.cached_reachability(child_score_key, false);
  if (child_reachability == REACHABILITY_UNKNOWN) return CHILD_UNKNOWN;

  child_result = child_reachability == REACHABILITY_YES;
  memo_store(worker, child_exact_key, child_result);
  return child_result ? CHILD_TRUE : CHILD_FALSE;
}

bool DfsAnySolutionRunner::candidates_immediate(
    Worker* worker, size_t letters_left) {
  if (worker->bag_mask == 0) return false;

  int const rank = __builtin_ctzll(worker->bag_mask);
  int const forced_symbol = data.class_list->rank_to_symbol()[size_t(rank)];
  size_t const begin = data.first_length_candidate(
      data.class_list->candidate_begin(forced_symbol),
      data.class_list->candidate_end(forced_symbol), letters_left);
  size_t const end = data.class_list->candidate_end(forced_symbol);

  FitClass const* const fits = data.fit_classes.get();
  uint64_t const* const supports = data.class_supports.get();
  uint64_t const absent = ~worker->bag_mask;
  auto const next_fit = [&](size_t from) {
    return next_support_fit(
        data.support_scan_vector, supports, from, end, absent);
  };
  for (size_t class_index = next_fit(begin); class_index < end;
       class_index = next_fit(class_index + 1)) {
    FitClass const& candidate = fits[class_index];
    if (!multiplicity_fits(uint32_t(class_index), *worker)) continue;
    size_t const candidate_length =
        hot_letter_length(candidate.metadata.packed_length_and_count);

    ChildResult const child = classify_child(
        worker, uint32_t(class_index), candidate_length, letters_left);
    if (child == CHILD_TRUE) return true;
    if (child == CHILD_FALSE) continue;

    uint64_t parent_bag_mask;
    subtract_class(uint32_t(class_index), worker, &parent_bag_mask);
    bool const result = expand_node(worker, letters_left - candidate_length);
    restore_class(uint32_t(class_index), worker, parent_bag_mask);
    if (result) return true;
  }
  return false;
}

bool DfsAnySolutionRunner::buffered_candidates(
    Worker* worker, size_t letters_left, uint32_t const* class_ids,
    size_t count) {
  DfsClassSpan const classes = data.class_list->classes();
  for (size_t i = 0; i < count; ++i) {
    uint32_t const class_index = class_ids[i];
    size_t const candidate_length = classes[class_index].key_length;
    ChildResult const child = classify_child(
        worker, class_index, candidate_length, letters_left);
    if (child != CHILD_UNKNOWN) {
      ++worker->stats.lookahead.reprobes_decided;
      if (child == CHILD_TRUE) {
        ++worker->stats.lookahead.known_true_wins;
        return true;
      }
      continue;
    }

    ++worker->stats.lookahead.recursive_expansions;
    uint64_t parent_bag_mask;
    subtract_class(class_index, worker, &parent_bag_mask);
    bool const result = expand_node(worker, letters_left - candidate_length);
    restore_class(class_index, worker, parent_bag_mask);
    if (result) return true;
  }
  return false;
}

bool DfsAnySolutionRunner::candidates_lookahead(
    Worker* worker, size_t letters_left) {
  assert(lookahead > 0);
  assert(lookahead <= EXACT_MEMO_LOOKAHEAD_MAX);
  if (worker->bag_mask == 0) return false;

  int const rank = __builtin_ctzll(worker->bag_mask);
  int const forced_symbol = data.class_list->rank_to_symbol()[size_t(rank)];
  size_t const begin = data.first_length_candidate(
      data.class_list->candidate_begin(forced_symbol),
      data.class_list->candidate_end(forced_symbol), letters_left);
  size_t const end = data.class_list->candidate_end(forced_symbol);

  uint32_t buffered[EXACT_MEMO_LOOKAHEAD_MAX];
  size_t buffered_count = 0;
  FitClass const* const fits = data.fit_classes.get();
  uint64_t const* const supports = data.class_supports.get();
  uint64_t const absent = ~worker->bag_mask;
  auto const next_fit = [&](size_t from) {
    return next_support_fit(
        data.support_scan_vector, supports, from, end, absent);
  };
  for (size_t class_index = next_fit(begin); class_index < end;
       class_index = next_fit(class_index + 1)) {
    FitClass const& candidate = fits[class_index];
    if (!multiplicity_fits(uint32_t(class_index), *worker)) continue;
    size_t const candidate_length =
        hot_letter_length(candidate.metadata.packed_length_and_count);

    ChildResult const child = classify_child(
        worker, uint32_t(class_index), candidate_length, letters_left);
    if (child == CHILD_TRUE) {
      if (buffered_count != 0)
        ++worker->stats.lookahead.known_true_wins;
      return true;
    }
    if (child == CHILD_FALSE) continue;

    buffered[buffered_count++] = uint32_t(class_index);
    if (buffered_count != lookahead) continue;

    ++worker->stats.lookahead.full_windows;
    if (buffered_candidates(worker, letters_left, buffered, buffered_count))
      return true;
    buffered_count = 0;
  }

  return buffered_count != 0 &&
      buffered_candidates(worker, letters_left, buffered, buffered_count);
}

bool DfsAnySolutionRunner::remainder_completable(
    Worker* worker, size_t letters_left, ResultSource* source) {
  if (letters_left == 0) {
    *source = RESULT_EMPTY;
    return true;
  }

  bool memoized;
  if (memo_lookup(worker, worker->exact_key, &memoized)) {
    *source = RESULT_MEMO;
    return memoized;
  }

  Reachability const reachability =
      data.cached_reachability(worker->score_key, false);
  if (reachability != REACHABILITY_UNKNOWN) {
    bool const result = reachability == REACHABILITY_YES;
    memo_store(worker, worker->exact_key, result);
    *source = result ? RESULT_BOUND_YES : RESULT_BOUND_NO;
    return result;
  }

  *source = RESULT_SEARCH;
  return expand_node(worker, letters_left);
}

bool DfsAnySolutionRunner::expand_node(
    Worker* worker, size_t letters_left) {
  ++worker->stats.nodes;

  bool const result = lookahead == 0
      ? candidates_immediate(worker, letters_left)
      : candidates_lookahead(worker, letters_left);

  memo_store(worker, worker->exact_key, result);
  return result;
}

DfsAnySolutionRunner::DfsAnySolutionRunner(
    DfsSearchData data)
    : data(std::move(data)), lookahead(exact_memo_lookahead_choice()) {}

bool DfsAnySolutionRunner::prepare_memo(
    DfsClassSpan classes) {
  static_assert(sizeof(AtomicWord) == sizeof(uint64_t),
                "exact memo words must remain eight bytes");
  static_assert(std::is_trivially_destructible<AtomicWord>::value,
                "exact memo words must be trivially destructible");
  uint64_t packed_root_key;
  if (!dfs_checked_multiply_u64(
          data.exact_root_key, UINT64_C(2), &packed_root_key) ||
      !dfs_checked_add_u64(
          packed_root_key, UINT64_C(2), &packed_root_key)) {
    dfs_diagnostic_to_stream(
        stderr,
        "error: phase 2 exact memo key arithmetic overflowed 64 bits\n");
    return false;
  }
  uint64_t wanted;
  if (!dfs_checked_multiply_u64(uint64_t(classes.size()), UINT64_C(2),
                                &wanted)) {
    dfs_diagnostic_to_stream(
        stderr,
        "error: phase 2 exact memo table sizing overflowed 64 bits\n");
    return false;
  }
  wanted = std::max<uint64_t>(wanted, UINT64_C(2));
  uint64_t capacity = 1;
  while (capacity < wanted) {
    if (!dfs_checked_multiply_u64(capacity, UINT64_C(2), &capacity)) {
      dfs_diagnostic_to_stream(
          stderr,
          "error: phase 2 exact memo table sizing overflowed 64 bits\n");
      return false;
    }
  }
  uint64_t table_bytes;
  if (!dfs_checked_multiply_u64(
          capacity, uint64_t(sizeof(AtomicWord)), &table_bytes) ||
      capacity > SIZE_MAX || table_bytes > SIZE_MAX) {
    dfs_diagnostic_to_stream(
        stderr, "error: phase 2 exact memo table exceeds the address space\n");
    return false;
  }
  AtomicWord* slots = static_cast<AtomicWord*>(
      dfs_allocate_aligned(size_t(table_bytes)));
  if (slots == NULL) {
    dfs_diagnostic_to_stream(
        stderr,
        "error: phase 2 could not allocate the %llu-byte exact memo table\n",
        (unsigned long long)table_bytes);
    return false;
  }
  for (size_t i = 0; i < size_t(capacity); ++i) {
    new (&slots[i]) AtomicWord;
    slots[i].value.store(0, std::memory_order_relaxed);
  }
  memo.slots.reset(slots);
  memo.capacity = size_t(capacity);
  memo.entry_limit = memo.capacity - memo.capacity / 4;
  if (data.progress_enabled)
    dfs_diagnostic("phase 2 exact memo table: %zu slots, %llu bytes\n",
                   memo.capacity, (unsigned long long)table_bytes);
  return true;
}

bool DfsAnySolutionRunner::run(
    std::vector<bool>* completable, DfsSearchStats* results) {
  DfsClassSpan const classes = data.class_list->classes();
  if (!prepare_memo(classes)) return false;
  completable->assign(classes.size(), false);
  std::vector<unsigned char> decided(classes.size(), 0);
  size_t const worker_count = std::min(
      data.requested_search_threads, std::max(size_t(1), classes.size()));
  std::atomic<size_t> next_class(0);
  std::vector<Worker> workers(worker_count);
  for (size_t i = 0; i < worker_count; ++i) {
    Worker& worker = workers[i];
    worker.bag = data.bag;
    worker.bag_mask = data.bag_mask;
    worker.score_key = data.score_key;
    worker.exact_key = data.exact_root_key;
  }

  auto const body = [&](size_t worker_index) {
    Worker& worker = workers[worker_index];
    for (;;) {
      size_t const class_index =
          next_class.fetch_add(1, std::memory_order_relaxed);
      if (class_index >= classes.size()) break;
      ++worker.stats.classes_checked;
      assert(class_fits(uint32_t(class_index), worker));
      uint64_t parent_bag_mask;
      subtract_class(uint32_t(class_index), &worker, &parent_bag_mask);
      ResultSource source;
      bool const result = remainder_completable(
          &worker, data.letter_count - classes[class_index].key_length,
          &source);
      restore_class(uint32_t(class_index), &worker, parent_bag_mask);
      decided[class_index] = result;
      if (source == RESULT_BOUND_NO) ++worker.stats.bound_rejects;
      else if (source == RESULT_BOUND_YES) ++worker.stats.exact_bound_accepts;
      else if (source == RESULT_SEARCH) ++worker.stats.exact_validations;
    }
  };
  std::vector<std::thread> pool;
  pool.reserve(worker_count - 1);
  try {
    for (size_t i = 1; i < worker_count; ++i)
      pool.push_back(std::thread(body, i));
  } catch (...) {
    // Successfully launched workers and the caller share the class cursor, so
    // they still finish the full batch if a later launch fails.
  }
  results->execution.search_threads = 1 + pool.size();
  body(0);
  for (size_t i = 0; i < pool.size(); ++i) pool[i].join();
  for (size_t i = 0; i < classes.size(); ++i)
    (*completable)[i] = decided[i] != 0;
  for (size_t i = 0; i < results->execution.search_threads; ++i)
    results->any_solution.add(workers[i].stats);

  if (data.progress_enabled) {
    DfsSearchStats::AnySolution const& any = results->any_solution;
    dfs_diagnostic(
        "phase 2 completability: %zu classes checked, "
        "%zu rejected by bounds, %zu accepted by exact bounds, "
        "%zu exact validations\n",
        any.classes_checked, any.bound_rejects,
        any.exact_bound_accepts, any.exact_validations);
    dfs_diagnostic(
        "phase 2 exact memo: %zu states computed, %zu hits\n",
        any.memo.states, any.memo.hits);
    dfs_diagnostic(
        "phase 2 exact lookahead: width %zu, %llu full windows, "
        "%llu known-true wins, %llu re-probes decided, "
        "%llu recursive expansions\n",
        lookahead,
        (unsigned long long)any.lookahead.full_windows,
        (unsigned long long)any.lookahead.known_true_wins,
        (unsigned long long)any.lookahead.reprobes_decided,
        (unsigned long long)any.lookahead.recursive_expansions);
  }
  return true;
}
