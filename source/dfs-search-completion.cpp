#include "dfs-search.h"

#include "dfs-diagnostic.h"
#include "dfs-search-internal.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <new>
#include <thread>
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

bool DfsAnagramSearch::exact_memo_lookup(
    SearchWorker* worker, uint64_t exact_key, bool* value) {
  assert(exact_flat_memo.get() != NULL);
  size_t slot = size_t(exact_memo_hash(exact_key)) &
      (exact_flat_capacity - 1);
  for (;;) {
    uint64_t const stored = exact_flat_memo.get()[slot].value.load(
        std::memory_order_relaxed);
    if (stored == 0) return false;
    if ((stored - 1) / 2 == exact_key) {
      *value = (stored & UINT64_C(1)) == 0;
      ++worker->exact_memo_hits;
      return true;
    }
    slot = (slot + 1) & (exact_flat_capacity - 1);
  }
}

void DfsAnagramSearch::exact_memo_store(
    SearchWorker* worker, uint64_t exact_key, bool value) {
  assert(exact_flat_memo.get() != NULL);
  assert(exact_key <= (UINT64_MAX - 2) / 2);
  uint64_t const desired =
      exact_key * UINT64_C(2) + (value ? 2 : 1);
  size_t slot = size_t(exact_memo_hash(exact_key)) &
      (exact_flat_capacity - 1);
  for (;;) {
    uint64_t stored = exact_flat_memo.get()[slot].value.load(
        std::memory_order_relaxed);
    if (stored != 0 && (stored - 1) / 2 == exact_key)
      return;
    if (stored == 0) {
      if (exact_flat_entries.load(std::memory_order_relaxed) >=
          exact_flat_entry_limit)
        return;
      if (exact_flat_memo.get()[slot].value.compare_exchange_strong(
              stored, desired, std::memory_order_relaxed,
              std::memory_order_relaxed)) {
        exact_flat_entries.fetch_add(1, std::memory_order_relaxed);
        ++worker->exact_memo_states;
        return;
      }
      // Recheck the slot after losing the race. The winner may have inserted
      // this exact key; advancing immediately would create a duplicate entry.
      continue;
    }
    slot = (slot + 1) & (exact_flat_capacity - 1);
  }
}

bool DfsAnagramSearch::exact_class_fits(
    size_t class_index, SearchWorker const& worker) const {
  if (hot_classes_ready)
    return hot_class_fits(uint32_t(class_index), worker);

  std::array<int, DFS_SYMBOL_COUNT> const& symbol_to_rank =
      class_list->symbol_to_rank();
  uint16_t decoded[DFS_SYMBOL_COUNT];
  size_t const count = class_list->decode_class_letters(class_index, decoded);
  for (size_t i = 0; i < count; ++i) {
    size_t const rank = size_t(
        symbol_to_rank[size_t(dfs_class_letter_symbol(decoded[i]))]);
    if (worker.bag[rank] < dfs_class_letter_count(decoded[i])) return false;
  }
  return true;
}

void DfsAnagramSearch::subtract_exact_class(
    size_t class_index, SearchWorker* worker,
    uint64_t* parent_bag_mask) {
  *parent_bag_mask = worker->bag_mask;
  worker->exact_key -= class_list->classes()[class_index].signature;
  if (hot_classes_ready) {
    FitClass const& fit = fit_classes.get()[class_index];
    uint32_t const* requirements =
        packed_letters.get() + fit.metadata.letters_offset;
    uint32_t const requirement_count =
        hot_requirement_count(fit.metadata.packed_length_and_count);
    for (uint32_t i = 0; i < requirement_count; ++i) {
      uint32_t const requirement = requirements[i];
      uint32_t const rank = packed_rank(requirement);
      uint32_t& remaining = worker->bag[rank];
      remaining -= packed_count(requirement);
      if (remaining == 0) worker->bag_mask &= ~(UINT64_C(1) << rank);
    }
    if (score_bounds_active())
      worker->score_key -= score_key_deltas.get()[class_index];
    return;
  }

  std::array<int, DFS_SYMBOL_COUNT> const& symbol_to_rank =
      class_list->symbol_to_rank();
  uint16_t decoded[DFS_SYMBOL_COUNT];
  size_t const count = class_list->decode_class_letters(class_index, decoded);
  for (size_t i = 0; i < count; ++i) {
    size_t const rank = size_t(
        symbol_to_rank[size_t(dfs_class_letter_symbol(decoded[i]))]);
    worker->bag[rank] -= dfs_class_letter_count(decoded[i]);
    if (worker->bag[rank] == 0)
      worker->bag_mask &= ~(UINT64_C(1) << rank);
  }
}

void DfsAnagramSearch::restore_exact_class(
    size_t class_index, SearchWorker* worker,
    uint64_t parent_bag_mask) {
  if (hot_classes_ready) {
    FitClass const& fit = fit_classes.get()[class_index];
    uint32_t const* requirements =
        packed_letters.get() + fit.metadata.letters_offset;
    uint32_t const requirement_count =
        hot_requirement_count(fit.metadata.packed_length_and_count);
    if (score_bounds_active())
      worker->score_key += score_key_deltas.get()[class_index];
    for (uint32_t i = 0; i < requirement_count; ++i) {
      uint32_t const requirement = requirements[i];
      worker->bag[packed_rank(requirement)] += packed_count(requirement);
    }
  } else {
    std::array<int, DFS_SYMBOL_COUNT> const& symbol_to_rank =
        class_list->symbol_to_rank();
    uint16_t decoded[DFS_SYMBOL_COUNT];
    size_t const count =
        class_list->decode_class_letters(class_index, decoded);
    for (size_t i = 0; i < count; ++i) {
      size_t const rank = size_t(
          symbol_to_rank[size_t(dfs_class_letter_symbol(decoded[i]))]);
      worker->bag[rank] += dfs_class_letter_count(decoded[i]);
    }
  }
  worker->exact_key += class_list->classes()[class_index].signature;
  worker->bag_mask = parent_bag_mask;
}

inline DfsAnagramSearch::ExactChildResult
DfsAnagramSearch::classify_exact_child(
    SearchWorker* worker, uint32_t class_index,
    size_t candidate_length, size_t letters_left) {
  assert(hot_classes_ready);
  if (candidate_length == letters_left)
    return EXACT_CHILD_TRUE;

  DfsClassSpan const classes = class_list->classes();
  uint64_t const child_exact_key =
      worker->exact_key - classes[class_index].signature;
  bool child_result;
  if (exact_memo_lookup(worker, child_exact_key, &child_result))
    return child_result ? EXACT_CHILD_TRUE : EXACT_CHILD_FALSE;

  if (!score_bounds_active()) return EXACT_CHILD_UNKNOWN;

  uint64_t const child_score_key =
      worker->score_key - score_key_deltas.get()[class_index];
  Reachability const child_reachability =
      cached_reachability(child_score_key, false);
  if (child_reachability == REACHABILITY_UNKNOWN)
    return EXACT_CHILD_UNKNOWN;

  child_result = child_reachability == REACHABILITY_YES;
  exact_memo_store(worker, child_exact_key, child_result);
  return child_result ? EXACT_CHILD_TRUE : EXACT_CHILD_FALSE;
}

bool DfsAnagramSearch::exact_candidates_immediate(
    SearchWorker* worker, size_t letters_left) {
  assert(hot_classes_ready);
  if (worker->bag_mask == 0) return false;

  int const rank = __builtin_ctzll(worker->bag_mask);
  int const forced_symbol =
      class_list->rank_to_symbol()[size_t(rank)];
  size_t const begin = first_length_candidate(
      class_list->candidate_begin(forced_symbol),
      class_list->candidate_end(forced_symbol), letters_left);
  size_t const end = class_list->candidate_end(forced_symbol);

  FitClass const* const fits = fit_classes.get();
  uint64_t const* const supports = class_supports.get();
  uint64_t const absent = ~worker->bag_mask;
  auto const next_fit = [&](size_t from) {
    return next_support_fit(support_scan_vector, supports, from, end, absent);
  };
  for (size_t class_index = next_fit(begin); class_index < end;
       class_index = next_fit(class_index + 1)) {
    FitClass const& candidate = fits[class_index];
    if (!hot_class_multiplicity_fits(uint32_t(class_index), *worker))
      continue;
    size_t const candidate_length =
        hot_letter_length(candidate.metadata.packed_length_and_count);

    ExactChildResult const child = classify_exact_child(
        worker, uint32_t(class_index), candidate_length, letters_left);
    if (child == EXACT_CHILD_TRUE) return true;
    if (child == EXACT_CHILD_FALSE) continue;

    uint64_t parent_bag_mask;
    subtract_exact_class(class_index, worker, &parent_bag_mask);
    bool const result =
        exact_expand_node(worker, letters_left - candidate_length);
    restore_exact_class(class_index, worker, parent_bag_mask);
    if (result) return true;
  }
  return false;
}

bool DfsAnagramSearch::exact_buffered_candidates(
    SearchWorker* worker, size_t letters_left,
    uint32_t const* class_ids, size_t count) {
  DfsClassSpan const classes = class_list->classes();
  for (size_t i = 0; i < count; ++i) {
    uint32_t const class_index = class_ids[i];
    size_t const candidate_length = classes[class_index].key_length;
    ExactChildResult const child = classify_exact_child(
        worker, class_index, candidate_length, letters_left);
    if (child != EXACT_CHILD_UNKNOWN) {
      ++worker->exact_lookahead_reprobes_decided;
      if (child == EXACT_CHILD_TRUE) {
        ++worker->exact_lookahead_known_true_wins;
        return true;
      }
      continue;
    }

    ++worker->exact_lookahead_recursive_expansions;
    uint64_t parent_bag_mask;
    subtract_exact_class(class_index, worker, &parent_bag_mask);
    bool const result =
        exact_expand_node(worker, letters_left - candidate_length);
    restore_exact_class(class_index, worker, parent_bag_mask);
    if (result) return true;
  }
  return false;
}

bool DfsAnagramSearch::exact_candidates_lookahead(
    SearchWorker* worker, size_t letters_left) {
  assert(exact_memo_lookahead > 0);
  assert(exact_memo_lookahead <= EXACT_MEMO_LOOKAHEAD_MAX);
  assert(hot_classes_ready);
  if (worker->bag_mask == 0) return false;

  int const rank = __builtin_ctzll(worker->bag_mask);
  int const forced_symbol =
      class_list->rank_to_symbol()[size_t(rank)];
  size_t const begin = first_length_candidate(
      class_list->candidate_begin(forced_symbol),
      class_list->candidate_end(forced_symbol), letters_left);
  size_t const end = class_list->candidate_end(forced_symbol);

  uint32_t buffered[EXACT_MEMO_LOOKAHEAD_MAX];
  size_t buffered_count = 0;
  FitClass const* const fits = fit_classes.get();
  uint64_t const* const supports = class_supports.get();
  uint64_t const absent = ~worker->bag_mask;
  auto const next_fit = [&](size_t from) {
    return next_support_fit(support_scan_vector, supports, from, end, absent);
  };
  for (size_t class_index = next_fit(begin); class_index < end;
       class_index = next_fit(class_index + 1)) {
    FitClass const& candidate = fits[class_index];
    if (!hot_class_multiplicity_fits(uint32_t(class_index), *worker))
      continue;
    size_t const candidate_length =
        hot_letter_length(candidate.metadata.packed_length_and_count);

    ExactChildResult const child = classify_exact_child(
        worker, uint32_t(class_index), candidate_length, letters_left);
    if (child == EXACT_CHILD_TRUE) {
      if (buffered_count != 0)
        ++worker->exact_lookahead_known_true_wins;
      return true;
    }
    if (child == EXACT_CHILD_FALSE) continue;

    buffered[buffered_count++] = uint32_t(class_index);
    if (buffered_count != exact_memo_lookahead) continue;

    ++worker->exact_lookahead_full_windows;
    if (exact_buffered_candidates(
            worker, letters_left, buffered, buffered_count))
      return true;
    buffered_count = 0;
  }

  return buffered_count != 0 &&
      exact_buffered_candidates(
          worker, letters_left, buffered, buffered_count);
}

bool DfsAnagramSearch::exact_remainder_completable(
    SearchWorker* worker, size_t letters_left,
    ExactResultSource* source) {
  if (letters_left == 0) {
    if (source != NULL) *source = EXACT_RESULT_EMPTY;
    return true;
  }

  bool memoized;
  if (exact_memo_lookup(worker, worker->exact_key, &memoized)) {
    if (source != NULL) *source = EXACT_RESULT_MEMO;
    return memoized;
  }

  Reachability const reachability =
      cached_reachability(worker->score_key, false);
  if (reachability != REACHABILITY_UNKNOWN) {
    bool const result = reachability == REACHABILITY_YES;
    exact_memo_store(worker, worker->exact_key, result);
    if (source != NULL)
      *source = result ? EXACT_RESULT_BOUND_YES : EXACT_RESULT_BOUND_NO;
    return result;
  }

  if (source != NULL) *source = EXACT_RESULT_SEARCH;
  return exact_expand_node(worker, letters_left);
}

bool DfsAnagramSearch::exact_expand_node(
    SearchWorker* worker, size_t letters_left) {
  ++worker->nodes;
  if (DFS_UNLIKELY(progress_enabled &&
                   worker->nodes == worker->next_progress))
    report_search_progress(worker);

  bool const result =
      exact_memo_lookahead == 0
          ? exact_candidates_immediate(worker, letters_left)
          : exact_candidates_lookahead(worker, letters_left);

  exact_memo_store(worker, worker->exact_key, result);
  return result;
}

bool DfsAnagramSearch::find_completable_classes(
    std::vector<bool>* completable, int64_t progress_factor,
    bool allow_cache_fallback, int exact_letters) {
  assert(completable != NULL);
  if (!prepare_phase_two(
          progress_factor, allow_cache_fallback, exact_letters, true))
    return false;

  DfsClassSpan const classes = class_list->classes();
  if (!classes.empty()) require_hot_classes();
  exact_flat_memo.reset();
  exact_flat_capacity = 0;
  exact_flat_entry_limit = 0;
  exact_flat_entries.store(0, std::memory_order_relaxed);
  uint64_t packed_root_key;
  if (!dfs_checked_multiply_u64(
          exact_root_key, UINT64_C(2), &packed_root_key) ||
      !dfs_checked_add_u64(
          packed_root_key, UINT64_C(2), &packed_root_key)) {
    dfs_diagnostic_to_stream(
        stderr,
        "error: phase 2 exact memo key arithmetic overflowed 64 bits\n");
    return false;
  }

  static_assert(sizeof(size_t) <= sizeof(uint64_t),
                "exact memo sizing requires at most 64-bit size_t");
  uint64_t wanted;
  if (!dfs_checked_multiply_u64(
          uint64_t(classes.size()), UINT64_C(2), &wanted)) {
    dfs_diagnostic_to_stream(
        stderr,
        "error: phase 2 exact memo table sizing overflowed 64 bits\n");
    return false;
  }
  wanted = std::max<uint64_t>(wanted, UINT64_C(2));
  uint64_t capacity = 1;
  while (capacity < wanted) {
    if (!dfs_checked_multiply_u64(
            capacity, UINT64_C(2), &capacity)) {
      dfs_diagnostic_to_stream(
          stderr,
          "error: phase 2 exact memo table sizing overflowed 64 bits\n");
      return false;
    }
  }
  uint64_t table_bytes;
  if (!dfs_checked_multiply_u64(
          capacity, uint64_t(sizeof(AtomicWord)), &table_bytes)) {
    dfs_diagnostic_to_stream(
        stderr,
        "error: phase 2 exact memo table sizing overflowed 64 bits\n");
    return false;
  }
  if (capacity > SIZE_MAX || table_bytes > SIZE_MAX) {
    dfs_diagnostic_to_stream(
        stderr,
        "error: phase 2 exact memo table exceeds the address space\n");
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
  exact_flat_memo.reset(slots);
  exact_flat_capacity = size_t(capacity);
  exact_flat_entry_limit =
      exact_flat_capacity - exact_flat_capacity / 4;
  if (progress_enabled)
    dfs_diagnostic(
        "phase 2 exact memo table: %zu slots, %llu bytes\n",
        exact_flat_capacity, (unsigned long long)table_bytes);
  completable_checked = 0;
  completable_bound_reject_count = 0;
  completable_exact_bound_accept_count = 0;
  completable_exact_validation_count = 0;
  exact_memo_states = 0;
  exact_memo_hit_count = 0;
  exact_memo_lookahead = exact_memo_lookahead_choice();
  exact_lookahead_full_windows = 0;
  exact_lookahead_known_true_wins = 0;
  exact_lookahead_reprobes_decided = 0;
  exact_lookahead_recursive_expansions = 0;

  completable->assign(classes.size(), false);
  std::vector<unsigned char> results(classes.size(), 0);
  size_t const worker_count = std::min(
      requested_search_threads, std::max(size_t(1), classes.size()));
  std::atomic<size_t> next_class(0);
  std::vector<SearchWorker> workers(worker_count);
  std::vector<size_t> checked(worker_count, 0);
  std::vector<size_t> bound_rejects(worker_count, 0);
  std::vector<size_t> bound_accepts(worker_count, 0);
  std::vector<size_t> exact_validations(worker_count, 0);
  for (size_t i = 0; i < worker_count; ++i)
    start_search_worker(&workers[i]);

  typedef std::chrono::steady_clock PhaseClock;
  PhaseClock::time_point const validation_start = PhaseClock::now();
  auto const body = [&](size_t worker_index) {
    SearchWorker& worker = workers[worker_index];
    for (;;) {
      size_t const class_index =
          next_class.fetch_add(1, std::memory_order_relaxed);
      if (class_index >= classes.size()) break;
      ++checked[worker_index];
      assert(exact_class_fits(class_index, worker));
      uint64_t parent_bag_mask;
      subtract_exact_class(class_index, &worker, &parent_bag_mask);
      ExactResultSource source;
      bool const result = exact_remainder_completable(
          &worker, letters.size() - classes[class_index].key_length,
          &source);
      restore_exact_class(class_index, &worker, parent_bag_mask);
      results[class_index] = result;

      if (source == EXACT_RESULT_BOUND_NO)
        ++bound_rejects[worker_index];
      else if (source == EXACT_RESULT_BOUND_YES)
        ++bound_accepts[worker_index];
      else if (source == EXACT_RESULT_SEARCH)
        ++exact_validations[worker_index];
    }
  };
  std::vector<std::thread> pool;
  pool.reserve(worker_count - 1);
  try {
    for (size_t i = 1; i < worker_count; ++i)
      pool.push_back(std::thread(body, i));
  } catch (...) {
    // Successfully launched workers and the caller share the class cursor,
    // so they still finish the full batch if a later launch fails.
  }
  actual_search_threads = 1 + pool.size();
  body(0);
  for (size_t i = 0; i < pool.size(); ++i)
    pool[i].join();
  for (size_t class_index = 0; class_index < classes.size(); ++class_index)
    (*completable)[class_index] = results[class_index] != 0;
  for (size_t i = 0; i < actual_search_threads; ++i) {
    merge_search_worker(workers[i]);
    completable_checked += checked[i];
    completable_bound_reject_count += bound_rejects[i];
    completable_exact_bound_accept_count += bound_accepts[i];
    completable_exact_validation_count += exact_validations[i];
    exact_memo_states += workers[i].exact_memo_states;
    exact_memo_hit_count += workers[i].exact_memo_hits;
    exact_lookahead_full_windows +=
        workers[i].exact_lookahead_full_windows;
    exact_lookahead_known_true_wins +=
        workers[i].exact_lookahead_known_true_wins;
    exact_lookahead_reprobes_decided +=
        workers[i].exact_lookahead_reprobes_decided;
    exact_lookahead_recursive_expansions +=
        workers[i].exact_lookahead_recursive_expansions;
  }
  search_seconds = std::chrono::duration<double>(
      PhaseClock::now() - validation_start).count();

  if (progress_enabled) {
    dfs_diagnostic(
        "phase 2 completability: %zu classes checked, "
        "%zu rejected by bounds, %zu accepted by exact bounds, "
        "%zu exact validations\n",
        completable_checked, completable_bound_reject_count,
        completable_exact_bound_accept_count,
        completable_exact_validation_count);
    dfs_diagnostic(
        "phase 2 exact memo: %zu states computed, %zu hits\n",
        exact_memo_states, exact_memo_hit_count);
    dfs_diagnostic(
        "phase 2 exact lookahead: width %zu, %llu full windows, "
        "%llu known-true wins, %llu re-probes decided, "
        "%llu recursive expansions\n",
        exact_memo_lookahead,
        (unsigned long long)exact_lookahead_full_windows,
        (unsigned long long)exact_lookahead_known_true_wins,
        (unsigned long long)exact_lookahead_reprobes_decided,
        (unsigned long long)exact_lookahead_recursive_expansions);
  }
  return true;
}
