#include "dfs-search.h"

#include <assert.h>
#include <fenv.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <new>
#include <thread>
#include <type_traits>

#if defined(__i386__) || defined(__x86_64__)
#include <immintrin.h>
#endif

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
uint64_t const BOUND_UNSEEN = UINT64_C(0x7ff8000000000001);
uint64_t const BOUND_COMPUTING = UINT64_C(0x7ff8000000000002);
uint32_t const FLOAT_BOUND_UNSEEN = UINT32_C(0x7fc00001);
uint32_t const FLOAT_BOUND_COMPUTING = UINT32_C(0x7fc00002);
size_t const CACHE_ALIGNMENT = 64;
size_t const MIB = size_t(1024) * size_t(1024);
// Presence masks are far fewer than multiplicity states. Measurements on the
// reference bags found that 1/16 retains every useful support list without
// materially shrinking the full candidate arena used by the later DFS.
size_t const SUPPORT_CACHE_BUDGET_DIVISOR = 16;
size_t const SUPPORT_CACHE_METADATA_DIVISOR = 4;

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

static bool dense_bound_requirements(
    uint64_t state_count, size_t value_bytes, size_t* dense_bytes) {
  if (state_count > SIZE_MAX / value_bytes ||
      !round_up_alignment(
          size_t(state_count) * value_bytes, dense_bytes))
    return false;
  return true;
}

static void* allocate_aligned(size_t bytes) {
  if (bytes == 0) return NULL;
  size_t rounded;
  if (!round_up_alignment(bytes, &rounded)) return NULL;
  return aligned_alloc(CACHE_ALIGNMENT, rounded);
}

static void* allocate_aligned_exact(size_t bytes) {
  if (bytes == 0) return NULL;
  void* result = NULL;
  return posix_memalign(&result, CACHE_ALIGNMENT, bytes) == 0
      ? result
      : NULL;
}

static uint64_t mix_key(uint64_t value) {
  value ^= value >> 30;
  value *= UINT64_C(0xbf58476d1ce4e5b9);
  value ^= value >> 27;
  value *= UINT64_C(0x94d049bb133111eb);
  value ^= value >> 31;
  return value;
}

static uint64_t double_to_bits(double value) {
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

static double bits_to_double(uint64_t bits) {
  double value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

static uint32_t float_to_bits(float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

static float bits_to_float(uint32_t bits) {
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

static float round_float_score_bound_up(double value) {
  if (value == -HUGE_VAL) return -HUGE_VALF;
  if (value == HUGE_VAL) return HUGE_VALF;
  float rounded = float(value);
  if (isinf(rounded) && rounded < 0.0f)
    return -FLT_MAX;
  if (double(rounded) < value)
    rounded = nextafterf(rounded, HUGE_VALF);
  return rounded;
}

static void bound_wait_backoff(unsigned int* spins) {
#if defined(__i386__) || defined(__x86_64__)
  _mm_pause();
#else
  std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
  ++*spins;
  if ((*spins & 255U) == 0)
    std::this_thread::yield();
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

static double round_score_bound_up(
    long double value, uint64_t* nextafter_calls) {
  if (value == -HUGE_VALL) return -HUGE_VAL;
  if (value == HUGE_VALL) return HUGE_VAL;
  double rounded = double(value);
  if (static_cast<long double>(rounded) < value) {
    rounded = nextafter(rounded, HUGE_VAL);
    ++*nextafter_calls;
  }
  // The candidate calculation supplies an explicit absolute-error envelope.
  // Keep two additional ulps as defense against conversion and later
  // regrouping with the path's accumulated score.
  rounded = nextafter(rounded, HUGE_VAL);
  rounded = nextafter(rounded, HUGE_VAL);
  *nextafter_calls += 2;
  return rounded;
}

// Bound the error in fl(fl(class_score + restart) + child). IEEE double
// round-to-nearest incurs less than one DBL_EPSILON times the sum of operand
// magnitudes here. The factor of four also covers rounding while calculating
// the magnitude and subnormal results (the added 1 dominates their error).
static double score_candidate_rounding_error(
    double class_score, double restart, double child) {
  double magnitude = fabs(class_score) + fabs(restart);
  magnitude += fabs(child);
  magnitude += 1.0;
  return magnitude * DBL_EPSILON * 4.0;
}

static bool score_bound_arithmetic_supported() {
#if defined(__FAST_MATH__)
  return false;
#else
  return std::numeric_limits<double>::is_iec559 &&
      std::numeric_limits<float>::is_iec559 &&
      FLT_RADIX == 2 && DBL_MANT_DIG == 53 &&
      FLT_MANT_DIG == 24 &&
      LDBL_MANT_DIG >= DBL_MANT_DIG &&
      fegetround() == FE_TONEAREST;
#endif
}

static char const* score_bound_mode_name(
    DfsAnagramSearch::ScoreBoundMode mode) {
  switch (mode) {
    case DfsAnagramSearch::SCORE_BOUND_DENSE:
      return "dense";
    case DfsAnagramSearch::SCORE_BOUND_PREFIX:
      return "dense prefix";
    case DfsAnagramSearch::SCORE_BOUND_OFF:
      return "off";
  }
  return "unknown";
}

static char const* candidate_cache_mode_name(
    DfsAnagramSearch::CandidateCacheMode mode) {
  switch (mode) {
    case DfsAnagramSearch::CANDIDATE_CACHE_DENSE:
      return "dense";
    case DfsAnagramSearch::CANDIDATE_CACHE_SPARSE:
      return "sparse";
    case DfsAnagramSearch::CANDIDATE_CACHE_OFF:
      return "off";
  }
  return "unknown";
}

}  // namespace

void DfsAnagramSearch::AlignedFree::operator()(void* pointer) const {
  free(pointer);
}

DfsAnagramSearch::DfsAnagramSearch(DfsClassList const* classes,
                                   std::string const& letters,
                                   double restart, int64_t corpus_total,
                                   size_t candidate_cache_bytes,
                                   size_t preprocess_threads,
                                   bool enable_candidate_cache):
    class_list(classes),
    letters(letters),
    restart_log_rate(make_restart_log_rate(restart, corpus_total)),
    max_depth(derived_max_depth(classes, letters.size())),
    candidate_cache_budget(candidate_cache_bytes),
    requested_preprocess_threads(std::max(size_t(1), preprocess_threads)),
    candidate_cache_enabled(enable_candidate_cache),
    active_candidate_cache_budget(candidate_cache_bytes),
    bag_mask(0),
    current_bag_key(0),
    current_score_key(0),
    current_letters_left(0),
    hot_classes_ready(false),
    bound_mode(SCORE_BOUND_OFF),
    bound_capacity(0),
    bound_value_bytes(0),
    bound_complete(false),
    root_score_bound(HUGE_VAL),
    root_score_bound_ready(false),
    bound_entries(0),
    bound_states_computed(0),
    bound_transitions(0),
    bound_nextafter_calls(0),
    bound_charged_bytes(0),
    bound_prunes(0),
    cache_mode(CANDIDATE_CACHE_OFF),
    cache_capacity(0),
    sparse_max_entries(0),
    sparse_filled(0),
    candidate_capacity(0),
    candidate_used(0),
    admitted_entries(0),
    charged_bytes(0),
    support_capacity(0),
    support_max_entries(0),
    support_filled(0),
    support_candidate_capacity(0),
    support_candidate_used(0),
    support_admitted_entries(0),
    support_charged_bytes(0),
    parallel_support_exhausted(false),
    progress_stream(NULL),
    progress_interval(0),
    next_progress(0),
    nodes(0),
    solutions(0),
    setup_seconds(0.0),
    search_seconds(0.0),
    actual_preprocess_threads(1) {
  assert(class_list != NULL);
  static_assert(sizeof(AtomicWord) == sizeof(uint64_t),
                "atomic cache words must remain eight bytes");
  static_assert(sizeof(AtomicFloatWord) == sizeof(uint32_t),
                "atomic float cache words must remain four bytes");
  static_assert(std::is_trivially_destructible<AtomicWord>::value,
                "atomic cache words must be trivially destructible");
  static_assert(std::is_trivially_destructible<AtomicFloatWord>::value,
                "atomic float cache words must be trivially destructible");

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
  static_assert(sizeof(FitClass) == 16,
                "FitClass must remain four per cache line");
  static_assert(sizeof(ScoreClass) == 16,
                "ScoreClass must remain four per cache line");
  fit_classes.reset();
  score_classes.reset();
  score_key_deltas.reset();
  packed_letters.reset();

  std::vector<DfsAnagramClass> const& classes = class_list->classes();
  if (classes.empty() || classes.size() > UINT32_MAX ||
      classes.size() > SIZE_MAX / sizeof(FitClass) ||
      classes.size() > SIZE_MAX / sizeof(ScoreClass) ||
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

  FitClass* fit = static_cast<FitClass*>(
      allocate_aligned(classes.size() * sizeof(FitClass)));
  if (fit == NULL) return false;
  std::unique_ptr<FitClass, AlignedFree> new_fit(fit);

  ScoreClass* score = static_cast<ScoreClass*>(
      allocate_aligned(classes.size() * sizeof(ScoreClass)));
  if (score == NULL) return false;
  std::unique_ptr<ScoreClass, AlignedFree> new_score(score);

  uint64_t* score_deltas = static_cast<uint64_t*>(
      allocate_aligned(classes.size() * sizeof(uint64_t)));
  if (score_deltas == NULL) return false;
  std::unique_ptr<uint64_t, AlignedFree> new_score_deltas(score_deltas);

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
    uint64_t score_delta = 0;
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
      if (!checked_multiply_u64(count, score_multipliers[rank], &term) ||
          !checked_add_u64(score_delta, term, &next))
        return false;
      score_delta = next;
    }

    fit[ci].support_mask = support;
    fit[ci].letters_offset = uint32_t(offset);
    fit[ci].packed_length_and_count =
        uint32_t(source.key.size()) |
        (uint32_t(source.letters.size()) << 16) |
        (repeated << 24);
    score[ci].best_member_log_score = best_member_log_scores[ci];
    score[ci].bag_key_delta = delta;
    score_deltas[ci] = score_delta;
    offset = write;
  }

  fit_classes = std::move(new_fit);
  score_classes = std::move(new_score);
  score_key_deltas = std::move(new_score_deltas);
  packed_letters = std::move(new_packed);
  return true;
}

void DfsAnagramSearch::clear_score_bounds() {
  bound_mode = SCORE_BOUND_OFF;
  bound_values.reset();
  bound_float_values.reset();
  bound_capacity = 0;
  bound_value_bytes = 0;
  bound_complete = false;
  root_score_bound = HUGE_VAL;
  root_score_bound_ready = false;
  bound_entries = 0;
  bound_charged_bytes = 0;
}

void DfsAnagramSearch::prepare_score_bounds(
    uint64_t state_count, DfsSolutionSink* sink) {
  clear_score_bounds();
  active_candidate_cache_budget = candidate_cache_budget;
  if (!hot_classes_ready || sink == NULL ||
      !sink->supports_score_pruning() ||
      !score_bound_arithmetic_supported() ||
      candidate_cache_budget < CACHE_ALIGNMENT)
    return;

  // The root's forced rarest symbol is consumed by every first class. Put that
  // symbol in the most-significant score digit and omit the unreachable root
  // plane. Every phase-2-prunable descendant then has a key below this count.
  if (bag_mask == 0) return;
  int const root_rank = __builtin_ctzll(bag_mask);
  uint64_t const root_radix = uint64_t(bag[size_t(root_rank)]) + 1;
  uint64_t const effective_states =
      (state_count / root_radix) * (root_radix - 1);
  if (effective_states == 0) return;

  size_t double_bytes = 0;
  bool const double_size_ok = dense_bound_requirements(
      effective_states, sizeof(double), &double_bytes);
  if (double_size_ok && double_bytes <= candidate_cache_budget) {
    AtomicWord* values = static_cast<AtomicWord*>(
        allocate_aligned(double_bytes));
    if (values == NULL) return;
    for (size_t i = 0; i < size_t(effective_states); ++i) {
      new (&values[i]) AtomicWord;
      values[i].value.store(BOUND_UNSEEN, std::memory_order_relaxed);
    }
    bound_values.reset(values);
    bound_capacity = size_t(effective_states);
    bound_value_bytes = sizeof(double);
    bound_complete = true;
    bound_mode = SCORE_BOUND_DENSE;
    bound_charged_bytes = double_bytes;
  } else {
    size_t float_bytes = 0;
    bool const float_size_ok = dense_bound_requirements(
        effective_states, sizeof(float), &float_bytes);
    bool const complete_float =
        float_size_ok && float_bytes <= candidate_cache_budget;
    uint64_t const available_entries =
        candidate_cache_budget / sizeof(float);
    uint64_t const selected_entries = complete_float
        ? effective_states
        : std::min(effective_states, available_entries);
    if (selected_entries == 0 || selected_entries > SIZE_MAX) return;
    size_t const selected_bytes =
        complete_float
            ? float_bytes
            : size_t(selected_entries) * sizeof(float);
    AtomicFloatWord* values = static_cast<AtomicFloatWord*>(
        complete_float
            ? allocate_aligned(selected_bytes)
            : allocate_aligned_exact(selected_bytes));
    if (values == NULL) return;
    for (size_t i = 0; i < size_t(selected_entries); ++i) {
      new (&values[i]) AtomicFloatWord;
      values[i].value.store(
          FLOAT_BOUND_UNSEEN, std::memory_order_relaxed);
    }
    bound_float_values.reset(values);
    bound_capacity = size_t(selected_entries);
    bound_value_bytes = sizeof(float);
    bound_complete = complete_float;
    bound_mode = complete_float
        ? SCORE_BOUND_DENSE
        : SCORE_BOUND_PREFIX;
    bound_charged_bytes = selected_bytes;
  }

  active_candidate_cache_budget =
      candidate_cache_budget - bound_charged_bytes;
}

bool DfsAnagramSearch::load_score_bound(
    uint64_t key, double* value) const {
  if (bound_mode == SCORE_BOUND_OFF || key >= bound_capacity)
    return false;
  if (bound_value_bytes == sizeof(double)) {
    uint64_t const stored = bound_values.get()[size_t(key)].value.load(
        std::memory_order_relaxed);
    if (stored == BOUND_UNSEEN || stored == BOUND_COMPUTING) return false;
    *value = bits_to_double(stored);
    return true;
  }
  assert(bound_value_bytes == sizeof(float));
  uint32_t const stored =
      bound_float_values.get()[size_t(key)].value.load(
          std::memory_order_relaxed);
  if (stored == FLOAT_BOUND_UNSEEN ||
      stored == FLOAT_BOUND_COMPUTING)
    return false;
  *value = double(bits_to_float(stored));
  return true;
}

bool DfsAnagramSearch::store_score_bound(uint64_t key, double value) {
  if (bound_mode == SCORE_BOUND_OFF || key >= bound_capacity)
    return false;
  if (bound_value_bytes == sizeof(double)) {
    AtomicWord& stored = bound_values.get()[size_t(key)];
    uint64_t const previous =
        stored.value.load(std::memory_order_relaxed);
    if (previous == BOUND_UNSEEN) {
      ++bound_entries;
      ++bound_states_computed;
    }
    stored.value.store(double_to_bits(value), std::memory_order_relaxed);
    return true;
  }
  assert(bound_value_bytes == sizeof(float));
  AtomicFloatWord& stored =
      bound_float_values.get()[size_t(key)];
  uint32_t const previous =
      stored.value.load(std::memory_order_relaxed);
  if (previous == FLOAT_BOUND_UNSEEN) {
    ++bound_entries;
    ++bound_states_computed;
  }
  stored.value.store(
      float_to_bits(round_float_score_bound_up(value)),
      std::memory_order_relaxed);
  return true;
}

void DfsAnagramSearch::publish_parallel_score_bound(
    uint64_t key, double value) {
  assert(key < bound_capacity);
  if (bound_value_bytes == sizeof(double)) {
    bound_values.get()[size_t(key)].value.store(
        double_to_bits(value), std::memory_order_release);
  } else {
    assert(bound_value_bytes == sizeof(float));
    bound_float_values.get()[size_t(key)].value.store(
        float_to_bits(round_float_score_bound_up(value)),
        std::memory_order_release);
  }
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
  support_metadata.reset();
  support_keys.reset();
  support_candidate_ids.reset();
  support_capacity = 0;
  support_max_entries = 0;
  support_filled = 0;
  support_candidate_capacity = 0;
  support_candidate_used = 0;
  support_admitted_entries = 0;
  support_charged_bytes = 0;
  parallel_support_exhausted.store(false, std::memory_order_relaxed);
}

void DfsAnagramSearch::prepare_support_cache(size_t* remaining_budget) {
  size_t const budget =
      *remaining_budget / SUPPORT_CACHE_BUDGET_DIVISOR;
  if (budget < 3 * CACHE_ALIGNMENT) return;

  size_t capacity = largest_power_of_two(
      (budget / SUPPORT_CACHE_METADATA_DIVISOR) /
      (2 * sizeof(uint64_t)));
  size_t const distinct_symbols = size_t(__builtin_popcountll(bag_mask));
  if (distinct_symbols < sizeof(size_t) * CHAR_BIT - 1) {
    size_t const possible_masks = size_t(1) << distinct_symbols;
    size_t const desired =
        possible_masks <= SIZE_MAX / 2 ? possible_masks * 2 : 0;
    if (desired != 0) capacity = std::min(capacity, desired);
  }
  if (capacity < 2) return;

  size_t array_bytes;
  if (!round_up_alignment(capacity * sizeof(uint64_t), &array_bytes) ||
      array_bytes > budget / 2)
    return;
  size_t const metadata_bytes = array_bytes * 2;
  size_t arena_bytes =
      (budget - metadata_bytes) & ~(CACHE_ALIGNMENT - 1);
  size_t const max_candidate_ids = std::min(
      size_t(UINT32_MAX - 1), SIZE_MAX / sizeof(uint32_t));
  arena_bytes = std::min(
      arena_bytes, max_candidate_ids * sizeof(uint32_t));
  if (arena_bytes == 0) return;

  AtomicWord* keys = static_cast<AtomicWord*>(
      allocate_aligned(array_bytes));
  if (keys == NULL) return;
  std::unique_ptr<AtomicWord, AlignedFree> new_keys(keys);
  uint64_t* metadata = static_cast<uint64_t*>(
      allocate_aligned(array_bytes));
  if (metadata == NULL) return;
  std::unique_ptr<uint64_t, AlignedFree> new_metadata(metadata);
  uint32_t* ids = static_cast<uint32_t*>(
      allocate_aligned(arena_bytes));
  if (ids == NULL) return;

  for (size_t i = 0; i < capacity; ++i) {
    new (&keys[i]) AtomicWord;
    keys[i].value.store(UINT64_MAX, std::memory_order_relaxed);
  }
  support_keys = std::move(new_keys);
  support_metadata = std::move(new_metadata);
  support_candidate_ids.reset(ids);
  support_capacity = capacity;
  support_max_entries = capacity / 2;
  support_candidate_capacity = arena_bytes / sizeof(uint32_t);
  support_charged_bytes = metadata_bytes;
  *remaining_budget -= metadata_bytes + arena_bytes;
}

void DfsAnagramSearch::prepare_cache(uint64_t state_count) {
  clear_cache();
  if (!candidate_cache_enabled || !hot_classes_ready ||
      active_candidate_cache_budget < CACHE_ALIGNMENT)
    return;

  size_t cache_budget = active_candidate_cache_budget;
  prepare_support_cache(&cache_budget);
  if (cache_budget < CACHE_ALIGNMENT) {
    clear_cache();
    return;
  }

  size_t dense_bytes = 0;
  bool const dense_size_ok =
      state_count <= SIZE_MAX / sizeof(uint64_t) &&
      round_up_alignment(size_t(state_count) * sizeof(uint64_t),
                         &dense_bytes);
  if (dense_size_ok &&
      dense_bytes <= cache_budget / 2) {
    uint64_t* metadata = static_cast<uint64_t*>(
        allocate_aligned(dense_bytes));
    if (metadata == NULL) return;
    std::fill(metadata, metadata + size_t(state_count), CACHE_UNSEEN);
    cache_metadata.reset(metadata);
    cache_capacity = size_t(state_count);
    cache_mode = CANDIDATE_CACHE_DENSE;
    charged_bytes = dense_bytes;
  } else {
    size_t const metadata_share = cache_budget / 2;
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
        array_bytes > cache_budget / 2)
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

  size_t remaining = cache_budget - charged_bytes;
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

}

bool DfsAnagramSearch::run(DfsSolutionSink* sink, FILE* progress,
                           int progress_factor,
                           bool allow_cache_fallback) {
  typedef std::chrono::steady_clock PhaseClock;
  PhaseClock::time_point const setup_start = PhaseClock::now();
  bound_states_computed = 0;
  bound_transitions = 0;
  bound_nextafter_calls = 0;
  setup_seconds = 0.0;
  search_seconds = 0.0;
  actual_preprocess_threads = 1;
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
  if (encodable) {
    uint64_t score_state_count = 1;
    for (int rank = DFS_SYMBOL_COUNT - 1; rank >= 0; --rank) {
      score_multipliers[size_t(rank)] = score_state_count;
      uint64_t const radix = uint64_t(bag[size_t(rank)]) + 1;
      if (!checked_multiply_u64(
              score_state_count, radix, &score_state_count)) {
        encodable = false;
        break;
      }
    }
    assert(!encodable || score_state_count == state_count);
  }

  bag_mask = 0;
  for (int rank = 0; rank < DFS_SYMBOL_COUNT; ++rank)
    if (bag[size_t(rank)] != 0)
      bag_mask |= UINT64_C(1) << rank;
  current_bag_key = encodable ? state_count - 1 : 0;
  current_score_key = encodable ? state_count - 1 : 0;
  current_letters_left = letters.size();
  hot_classes_ready = encodable && prepare_hot_classes();
  uint64_t effective_states = 0;
  size_t double_bytes = 0;
  size_t float_bytes = 0;
  bool dense_sizes_available = false;
  if (encodable && bag_mask != 0) {
    int const root_rank = __builtin_ctzll(bag_mask);
    uint64_t const root_radix =
        uint64_t(bag[size_t(root_rank)]) + 1;
    effective_states =
        (state_count / root_radix) * (root_radix - 1);
    bool const double_ok = dense_bound_requirements(
        effective_states, sizeof(double), &double_bytes);
    bool const float_ok = dense_bound_requirements(
        effective_states, sizeof(float), &float_bytes);
    dense_sizes_available = double_ok && float_ok;
  }
  if (progress != NULL) {
    if (!encodable) {
      fputs("# phase 2 preflight: theoretical state count exceeds the "
            "supported range; dense score-table size and minimum -C "
            "unavailable\n", progress);
    } else if (bag_mask != 0 && dense_sizes_available) {
      size_t const minimum_mib =
          float_bytes / MIB + size_t(float_bytes % MIB != 0);
      fprintf(progress,
              "# phase 2 preflight: %llu theoretical states, "
              "%llu effective non-root states\n"
              "# phase 2 preflight: "
              "%zu double/%zu float dense score-table bytes, "
              "minimum -C %zu MiB (%zu bytes)\n",
              (unsigned long long) state_count,
              (unsigned long long) effective_states,
              double_bytes, float_bytes, minimum_mib, float_bytes);
    } else if (bag_mask != 0) {
      fprintf(progress,
              "# phase 2 preflight: %llu theoretical states, "
              "%llu effective non-root states\n"
              "# phase 2 preflight: dense score-table size and minimum -C "
              "exceed the "
              "supported range\n",
              (unsigned long long) state_count,
              (unsigned long long) effective_states);
    } else {
      fputs("# phase 2 preflight: empty bag; no score table needed\n",
            progress);
    }
    fflush(progress);
  }
  bool const score_bounds_applicable =
      hot_classes_ready && sink != NULL &&
      sink->supports_score_pruning() &&
      score_bound_arithmetic_supported() &&
      bag_mask != 0;
  if (!allow_cache_fallback && score_bounds_applicable) {
    if (!dense_sizes_available) {
      if (progress != NULL) {
        fputs("error: dense score table size exceeds the supported range\n"
              "       use --allow-cache-fallback\n", progress);
        fflush(progress);
      }
      return false;
    }
    if (float_bytes > candidate_cache_budget) {
      size_t const required_mib =
          float_bytes / MIB + size_t(float_bytes % MIB != 0);
      size_t const supplied_mib = candidate_cache_budget / MIB;
      if (progress != NULL) {
        fprintf(progress,
                "error: dense score table requires at least %zu MiB; "
                "supplied cache is %zu MiB\n"
                "       use -C %zu or --allow-cache-fallback\n",
                required_mib, supplied_mib, required_mib);
        fflush(progress);
      }
      return false;
    }
  }
  prepare_score_bounds(state_count, sink);
  prepare_cache(state_count);
  if (progress != NULL) {
    fprintf(progress, "# phase 2 preflight: score-bound mode %s",
            score_bound_mode_name(bound_mode));
    if (bound_mode != SCORE_BOUND_OFF)
      fprintf(progress, " (%zu-byte values, capacity %zu, %s coverage)",
              bound_value_bytes, bound_capacity,
              bound_complete ? "complete effective" : "partial");
    fprintf(progress, "; candidate-cache mode %s\n",
            candidate_cache_mode_name(cache_mode));
    fflush(progress);
  }
  if (bound_mode != SCORE_BOUND_OFF && bound_complete) {
    bool const ran_parallel =
        compute_score_bound_parallel(requested_preprocess_threads);
    if (!ran_parallel && cache_mode == CANDIDATE_CACHE_DENSE)
      root_score_bound = compute_score_bound<WALK_DENSE>();
    else if (!ran_parallel && cache_mode == CANDIDATE_CACHE_SPARSE)
      root_score_bound = compute_score_bound<WALK_SPARSE>();
    else if (!ran_parallel)
      root_score_bound = compute_score_bound<WALK_UNCACHED>();
    if (!ran_parallel) root_score_bound_ready = true;
  }

  path.clear();
  path.reserve(letters.size());
  progress_stream = progress;
  progress_interval =
      int64_t(100000) * int64_t(std::max(progress_factor, 1));
  next_progress = progress_interval;
  nodes = 0;
  solutions = 0;
  bound_prunes = 0;

  PhaseClock::time_point const search_start = PhaseClock::now();
  setup_seconds =
      std::chrono::duration<double>(search_start - setup_start).count();
  if (progress_stream != NULL) {
    fprintf(progress_stream,
            "# phase 2: precomputed %zu bounded states in %.3fs\n",
            bound_states_computed, setup_seconds);
    if (bound_mode == SCORE_BOUND_PREFIX)
      fprintf(progress_stream,
              "# phase 2: dense-prefix bounds will be constructed lazily "
              "during search for score keys below %zu once a score floor "
              "is available\n",
              bound_capacity);
    if (actual_preprocess_threads > 1)
      fprintf(progress_stream,
              "# phase 2: preprocessing used %zu threads\n",
              actual_preprocess_threads);
    fflush(progress_stream);
  }
  if (!hot_classes_ready) {
    walk_unoptimized(letters.size(), 0, 0, 0.0, sink);
  } else if (cache_mode == CANDIDATE_CACHE_DENSE) {
    walk<WALK_DENSE>(letters.size(), 0, 0.0, sink);
  } else if (cache_mode == CANDIDATE_CACHE_SPARSE) {
    walk<WALK_SPARSE>(letters.size(), 0, 0.0, sink);
  } else {
    walk<WALK_UNCACHED>(letters.size(), 0, 0.0, sink);
  }
  search_seconds =
      std::chrono::duration<double>(PhaseClock::now() - search_start).count();
  return true;
}

bool DfsAnagramSearch::hot_class_fits(uint32_t class_index) const {
  FitClass const& candidate = fit_classes.get()[class_index];
  if ((candidate.support_mask & ~bag_mask) != 0) return false;
  return hot_class_multiplicity_fits(class_index);
}

bool DfsAnagramSearch::hot_class_multiplicity_fits(
    uint32_t class_index) const {
  FitClass const& candidate = fit_classes.get()[class_index];
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

bool DfsAnagramSearch::hot_class_fits(
    uint32_t class_index, BoundWorker const& worker) const {
  FitClass const& candidate = fit_classes.get()[class_index];
  if ((candidate.support_mask & ~worker.bag_mask) != 0) return false;
  return hot_class_multiplicity_fits(class_index, worker);
}

bool DfsAnagramSearch::hot_class_multiplicity_fits(
    uint32_t class_index, BoundWorker const& worker) const {
  FitClass const& candidate = fit_classes.get()[class_index];
  uint32_t const* requirements =
      packed_letters.get() + candidate.letters_offset;
  uint32_t const repeated =
      hot_repeated_count(candidate.packed_length_and_count);
  for (uint32_t i = 0; i < repeated; ++i) {
    uint32_t const requirement = requirements[i];
    if (worker.bag[packed_rank(requirement)] <
        packed_count(requirement))
      return false;
  }
  return true;
}

size_t DfsAnagramSearch::first_length_candidate(
    size_t begin, size_t end, size_t letters_left) const {
  while (begin < end) {
    size_t const middle = begin + (end - begin) / 2;
    size_t const candidate_length = hot_letter_length(
        fit_classes.get()[middle].packed_length_and_count);
    if (candidate_length > letters_left)
      begin = middle + 1;
    else
      end = middle;
  }
  return begin;
}

uint32_t const* DfsAnagramSearch::first_length_support_candidate(
    uint32_t const* begin, uint32_t const* end,
    size_t letters_left) const {
  while (begin < end) {
    uint32_t const* middle = begin + (end - begin) / 2;
    size_t const candidate_length = hot_letter_length(
        fit_classes.get()[*middle].packed_length_and_count);
    if (candidate_length > letters_left)
      begin = middle + 1;
    else
      end = middle;
  }
  return begin;
}

uint64_t DfsAnagramSearch::support_lookup(
    uint64_t key, size_t* slot, bool* may_insert) const {
  if (support_capacity == 0) {
    *slot = 0;
    *may_insert = false;
    return CACHE_BYPASSED;
  }
  size_t const mask = support_capacity - 1;
  size_t position = size_t(mix_key(key)) & mask;
  for (;;) {
    uint64_t const stored_key =
        support_keys.get()[position].value.load(std::memory_order_acquire);
    if (stored_key == key) {
      *slot = position;
      *may_insert = false;
      return support_metadata.get()[position];
    }
    if (stored_key == UINT64_MAX) {
      *slot = position;
      *may_insert =
          support_filled.load(std::memory_order_relaxed) <
          support_max_entries;
      return CACHE_UNSEEN;
    }
    position = (position + 1) & mask;
  }
}

void DfsAnagramSearch::publish_support(
    size_t slot, uint64_t key, uint64_t metadata) {
  support_metadata.get()[slot] = metadata;
  support_keys.get()[slot].value.store(key, std::memory_order_release);
  support_filled.fetch_add(1, std::memory_order_relaxed);
}

bool DfsAnagramSearch::build_support_entry(
    size_t begin, size_t end, uint64_t candidate_mask,
    uint64_t* metadata) {
  size_t write = support_candidate_used;
  // The bucket and its support-filtered subsequence are both in ascending
  // global class order. This also preserves descending class-length order.
  for (size_t class_index = begin; class_index < end; ++class_index) {
    FitClass const& candidate = fit_classes.get()[class_index];
    if ((candidate.support_mask & ~candidate_mask) != 0) continue;
    if (write == support_candidate_capacity) {
      *metadata = CACHE_BYPASSED;
      return false;
    }
    support_candidate_ids.get()[write++] = uint32_t(class_index);
  }

  uint32_t const offset = uint32_t(support_candidate_used);
  uint32_t const count = uint32_t(write - support_candidate_used);
  support_candidate_used += count;
  support_charged_bytes += size_t(count) * sizeof(uint32_t);
  ++support_admitted_entries;
  *metadata = pack_entry(offset, count);
  return true;
}

uint64_t DfsAnagramSearch::parallel_support_entry(
    BoundWorker const& worker, size_t end) {
  size_t slot = 0;
  bool may_insert = false;
  uint64_t metadata = support_lookup(
      worker.bag_mask, &slot, &may_insert);
  if (metadata != CACHE_UNSEEN) return metadata;
  if (parallel_support_exhausted.load(std::memory_order_acquire))
    return CACHE_BYPASSED;

  std::lock_guard<std::mutex> const lock(support_build_mutex);
  metadata = support_lookup(worker.bag_mask, &slot, &may_insert);
  if (metadata != CACHE_UNSEEN) return metadata;
  if (!may_insert) {
    parallel_support_exhausted.store(true, std::memory_order_release);
    return CACHE_BYPASSED;
  }

  int const rank = __builtin_ctzll(worker.bag_mask);
  int const forced_symbol =
      class_list->rank_to_symbol()[size_t(rank)];
  size_t const forced_begin =
      class_list->candidate_begin(forced_symbol);
  bool const admitted = build_support_entry(
      forced_begin, end, worker.bag_mask, &metadata);
  publish_support(slot, worker.bag_mask, metadata);
  if (!admitted)
    parallel_support_exhausted.store(true, std::memory_order_release);
  return metadata;
}

bool DfsAnagramSearch::build_candidate_entry(
    size_t begin, size_t end, uint64_t* metadata) {
  size_t write = candidate_used;

  size_t support_slot = 0;
  bool support_may_insert = false;
  uint64_t support_entry = support_lookup(
      bag_mask, &support_slot, &support_may_insert);
  if (support_entry == CACHE_UNSEEN) {
    if (support_may_insert) {
      size_t const forced_begin =
          class_list->candidate_begin(
              class_list->rank_to_symbol()[
                  size_t(__builtin_ctzll(bag_mask))]);
      build_support_entry(
          forced_begin, end, bag_mask, &support_entry);
      publish_support(
          support_slot, bag_mask, support_entry);
    } else {
      support_entry = CACHE_BYPASSED;
    }
  }

  if (support_entry != CACHE_BYPASSED) {
    uint32_t const* first =
        support_candidate_ids.get() + entry_offset(support_entry);
    uint32_t const* last = first + entry_count(support_entry);
    first = first_length_support_candidate(
        first, last, current_letters_left);
    for (; first != last; ++first) {
      uint32_t const id = *first;
      // Presence was proved when the support entry was built. Only repeated
      // letter requirements can distinguish bags sharing this mask.
      if (!hot_class_multiplicity_fits(id)) continue;
      if (write == candidate_capacity) {
        *metadata = CACHE_BYPASSED;
        return false;
      }
      candidate_ids.get()[write++] = id;
    }
  } else {
    for (size_t class_index = begin; class_index < end; ++class_index) {
      uint32_t const id = uint32_t(class_index);
      if (!hot_class_fits(id)) continue;
      if (write == candidate_capacity) {
        *metadata = CACHE_BYPASSED;
        return false;
      }
      candidate_ids.get()[write++] = id;
    }
  }

  uint32_t const offset = uint32_t(candidate_used);
  uint32_t const count = uint32_t(write - candidate_used);
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
void DfsAnagramSearch::consider_bound_candidate(
    uint32_t class_index, double* best, double* max_rounding_error) {
  FitClass const& fit = fit_classes.get()[class_index];
  ScoreClass const& score = score_classes.get()[class_index];
  uint32_t const* requirements =
      packed_letters.get() + fit.letters_offset;
  uint32_t const requirement_count =
      hot_requirement_count(fit.packed_length_and_count);
  size_t const candidate_length =
      hot_letter_length(fit.packed_length_and_count);
  uint64_t const parent_bag_mask = bag_mask;
  for (uint32_t i = 0; i < requirement_count; ++i) {
    uint32_t const requirement = requirements[i];
    uint32_t const requirement_rank = packed_rank(requirement);
    uint32_t& remaining = bag[requirement_rank];
    remaining -= packed_count(requirement);
    bag_mask &= ~(uint64_t(remaining == 0) << requirement_rank);
  }
  current_bag_key -= score.bag_key_delta;
  current_score_key -= score_key_deltas.get()[class_index];
  current_letters_left -= candidate_length;
  double const child = compute_score_bound<mode>();
  current_letters_left += candidate_length;
  current_score_key += score_key_deltas.get()[class_index];
  current_bag_key += score.bag_key_delta;
  for (uint32_t i = 0; i < requirement_count; ++i) {
    uint32_t const requirement = requirements[i];
    bag[packed_rank(requirement)] += packed_count(requirement);
  }
  bag_mask = parent_bag_mask;

  if (child == -HUGE_VAL) return;
  ++bound_transitions;
  double const partial =
      score.best_member_log_score + restart_log_rate;
  double const candidate_bound = partial + child;
  *best = std::max(*best, candidate_bound);
  *max_rounding_error = std::max(
      *max_rounding_error,
      score_candidate_rounding_error(
          score.best_member_log_score, restart_log_rate, child));
}

template<DfsAnagramSearch::WalkMode mode>
double DfsAnagramSearch::compute_score_bound() {
  double cached;
  if (load_score_bound(current_score_key, &cached)) return cached;
  if (bag_mask == 0) {
    if (!store_score_bound(current_score_key, 0.0))
      return HUGE_VAL;
    return 0.0;
  }

  int const rank = __builtin_ctzll(bag_mask);
  int const forced_symbol =
      class_list->rank_to_symbol()[size_t(rank)];
  size_t const begin = first_length_candidate(
      class_list->candidate_begin(forced_symbol),
      class_list->candidate_end(forced_symbol),
      current_letters_left);
  size_t const end = class_list->candidate_end(forced_symbol);

  uint64_t metadata = CACHE_BYPASSED;
  size_t sparse_slot = 0;
  bool sparse_may_insert = false;
  if (mode == WALK_DENSE) {
    metadata = cache_metadata.get()[size_t(current_bag_key)];
  } else if (mode == WALK_SPARSE) {
    metadata = sparse_lookup(
        current_bag_key, &sparse_slot, &sparse_may_insert);
  }

  if (mode == WALK_DENSE && metadata == CACHE_UNSEEN) {
    build_candidate_entry(begin, end, &metadata);
    publish_dense(current_bag_key, metadata);
  } else if (mode == WALK_SPARSE && metadata == CACHE_UNSEEN) {
    if (sparse_may_insert) {
      build_candidate_entry(begin, end, &metadata);
      publish_sparse(sparse_slot, current_bag_key, metadata);
    } else {
      metadata = CACHE_BYPASSED;
    }
  }

  double best = -HUGE_VAL;
  double max_rounding_error = 0.0;
  if (mode != WALK_UNCACHED && metadata != CACHE_BYPASSED) {
    uint32_t const count = entry_count(metadata);
    uint32_t const* first =
        candidate_ids.get() + entry_offset(metadata);
    uint32_t const* last = first + count;
    for (; first != last; ++first)
      consider_bound_candidate<mode>(
          *first, &best, &max_rounding_error);
  } else {
    for (size_t class_index = begin;
         class_index < end; ++class_index) {
      uint32_t const id = uint32_t(class_index);
      if (!hot_class_fits(id)) continue;
      consider_bound_candidate<mode>(
          id, &best, &max_rounding_error);
    }
  }

  // For every edge i, exact_i <= computed_i + error_i. Therefore
  // max(computed_i) + max(error_i) bounds even an unselected edge whose
  // computed value rounded down. Do this inflation and the long-double work
  // once per state, after selecting the computed maximum.
  double const stored_best =
      best == -HUGE_VAL
          ? -HUGE_VAL
          : round_score_bound_up(
                static_cast<long double>(best) +
                static_cast<long double>(max_rounding_error),
                &bound_nextafter_calls);
  if (current_score_key < bound_capacity) {
    if (!store_score_bound(current_score_key, stored_best))
      return HUGE_VAL;
  } else {
    // Complete-effective preprocessing starts at the sole omitted state: the
    // root. Every recursive child consumes its most-significant rarest digit
    // and is inside the table.
    assert(bound_complete && current_score_key == current_bag_key);
  }
  return stored_best;
}

void DfsAnagramSearch::consider_parallel_bound_candidate(
    uint32_t class_index, BoundWorker* worker, double* best,
    double* max_rounding_error) {
  FitClass const& fit = fit_classes.get()[class_index];
  ScoreClass const& score = score_classes.get()[class_index];
  uint32_t const* requirements =
      packed_letters.get() + fit.letters_offset;
  uint32_t const requirement_count =
      hot_requirement_count(fit.packed_length_and_count);
  size_t const candidate_length =
      hot_letter_length(fit.packed_length_and_count);
  uint64_t const parent_bag_mask = worker->bag_mask;
  for (uint32_t i = 0; i < requirement_count; ++i) {
    uint32_t const requirement = requirements[i];
    uint32_t const requirement_rank = packed_rank(requirement);
    uint32_t& remaining = worker->bag[requirement_rank];
    remaining -= packed_count(requirement);
    worker->bag_mask &=
        ~(uint64_t(remaining == 0) << requirement_rank);
  }
  worker->score_key -= score_key_deltas.get()[class_index];
  worker->letters_left -= candidate_length;
  double const child = compute_parallel_score_bound(worker);
  worker->letters_left += candidate_length;
  worker->score_key += score_key_deltas.get()[class_index];
  for (uint32_t i = 0; i < requirement_count; ++i) {
    uint32_t const requirement = requirements[i];
    worker->bag[packed_rank(requirement)] += packed_count(requirement);
  }
  worker->bag_mask = parent_bag_mask;

  if (child == -HUGE_VAL) return;
  ++worker->transitions;
  double const partial =
      score.best_member_log_score + restart_log_rate;
  double const candidate_bound = partial + child;
  *best = std::max(*best, candidate_bound);
  *max_rounding_error = std::max(
      *max_rounding_error,
      score_candidate_rounding_error(
          score.best_member_log_score, restart_log_rate, child));
}

double DfsAnagramSearch::compute_parallel_score_bound(
    BoundWorker* worker) {
  assert(bound_mode == SCORE_BOUND_DENSE);
  assert(worker->score_key < bound_capacity);
  unsigned int wait_spins = 0;
  if (bound_value_bytes == sizeof(double)) {
    AtomicWord& slot =
        bound_values.get()[size_t(worker->score_key)];
    uint64_t stored = slot.value.load(std::memory_order_acquire);
    for (;;) {
      if (stored != BOUND_UNSEEN && stored != BOUND_COMPUTING)
        return bits_to_double(stored);
      if (stored == BOUND_UNSEEN &&
          slot.value.compare_exchange_weak(
              stored, BOUND_COMPUTING,
              std::memory_order_acq_rel,
              std::memory_order_acquire))
        break;
      if (stored == BOUND_COMPUTING) {
        bound_wait_backoff(&wait_spins);
        stored = slot.value.load(std::memory_order_acquire);
      }
    }
  } else {
    assert(bound_value_bytes == sizeof(float));
    AtomicFloatWord& slot =
        bound_float_values.get()[size_t(worker->score_key)];
    uint32_t stored = slot.value.load(std::memory_order_acquire);
    for (;;) {
      if (stored != FLOAT_BOUND_UNSEEN &&
          stored != FLOAT_BOUND_COMPUTING)
        return double(bits_to_float(stored));
      if (stored == FLOAT_BOUND_UNSEEN &&
          slot.value.compare_exchange_weak(
              stored, FLOAT_BOUND_COMPUTING,
              std::memory_order_acq_rel,
              std::memory_order_acquire))
        break;
      if (stored == FLOAT_BOUND_COMPUTING) {
        bound_wait_backoff(&wait_spins);
        stored = slot.value.load(std::memory_order_acquire);
      }
    }
  }

  if (worker->bag_mask == 0) {
    publish_parallel_score_bound(worker->score_key, 0.0);
    ++worker->states_computed;
    return 0.0;
  }

  int const rank = __builtin_ctzll(worker->bag_mask);
  int const forced_symbol =
      class_list->rank_to_symbol()[size_t(rank)];
  size_t const begin = first_length_candidate(
      class_list->candidate_begin(forced_symbol),
      class_list->candidate_end(forced_symbol),
      worker->letters_left);
  size_t const end = class_list->candidate_end(forced_symbol);
  uint64_t const support_entry =
      parallel_support_entry(*worker, end);

  double best = -HUGE_VAL;
  double max_rounding_error = 0.0;
  if (support_entry != CACHE_BYPASSED) {
    uint32_t const* first =
        support_candidate_ids.get() + entry_offset(support_entry);
    uint32_t const* last = first + entry_count(support_entry);
    first = first_length_support_candidate(
        first, last, worker->letters_left);
    for (; first != last; ++first) {
      uint32_t const id = *first;
      if (!hot_class_multiplicity_fits(id, *worker)) continue;
      consider_parallel_bound_candidate(
          id, worker, &best, &max_rounding_error);
    }
  } else {
    for (size_t class_index = begin; class_index < end; ++class_index) {
      uint32_t const id = uint32_t(class_index);
      if (!hot_class_fits(id, *worker)) continue;
      consider_parallel_bound_candidate(
          id, worker, &best, &max_rounding_error);
    }
  }

  double const result =
      best == -HUGE_VAL
          ? -HUGE_VAL
          : round_score_bound_up(
                static_cast<long double>(best) +
                static_cast<long double>(max_rounding_error),
                &worker->nextafter_calls);
  publish_parallel_score_bound(worker->score_key, result);
  ++worker->states_computed;
  return result;
}

bool DfsAnagramSearch::compute_score_bound_parallel(
    size_t requested_threads) {
  if (bound_mode != SCORE_BOUND_DENSE || requested_threads < 2 ||
      !bound_complete || bound_capacity == 0)
    return false;
  if (bound_value_bytes == sizeof(double)) {
    if (!bound_values.get()[0].value.is_lock_free()) return false;
  } else if (!bound_float_values.get()[0].value.is_lock_free()) {
    return false;
  }

  BoundWorker root;
  root.bag = bag;
  root.bag_mask = bag_mask;
  root.score_key = current_score_key;
  root.letters_left = current_letters_left;
  root.states_computed = 0;
  root.transitions = 0;
  root.nextafter_calls = 0;
  root.best = -HUGE_VAL;
  root.max_rounding_error = 0.0;

  int const rank = __builtin_ctzll(root.bag_mask);
  int const forced_symbol =
      class_list->rank_to_symbol()[size_t(rank)];
  size_t const begin = first_length_candidate(
      class_list->candidate_begin(forced_symbol),
      class_list->candidate_end(forced_symbol),
      root.letters_left);
  size_t const end = class_list->candidate_end(forced_symbol);
  std::vector<uint32_t> root_candidates;
  for (size_t class_index = begin; class_index < end; ++class_index) {
    uint32_t const id = uint32_t(class_index);
    if (hot_class_fits(id, root))
      root_candidates.push_back(id);
  }
  if (root_candidates.size() < 2) return false;

  size_t const worker_count = std::min(
      requested_threads, root_candidates.size());
  std::vector<BoundWorker> workers(worker_count, root);
  std::vector<std::thread> background;
  try {
    background.reserve(worker_count - 1);
  } catch (...) {
    return false;
  }

  std::atomic<size_t> next_candidate(0);
  auto work = [&](size_t worker_index) {
    BoundWorker* worker = &workers[worker_index];
    for (;;) {
      size_t const index =
          next_candidate.fetch_add(1, std::memory_order_relaxed);
      if (index >= root_candidates.size()) break;
      consider_parallel_bound_candidate(
          root_candidates[index], worker, &worker->best,
          &worker->max_rounding_error);
    }
  };

  for (size_t i = 1; i < worker_count; ++i) {
    try {
      background.emplace_back(work, i);
    } catch (...) {
      break;
    }
  }
  work(0);
  for (size_t i = 0; i < background.size(); ++i)
    background[i].join();

  size_t states = 0;
  uint64_t transitions = 0;
  uint64_t nextafter_calls = 0;
  double best = -HUGE_VAL;
  double max_rounding_error = 0.0;
  size_t const active_workers = background.size() + 1;
  for (size_t i = 0; i < active_workers; ++i) {
    states += workers[i].states_computed;
    transitions += workers[i].transitions;
    nextafter_calls += workers[i].nextafter_calls;
    best = std::max(best, workers[i].best);
    max_rounding_error = std::max(
        max_rounding_error, workers[i].max_rounding_error);
  }

  double const result =
      best == -HUGE_VAL
          ? -HUGE_VAL
          : round_score_bound_up(
                static_cast<long double>(best) +
                static_cast<long double>(max_rounding_error),
                &nextafter_calls);
  root_score_bound = result;
  root_score_bound_ready = true;
  bound_entries = states;
  bound_states_computed = states;
  bound_transitions = transitions;
  bound_nextafter_calls = nextafter_calls;
  actual_preprocess_threads = active_workers;
  return true;
}

template<DfsAnagramSearch::WalkMode mode>
bool DfsAnagramSearch::should_prune(
    double representative_log_score, DfsSolutionSink* sink) {
  if (bound_mode == SCORE_BOUND_OFF) return false;
  // H charges a restart to every class it adds. That is exact below the first
  // chosen class, but the root's first class does not pay a restart.
  if (path.empty())
    return root_score_bound_ready && root_score_bound == -HUGE_VAL;

  double floor;
  if (sink == NULL || !sink->score_floor(&floor)) return false;
  if (current_score_key >= bound_capacity) return false;

  double remaining_bound;
  if (!load_score_bound(current_score_key, &remaining_bound)) {
    if (bound_mode != SCORE_BOUND_PREFIX) return false;
    // A rarest-most-significant prefix is dependency-closed: every fitting
    // subtraction decreases the key. Build its complete bound only after
    // phase 2 both enters the prefix and has a useful score floor.
    compute_score_bound<mode>();
    if (!load_score_bound(current_score_key, &remaining_bound))
      return false;
  }
  if (remaining_bound == -HUGE_VAL) return true;
  long double const upper =
      static_cast<long double>(representative_log_score) +
      static_cast<long double>(remaining_bound);
  long double const magnitude =
      fabsl(static_cast<long double>(representative_log_score)) +
      fabsl(static_cast<long double>(remaining_bound)) +
      fabsl(static_cast<long double>(floor)) + 1.0L;
  long double const padding =
      magnitude * static_cast<long double>(DBL_EPSILON) *
      (static_cast<long double>(max_depth) + 2.0L) * 16.0L;
  return upper + padding <= static_cast<long double>(floor);
}

template<DfsAnagramSearch::WalkMode mode>
void DfsAnagramSearch::visit_fitting_class(
    uint32_t class_index, size_t letters_left,
    double representative_log_score, DfsSolutionSink* sink) {
  FitClass const& fit = fit_classes.get()[class_index];
  ScoreClass const& score = score_classes.get()[class_index];
  size_t const candidate_length =
      hot_letter_length(fit.packed_length_and_count);
  assert(candidate_length <= letters_left);
  size_t const next_letters_left = letters_left - candidate_length;
  bool const first_class = path.empty();

  path.push_back(class_index);
  if (DFS_UNLIKELY(next_letters_left == 0)) {
    double const next_log_score =
        first_class
            ? score.best_member_log_score
            : representative_log_score + restart_log_rate +
                  score.best_member_log_score;
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
          ? score.best_member_log_score
          : representative_log_score + restart_log_rate +
                score.best_member_log_score;
  uint32_t const* requirements =
      packed_letters.get() + fit.letters_offset;
  uint32_t const requirement_count =
      hot_requirement_count(fit.packed_length_and_count);
  uint64_t const parent_bag_mask = bag_mask;
  for (uint32_t i = 0; i < requirement_count; ++i) {
    uint32_t const requirement = requirements[i];
    uint32_t const requirement_rank = packed_rank(requirement);
    uint32_t& remaining = bag[requirement_rank];
    remaining -= packed_count(requirement);
    bag_mask &= ~(uint64_t(remaining == 0) << requirement_rank);
  }
  current_bag_key -= score.bag_key_delta;
  current_score_key -= score_key_deltas.get()[class_index];
  walk<mode>(next_letters_left, class_index, next_log_score, sink);
  current_score_key += score_key_deltas.get()[class_index];
  current_bag_key += score.bag_key_delta;
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
  if (DFS_UNLIKELY(should_prune<mode>(
          representative_log_score, sink))) {
    ++bound_prunes;
    return;
  }

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
  size_t const begin = first_length_candidate(
      class_list->candidate_begin(forced_symbol),
      class_list->candidate_end(forced_symbol),
      letters_left);
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
