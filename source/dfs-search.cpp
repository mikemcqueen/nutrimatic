#include "dfs-search.h"

#include "dfs-diagnostic.h"
#include "dfs-search-internal.h"

#include <assert.h>
#include <errno.h>
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

#include <immintrin.h>

namespace {

size_t const MIB = size_t(1024) * size_t(1024);

static size_t derived_max_depth(DfsClassList const* classes,
                                size_t letter_count) {
  assert(classes != NULL);
  return letter_count / size_t(classes->min_word_length());
}

static bool dense_bound_requirements(
    uint64_t state_count, size_t value_bytes, size_t* dense_bytes) {
  if (state_count > SIZE_MAX / value_bytes ||
      !dfs_round_up_alignment(
          size_t(state_count) * value_bytes, dense_bytes))
    return false;
  return true;
}

}  // namespace

uint32_t float_to_bits(float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

float bits_to_float(uint32_t bits) {
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

float round_float_score_bound_up(double value) {
  if (value == -HUGE_VAL) return -HUGE_VALF;
  if (value == HUGE_VAL) return HUGE_VALF;
  float rounded = float(value);
  if (isinf(rounded) && rounded < 0.0f)
    return -FLT_MAX;
  if (double(rounded) < value)
    rounded = nextafterf(rounded, HUGE_VALF);
  return rounded;
}

void bound_wait_backoff(unsigned int* spins) {
#if defined(__i386__) || defined(__x86_64__)
  _mm_pause();
#else
  std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
  ++*spins;
  if ((*spins & 255U) == 0)
    std::this_thread::yield();
}

uint32_t packed_rank(uint32_t requirement) {
  return requirement & 63U;
}

uint32_t packed_count(uint32_t requirement) {
  return requirement >> 6;
}

uint32_t hot_letter_length(uint32_t packed) {
  return packed & 0xffffU;
}

uint32_t hot_requirement_count(uint32_t packed) {
  return (packed >> 16) & 0xffU;
}

uint32_t hot_repeated_count(uint32_t packed) {
  return packed >> 24;
}

uint32_t projected_total_length(uint32_t packed) {
  return packed & 0xffffU;
}

uint32_t projected_wild_length(uint32_t packed) {
  return packed >> 16;
}

double round_score_bound_up(
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

namespace {

// Bound the error in fl(fl(class_score + boundary) + child). IEEE double
// round-to-nearest incurs less than one DBL_EPSILON times the sum of operand
// magnitudes here. The factor of four also covers rounding while calculating
// the magnitude and subnormal results (the added 1 dominates their error).
static double score_candidate_rounding_error(
    double class_score, double boundary, double child) {
  double magnitude = fabs(class_score) + fabs(boundary);
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

static int length_certificate_mode() {
  char const* mode = getenv("NUTRIMATIC_LENGTH_CERTIFICATE");
  if (mode == NULL || mode[0] == '\0' || strcmp(mode, "1") == 0)
    return 2;
  if (strcmp(mode, "shadow") == 0) return 1;
  if (strcmp(mode, "0") == 0) return 0;
  return 0;
}

static size_t search_task_target(size_t threads) {
  size_t target =
      threads > SIZE_MAX / 64 ? SIZE_MAX : threads * 64;
  char const* forced = getenv("NUTRIMATIC_SEARCH_TASKS");
  if (forced == NULL || forced[0] == '\0') return target;
  errno = 0;
  char* end = NULL;
  unsigned long long const parsed = strtoull(forced, &end, 10);
  if (errno == 0 && end != forced && *end == '\0' &&
      parsed != 0 && parsed <= SIZE_MAX)
    return size_t(parsed);
  return target;
}

static char const* score_bound_mode_name(
    DfsAnagramSearch::ScoreBoundMode mode) {
  switch (mode) {
    case DfsAnagramSearch::SCORE_BOUND_PROJECTED:
      return "projected dense";
    case DfsAnagramSearch::SCORE_BOUND_OFF:
      return "off";
  }
  return "unknown";
}

}  // namespace

DfsAnagramSearch::DfsAnagramSearch(DfsClassList const* classes,
                                   std::string const& letters,
                                   double segment_penalty,
                                   int64_t corpus_total,
                                   size_t score_cache_bytes,
                                   size_t preprocess_threads,
                                   size_t search_threads,
                                   double word_bonus):
    class_list(classes),
    letters(letters),
    score_model(segment_penalty, corpus_total, word_bonus),
    segment_boundary_log_score(
        score_model.segment_boundary_log_score()),
    max_depth(derived_max_depth(classes, letters.size())),
    score_cache_budget(score_cache_bytes),
    requested_preprocess_threads(std::max(size_t(1), preprocess_threads)),
    requested_search_threads(std::max(size_t(1), search_threads)),
    bag_mask(0),
    current_score_key(0),
    exact_root_key(0),
    current_letters_left(0),
    score_exact_mask(0),
    score_state_count(0),
    score_effective_states(0),
    score_exact_letters(0),
    score_wild_letters(0),
    score_wild_span(1),
    projected_actions_ready(false),
    support_scan_vector(support_scan_avx2_enabled()),
    hot_classes_ready(false),
    empty_class_list(false),
    unsupported_reason(NULL),
    length_certificate_requested(false),
    length_certificate_shadow(false),
    length_certificate_ready(false),
    certificate_stride(0),
    certificate_group_tests(0),
    certificate_group_rejects(0),
    certificate_scans_skipped(0),
    certificate_scans_kept(0),
    bound_mode(SCORE_BOUND_OFF),
    bound_capacity(0),
    bound_value_bytes(0),
    bound_complete(false),
    root_score_bound(HUGE_VAL),
    root_score_bound_ready(false),
    bound_entries(0),
    bound_states_computed(0),
    bound_candidate_tests(0),
    bound_fitting_transitions(0),
    bound_transitions(0),
    bound_nextafter_calls(0),
    bound_charged_bytes(0),
    bound_prunes(0),
    exact_flat_capacity(0),
    exact_flat_entry_limit(0),
    exact_flat_entries(0),
    completable_checked(0),
    completable_bound_reject_count(0),
    completable_exact_bound_accept_count(0),
    completable_exact_validation_count(0),
    exact_memo_states(0),
    exact_memo_hit_count(0),
    exact_memo_lookahead(EXACT_MEMO_LOOKAHEAD_DEFAULT),
    exact_lookahead_full_windows(0),
    exact_lookahead_known_true_wins(0),
    exact_lookahead_reprobes_decided(0),
    exact_lookahead_recursive_expansions(0),
    progress_enabled(false),
    progress_interval(0),
    next_progress(0),
    nodes(0),
    solutions(0),
    setup_seconds(0.0),
    search_seconds(0.0),
    actual_preprocess_threads(1),
    actual_search_threads(1),
    search_tasks_created(0),
    progress_nodes(0),
    progress_solutions(0) {
  assert(class_list != NULL);
  static_assert(sizeof(AtomicWord) == sizeof(uint64_t),
                "atomic bound words must remain eight bytes");
  static_assert(sizeof(AtomicFloatWord) == sizeof(uint32_t),
                "atomic float bound words must remain four bytes");
  static_assert(std::is_trivially_destructible<AtomicWord>::value,
                "atomic bound words must be trivially destructible");
  static_assert(std::is_trivially_destructible<AtomicFloatWord>::value,
                "atomic float bound words must be trivially destructible");

  // members[0] is exactly what a global member sort destroys, and this is the
  // only production read of the member store outside output.
  assert(!class_list->members_invalidated());
  DfsClassSpan const all_classes = class_list->classes();
  best_member_log_scores.reserve(all_classes.size());
  for (size_t i = 0; i < all_classes.size(); ++i) {
    assert(class_list->member_count(i) > 0);
    DfsMemberView const best = class_list->member(i, 0);
    assert(best.count > 0);
    best_member_log_scores.push_back(score_model.first_segment_log_score(
        best.count, best.word_count > 1));
  }
}

// The non-hot fallbacks below are retained as source but unreachable: any bag
// phase 1 accepted is one phase 2 can prepare, so failing here means an
// invariant broke rather than a query being too large.
void DfsAnagramSearch::require_hot_classes() const {
  if (hot_classes_ready) return;
  dfs_diagnostic_to_stream(stderr,
      "error: phase 2 cannot search this bag (%s)\n",
      unsupported_reason != NULL ? unsupported_reason : "reason not recorded");
  abort();
}

bool DfsAnagramSearch::prepare_hot_classes() {
  static_assert(sizeof(FitClassMetadata) == sizeof(uint64_t),
                "FitClass metadata must remain one register wide");
  static_assert(std::is_trivially_copyable<FitClassMetadata>::value,
                "FitClass metadata must remain trivially copyable");
  static_assert(sizeof(FitClass) == 16,
                "FitClass must remain four per cache line");
  fit_classes.reset();
  class_supports.reset();
  score_key_deltas.reset();
  score_wild_lengths.reset();
  packed_letters.reset();

  auto const unsupported = [this](char const* reason) {
    unsupported_reason = reason;
    return false;
  };

  DfsClassSpan const classes = class_list->classes();
  assert(!classes.empty());
  if (classes.size() > UINT32_MAX ||
      classes.size() > SIZE_MAX / sizeof(FitClass) ||
      classes.size() > SIZE_MAX / sizeof(uint16_t))
    return unsupported("too many classes to index");
  if (letters.size() > UINT16_MAX)
    return unsupported("bag longer than a wildcard length field holds");

  size_t requirements = 0;
  for (size_t ci = 0; ci < classes.size(); ++ci) {
    if (requirements > UINT32_MAX - classes[ci].letters_count)
      return unsupported("too many packed letter requirements");
    requirements += classes[ci].letters_count;
  }
  if (requirements > SIZE_MAX / sizeof(uint32_t))
    return unsupported("packed letter requirements exceed the address space");

  FitClass* fit = static_cast<FitClass*>(
      dfs_allocate_aligned(classes.size() * sizeof(FitClass)));
  if (fit == NULL) return unsupported("could not allocate the fit classes");
  std::unique_ptr<FitClass, DfsAlignedFree> new_fit(fit);

  uint64_t* supports = static_cast<uint64_t*>(
      dfs_allocate_aligned(classes.size() * sizeof(uint64_t)));
  if (supports == NULL)
    return unsupported("could not allocate the support masks");
  std::unique_ptr<uint64_t, DfsAlignedFree> new_supports(supports);

  uint64_t* score_deltas = static_cast<uint64_t*>(
      dfs_allocate_aligned(classes.size() * sizeof(uint64_t)));
  if (score_deltas == NULL)
    return unsupported("could not allocate the score-key deltas");
  std::unique_ptr<uint64_t, DfsAlignedFree> new_score_deltas(score_deltas);

  uint16_t* wild_lengths = static_cast<uint16_t*>(
      dfs_allocate_aligned(classes.size() * sizeof(uint16_t)));
  if (wild_lengths == NULL)
    return unsupported("could not allocate the wildcard lengths");
  std::unique_ptr<uint16_t, DfsAlignedFree> new_wild_lengths(wild_lengths);

  uint32_t* packed = NULL;
  if (requirements != 0) {
    packed = static_cast<uint32_t*>(
        dfs_allocate_aligned(requirements * sizeof(uint32_t)));
    if (packed == NULL)
      return unsupported("could not allocate the packed letters");
  }
  std::unique_ptr<uint32_t, DfsAlignedFree> new_packed(packed);

  size_t offset = 0;
  std::array<int, DFS_SYMBOL_COUNT> const& symbol_to_rank =
      class_list->symbol_to_rank();
  uint16_t decoded[DFS_SYMBOL_COUNT];
  for (size_t ci = 0; ci < classes.size(); ++ci) {
    DfsClassRecord const& source = classes[ci];
    size_t const letter_count = class_list->decode_class_letters(ci, decoded);
    assert(letter_count == source.letters_count);
    uint64_t score_delta = 0;
    uint32_t wild_length = 0;
    uint64_t support = 0;
    uint32_t repeated = 0;

    for (size_t i = 0; i < letter_count; ++i)
      if (dfs_class_letter_count(decoded[i]) > 1) ++repeated;

    size_t write = offset;
    for (int repeated_pass = 1; repeated_pass >= 0; --repeated_pass) {
      for (size_t i = 0; i < letter_count; ++i) {
        uint32_t const count = dfs_class_letter_count(decoded[i]);
        if ((count > 1) != (repeated_pass != 0)) continue;
        uint32_t const rank = uint32_t(symbol_to_rank[
            size_t(dfs_class_letter_symbol(decoded[i]))]);
        packed[write++] = (count << 6) | rank;
      }
    }
    assert(write == offset + letter_count);

    for (size_t i = 0; i < letter_count; ++i) {
      uint32_t const count = dfs_class_letter_count(decoded[i]);
      uint32_t const rank = uint32_t(symbol_to_rank[
          size_t(dfs_class_letter_symbol(decoded[i]))]);
      support |= UINT64_C(1) << rank;
      uint64_t term;
      uint64_t next;
      if ((score_exact_mask & (UINT64_C(1) << rank)) != 0) {
        if (!dfs_checked_multiply_u64(
                count, score_multipliers[rank], &term) ||
            !dfs_checked_add_u64(score_delta, term, &next))
          return unsupported("score-key delta overflowed 64 bits");
        score_delta = next;
      } else {
        wild_length += count;
      }
    }

    fit[ci].support_mask = support;
    supports[ci] = support;
    fit[ci].metadata.letters_offset = uint32_t(offset);
    fit[ci].metadata.packed_length_and_count =
        uint32_t(source.key_length) |
        (uint32_t(letter_count) << 16) |
        (repeated << 24);
    uint64_t flat_score_delta;
    if (!dfs_checked_multiply_u64(
            score_delta, uint64_t(score_wild_span), &flat_score_delta) ||
        !dfs_checked_add_u64(
            flat_score_delta, uint64_t(wild_length),
            &flat_score_delta))
      return unsupported("flattened score-key delta overflowed 64 bits");
    score_deltas[ci] = flat_score_delta;
    wild_lengths[ci] = uint16_t(wild_length);
    offset = write;
  }

  fit_classes = std::move(new_fit);
  class_supports = std::move(new_supports);
  score_key_deltas = std::move(new_score_deltas);
  score_wild_lengths = std::move(new_wild_lengths);
  packed_letters = std::move(new_packed);
  return true;
}

bool DfsAnagramSearch::prepare_length_certificate() {
  auto const fail = [this]() {
    certificate_stride = 0;
    certificate_max_score.clear();
    certificate_group_end.clear();
    length_tail_bounds.clear();
    length_certificate_ready = false;
    return false;
  };
  fail();
  if (!length_certificate_requested) return true;
  if (letters.size() == SIZE_MAX) return false;

  try {
    DfsClassSpan const classes = class_list->classes();
    size_t max_length = 0;
    for (size_t i = 0; i < classes.size(); ++i)
      max_length = std::max<size_t>(max_length, classes[i].key_length);
    if (max_length == SIZE_MAX) return false;
    certificate_stride = max_length + 1;
    if (certificate_stride != 0 &&
        size_t(DFS_SYMBOL_COUNT) >
            SIZE_MAX / certificate_stride)
      return false;
    size_t const group_entries =
        size_t(DFS_SYMBOL_COUNT) * certificate_stride;
    certificate_max_score.assign(group_entries, -HUGE_VAL);
    certificate_group_end.assign(group_entries, 0);

    std::vector<double> best_score(
        letters.size() + 1, -HUGE_VAL);
    for (size_t rank = 0; rank < DFS_SYMBOL_COUNT; ++rank) {
      int const symbol = class_list->rank_to_symbol()[rank];
      size_t const begin = class_list->candidate_begin(symbol);
      size_t const end = class_list->candidate_end(symbol);
      size_t const base = rank * certificate_stride;
      size_t previous_length = SIZE_MAX;
      for (size_t i = begin; i < end; ++i) {
        size_t const length = classes[i].key_length;
        if (length >= certificate_stride ||
            length > letters.size())
          return fail();
        if (previous_length != SIZE_MAX &&
            length > previous_length)
          return fail();
        previous_length = length;
        certificate_max_score[base + length] = std::max(
            certificate_max_score[base + length],
            best_member_log_scores[i]);
        certificate_group_end[base + length] = uint32_t(i + 1);
        best_score[length] = std::max(
            best_score[length], best_member_log_scores[i]);
      }
    }

    length_tail_bounds.assign(letters.size() + 1, -HUGE_VAL);
    length_tail_bounds[0] = 0.0;
    uint64_t ignored_nextafter_calls = 0;
    for (size_t left = 1; left <= letters.size(); ++left) {
      double best = -HUGE_VAL;
      double max_rounding_error = 0.0;
      for (size_t length = 1; length <= left; ++length) {
        if (best_score[length] == -HUGE_VAL ||
            length_tail_bounds[left - length] == -HUGE_VAL)
          continue;
        double const child = length_tail_bounds[left - length];
        double const candidate =
            best_score[length] + segment_boundary_log_score + child;
        best = std::max(best, candidate);
        max_rounding_error = std::max(
            max_rounding_error,
            score_candidate_rounding_error(
                best_score[length], segment_boundary_log_score, child));
      }
      if (best != -HUGE_VAL)
        length_tail_bounds[left] = round_score_bound_up(
            static_cast<long double>(best) +
                static_cast<long double>(max_rounding_error),
            &ignored_nextafter_calls);
    }
  } catch (...) {
    return fail();
  }
  length_certificate_ready = true;
  return true;
}

bool DfsAnagramSearch::length_certificate_rejects(
    size_t base, size_t length, size_t letters_left,
    double representative_log_score, double floor) const {
  assert(base + length < certificate_max_score.size());
  assert(length <= letters_left);
  double const group_best = certificate_max_score[base + length];
  double const tail = length_tail_bounds[letters_left - length];
  if (group_best == -HUGE_VAL || tail == -HUGE_VAL) return true;
  long double const upper =
      static_cast<long double>(representative_log_score) +
      static_cast<long double>(segment_boundary_log_score) +
      static_cast<long double>(group_best) +
      static_cast<long double>(tail);
  long double const magnitude =
      fabsl(static_cast<long double>(representative_log_score)) +
      fabsl(static_cast<long double>(segment_boundary_log_score)) +
      fabsl(static_cast<long double>(group_best)) +
      fabsl(static_cast<long double>(tail)) +
      fabsl(static_cast<long double>(floor)) + 1.0L;
  long double const padding =
      magnitude * static_cast<long double>(DBL_EPSILON) *
      (static_cast<long double>(max_depth) + 2.0L) * 16.0L;
  return upper + padding <= static_cast<long double>(floor);
}

void DfsAnagramSearch::clear_score_bounds() {
  bound_mode = SCORE_BOUND_OFF;
  bound_float_values.reset();
  bound_plain_float_values.reset();
  bound_capacity = 0;
  bound_value_bytes = 0;
  bound_complete = false;
  root_score_bound = HUGE_VAL;
  root_score_bound_ready = false;
  bound_entries = 0;
  bound_charged_bytes = 0;
}

void DfsAnagramSearch::prepare_score_bounds(bool requested) {
  clear_score_bounds();
  if (!hot_classes_ready || !requested ||
      !score_bound_arithmetic_supported() ||
      score_cache_budget < DFS_CACHE_ALIGNMENT)
    return;

  if (!projected_actions_ready) return;
  size_t float_bytes = 0;
  if (score_effective_states == 0 ||
      score_effective_states > SIZE_MAX ||
      !dense_bound_requirements(
          score_effective_states, sizeof(float), &float_bytes) ||
      float_bytes > score_cache_budget)
    return;
  bool const bottom_up_eligible =
      score_wild_span != 0 &&
      score_effective_states / score_wild_span <= UINT32_MAX;
  if (bottom_up_eligible) {
    float* values = static_cast<float*>(dfs_allocate_aligned(float_bytes));
    if (values == NULL) return;
    bound_plain_float_values.reset(values);
  } else {
    AtomicFloatWord* values = static_cast<AtomicFloatWord*>(
        dfs_allocate_aligned(float_bytes));
    if (values == NULL) return;
    for (size_t i = 0; i < size_t(score_effective_states); ++i) {
      new (&values[i]) AtomicFloatWord;
      values[i].value.store(FLOAT_BOUND_UNSEEN, std::memory_order_relaxed);
    }
    bound_float_values.reset(values);
  }
  bound_capacity = size_t(score_effective_states);
  bound_value_bytes = sizeof(float);
  bound_complete = true;
  bound_mode = SCORE_BOUND_PROJECTED;
  bound_charged_bytes = float_bytes;
}

bool DfsAnagramSearch::load_score_bound(
    uint64_t key, double* value) const {
  if (bound_mode == SCORE_BOUND_OFF || key >= bound_capacity)
    return false;
  assert(bound_value_bytes == sizeof(float));
  if (bound_plain_float_values.get() != NULL) {
    *value = double(bound_plain_float_values.get()[size_t(key)]);
    return true;
  }
  uint32_t const stored =
      bound_float_values.get()[size_t(key)].value.load(
          std::memory_order_relaxed);
  if (stored == FLOAT_BOUND_UNSEEN ||
      stored == FLOAT_BOUND_COMPUTING)
    return false;
  *value = double(bits_to_float(stored));
  return true;
}

DfsAnagramSearch::Reachability DfsAnagramSearch::cached_reachability(
    uint64_t score_key, bool original_root) const {
  double value;
  if (original_root) {
    if (!root_score_bound_ready) return REACHABILITY_UNKNOWN;
    value = root_score_bound;
  } else if (!load_score_bound(score_key, &value)) {
    return REACHABILITY_UNKNOWN;
  }
  if (value == -HUGE_VAL) return REACHABILITY_NO;
  if (bound_mode != SCORE_BOUND_PROJECTED || score_wild_letters == 0)
    return REACHABILITY_YES;
  return REACHABILITY_UNKNOWN;
}

void DfsAnagramSearch::publish_parallel_score_bound(
    uint64_t key, double value) {
  assert(key < bound_capacity);
  assert(bound_value_bytes == sizeof(float));
  assert(bound_plain_float_values.get() == NULL);
  bound_float_values.get()[size_t(key)].value.store(
      float_to_bits(round_float_score_bound_up(value)),
      std::memory_order_release);
}

bool DfsAnagramSearch::prepare_phase_two(
    int64_t progress_factor, bool allow_cache_fallback, int exact_letters,
    bool score_bounds_requested) {
  FILE* const progress = dfs_diagnostic_stream();
  typedef std::chrono::steady_clock PhaseClock;
  PhaseClock::time_point const setup_start = PhaseClock::now();
  bound_states_computed = 0;
  bound_candidate_tests = 0;
  bound_fitting_transitions = 0;
  bound_transitions = 0;
  bound_nextafter_calls = 0;
  setup_seconds = 0.0;
  search_seconds = 0.0;
  actual_preprocess_threads = 1;
  actual_search_threads = 1;
  search_tasks_created = 0;
  progress_nodes.store(0, std::memory_order_relaxed);
  progress_solutions.store(0, std::memory_order_relaxed);
  certificate_stride = 0;
  certificate_max_score.clear();
  certificate_group_end.clear();
  length_tail_bounds.clear();
  length_certificate_requested = false;
  length_certificate_shadow = false;
  length_certificate_ready = false;
  certificate_group_tests = 0;
  certificate_group_rejects = 0;
  certificate_scans_skipped = 0;
  certificate_scans_kept = 0;
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
    for (int rank = DFS_SYMBOL_COUNT - 1; rank >= 0; --rank) {
      uint64_t const radix = uint64_t(bag[size_t(rank)]) + 1;
      if (!dfs_checked_multiply_u64(state_count, radix, &state_count)) {
        encodable = false;
        break;
      }
    }
  }
  // Phase 1 aborts when this same product overflows, and its symbol-ordered
  // multipliers are the commuted form of the rank-ordered ones above, so the
  // two verdicts cannot drift.
  assert(encodable);
  exact_root_key = state_count - 1;
  bag_mask = 0;
  for (int rank = 0; rank < DFS_SYMBOL_COUNT; ++rank)
    if (bag[size_t(rank)] != 0)
      bag_mask |= UINT64_C(1) << rank;

  size_t forced_exact_letters = 0;
  bool has_forced_exact_letters = false;
  if (exact_letters >= 0) {
    forced_exact_letters = size_t(exact_letters);
    has_forced_exact_letters = true;
  }
  projected_actions_ready = false;
  projected_actions.clear();
  projected_repeated_requirements.clear();
  projected_bucket_starts.fill(0);
  score_exact_mask = bag_mask;
  score_exact_letters = size_t(__builtin_popcountll(bag_mask));
  score_wild_letters = 0;
  score_wild_span = 1;
  score_state_count = state_count;
  score_effective_states = 0;

  if (encodable) {
    std::vector<int> present_ranks;
    present_ranks.reserve(score_exact_letters);
    for (int rank = 0; rank < DFS_SYMBOL_COUNT; ++rank)
      if (bag[size_t(rank)] != 0)
        present_ranks.push_back(rank);

    size_t selected_exact_letters = 0;
    if (has_forced_exact_letters) {
      selected_exact_letters =
          std::min(forced_exact_letters, present_ranks.size());
    } else {
      uint64_t exact_product = 1;
      size_t exact_total = 0;
      for (size_t d = 0; d <= present_ranks.size(); ++d) {
        size_t const wild_total = letters.size() - exact_total;
        uint64_t projected_states;
        bool const states_ok = dfs_checked_multiply_u64(
            exact_product, uint64_t(wild_total + 1),
            &projected_states);
        uint64_t effective = projected_states;
        if (states_ok && d != 0) {
          uint64_t const root_radix =
              uint64_t(bag[size_t(present_ranks[0])]) + 1;
          effective =
              (projected_states / root_radix) * (root_radix - 1);
        }
        size_t bytes = 0;
        if (states_ok &&
            dense_bound_requirements(
                effective, sizeof(float), &bytes) &&
            bytes <= score_cache_budget)
          selected_exact_letters = d;
        if (d != present_ranks.size()) {
          int const rank = present_ranks[d];
          uint64_t next_product;
          if (!dfs_checked_multiply_u64(
                  exact_product,
                  uint64_t(bag[size_t(rank)]) + 1,
                  &next_product))
            break;
          exact_product = next_product;
          exact_total += bag[size_t(rank)];
        }
      }
    }

    score_exact_mask = 0;
    size_t exact_total = 0;
    for (size_t i = 0; i < selected_exact_letters; ++i) {
      int const rank = present_ranks[i];
      score_exact_mask |= UINT64_C(1) << rank;
      exact_total += bag[size_t(rank)];
    }
    score_exact_letters = selected_exact_letters;
    score_wild_letters = letters.size() - exact_total;
    score_wild_span = score_wild_letters + 1;

    uint64_t exact_product = 1;
    score_multipliers.fill(0);
    for (int rank = DFS_SYMBOL_COUNT - 1; rank >= 0; --rank) {
      if ((score_exact_mask & (UINT64_C(1) << rank)) == 0) continue;
      score_multipliers[size_t(rank)] = exact_product;
      if (!dfs_checked_multiply_u64(
              exact_product, uint64_t(bag[size_t(rank)]) + 1,
              &exact_product)) {
        encodable = false;
        break;
      }
    }
    if (encodable &&
        !dfs_checked_multiply_u64(
            exact_product, uint64_t(score_wild_span),
            &score_state_count))
      encodable = false;
    if (encodable) {
      score_effective_states = score_state_count;
      if (score_exact_mask != 0) {
        int const root_rank = __builtin_ctzll(score_exact_mask);
        uint64_t const root_radix =
            uint64_t(bag[size_t(root_rank)]) + 1;
        score_effective_states =
            (score_state_count / root_radix) * (root_radix - 1);
      }
    }
  }

  current_score_key = encodable ? score_state_count - 1 : 0;
  current_letters_left = letters.size();
  unsupported_reason = encodable ? NULL : "bag state count exceeds 64 bits";
  empty_class_list = class_list->classes().empty();
  hot_classes_ready =
      encodable && !empty_class_list && prepare_hot_classes();
  if (hot_classes_ready)
    prepare_projected_actions();
  if (progress != NULL) {
    if (encodable) {
      size_t projected_bytes = 0;
      bool const projected_size_ok = dense_bound_requirements(
          score_effective_states, sizeof(float), &projected_bytes);
      dfs_diagnostic(
          "phase 2 preflight: projected score table keeps %zu "
          "rarest letters exact, merges %zu wildcard letters; "
          "%llu states, ",
          score_exact_letters, score_wild_letters,
          (unsigned long long) score_effective_states);
      if (projected_size_ok)
        fprintf(progress, "%zu bytes\n", projected_bytes);
      else
        fputs("size exceeds the supported range\n", progress);
      if (projected_actions_ready)
        dfs_diagnostic(
            "phase 2 preflight: %zu concrete classes, "
            "%zu projected actions\n",
            class_list->classes().size(),
            projected_actions.size());
    }
    fflush(progress);
  }
  bool const score_bounds_applicable =
      hot_classes_ready && score_bounds_requested &&
      score_bound_arithmetic_supported() &&
      bag_mask != 0;
  int const certificate_mode = length_certificate_mode();
  length_certificate_requested =
      certificate_mode != 0 && score_bounds_applicable;
  length_certificate_shadow = certificate_mode == 1;
  if (length_certificate_requested) {
    if (!prepare_length_certificate())
      length_certificate_ready = false;
  }
  if (!allow_cache_fallback && score_bounds_applicable) {
    size_t required_bytes = 0;
    char const* mode_name = "projected dense";
    bool const size_available = dense_bound_requirements(
        score_effective_states, sizeof(float), &required_bytes);
    if (!size_available) {
      if (progress != NULL) {
        fprintf(progress,
                "error: %s score table size exceeds the supported range\n"
                "       use --allow-cache-fallback\n",
                mode_name);
        fflush(progress);
      }
      return false;
    }
    if (required_bytes > score_cache_budget) {
      size_t const required_mib =
          required_bytes / MIB +
          size_t(required_bytes % MIB != 0);
      size_t const supplied_mib = score_cache_budget / MIB;
      if (progress != NULL) {
        fprintf(progress,
                "error: %s score table requires at least %zu MiB; "
                "supplied cache is %zu MiB\n"
                "       use -C %zu or --allow-cache-fallback\n",
                mode_name, required_mib, supplied_mib, required_mib);
        fflush(progress);
      }
      return false;
    }
  }
  prepare_score_bounds(score_bounds_applicable);
  if (progress != NULL) {
    dfs_diagnostic("phase 2 preflight: score-bound mode %s",
                   score_bound_mode_name(bound_mode));
    if (bound_mode != SCORE_BOUND_OFF)
      fprintf(progress, " (%zu-byte values, capacity %zu, %s coverage)",
              bound_value_bytes, bound_capacity,
              bound_complete ? "complete effective" : "partial");
    fputc('\n', progress);
    if (bound_mode == SCORE_BOUND_PROJECTED)
      dfs_diagnostic(
          "phase 2 preflight: projected evaluator %s\n",
          bound_plain_float_values.get() != NULL
              ? "bottom-up plain"
              : "recursive atomic");
    fflush(progress);
  }
  if (bound_mode != SCORE_BOUND_OFF && bound_complete) {
    if (bound_mode == SCORE_BOUND_PROJECTED) {
      bool const computed =
          bound_plain_float_values.get() != NULL
              ? compute_projected_score_bound_bottom_up(
                    requested_preprocess_threads)
              : compute_projected_score_bound_parallel(
                    requested_preprocess_threads);
      if (!computed)
        clear_score_bounds();
    }
  }

  path.clear();
  path.reserve(letters.size());
  progress_enabled = progress != NULL;
  int64_t const normalized_progress_factor =
      std::max<int64_t>(progress_factor, 1);
  progress_interval =
      normalized_progress_factor <= INT64_MAX / INT64_C(100000)
          ? INT64_C(100000) * normalized_progress_factor
          : INT64_MAX;
  next_progress = progress_interval;
  nodes = 0;
  solutions = 0;
  bound_prunes = 0;

  PhaseClock::time_point const setup_end = PhaseClock::now();
  setup_seconds =
      std::chrono::duration<double>(setup_end - setup_start).count();
  if (progress_enabled) {
    dfs_diagnostic(
        "phase 2: precomputed %zu bounded states in %.1fs\n",
        bound_states_computed, setup_seconds);
  }
  return true;
}

bool DfsAnagramSearch::run(DfsSolutionSink* sink,
                           int64_t progress_factor,
                           bool allow_cache_fallback,
                           int exact_letters,
                           bool verbose) {
  bool const score_bounds_requested =
      sink != NULL && sink->supports_score_pruning();
  if (!prepare_phase_two(
          progress_factor, allow_cache_fallback, exact_letters,
          score_bounds_requested))
    return false;

  typedef std::chrono::steady_clock PhaseClock;
  PhaseClock::time_point const search_start = PhaseClock::now();
  if (empty_class_list) {
    search_seconds = 0.0;
    return true;
  }
  require_hot_classes();
  {
    bool const parallel_eligible =
        requested_search_threads > 1 &&
        (sink == NULL || sink->supports_parallel_search());
    if (parallel_eligible) {
      run_parallel_search(
          sink, requested_search_threads,
          search_task_target(requested_search_threads),
          uint64_t(std::max<int64_t>(progress_factor, 1)), verbose);
    } else {
      SearchWorker worker;
      start_search_worker(&worker);
      walk(&worker, letters.size(), 0, 0.0, sink);
      merge_search_worker(worker);
    }
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
      packed_letters.get() + candidate.metadata.letters_offset;
  uint32_t const repeated =
      hot_repeated_count(candidate.metadata.packed_length_and_count);
  for (uint32_t i = 0; i < repeated; ++i) {
    uint32_t const requirement = requirements[i];
    if (bag[packed_rank(requirement)] < packed_count(requirement))
      return false;
  }
  return true;
}

bool DfsAnagramSearch::hot_class_fits(
    uint32_t class_index, SearchWorker const& worker) const {
  FitClass const& candidate = fit_classes.get()[class_index];
  if ((candidate.support_mask & ~worker.bag_mask) != 0) return false;
  return hot_class_multiplicity_fits(class_index, worker);
}

bool DfsAnagramSearch::hot_class_multiplicity_fits(
    uint32_t class_index, SearchWorker const& worker) const {
  return hot_class_multiplicity_fits(
      fit_classes.get()[class_index].metadata, worker);
}

bool DfsAnagramSearch::hot_class_multiplicity_fits(
    FitClassMetadata metadata, SearchWorker const& worker) const {
  uint32_t const* requirements =
      packed_letters.get() + metadata.letters_offset;
  uint32_t const repeated =
      hot_repeated_count(metadata.packed_length_and_count);
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
        fit_classes.get()[middle].metadata.packed_length_and_count);
    if (candidate_length > letters_left)
      begin = middle + 1;
    else
      end = middle;
  }
  return begin;
}

bool DfsAnagramSearch::should_prune(
    SearchWorker* worker, double representative_log_score,
    DfsSolutionSink* sink, size_t letters_left) {
  Reachability const reachability = cached_reachability(
      worker->score_key, worker->path.empty());
  if (reachability == REACHABILITY_NO) return true;
  // H charges a boundary to every class it adds. That is exact below the first
  // chosen class, but the root's first class does not pay a boundary.
  if (worker->path.empty()) return false;

  double floor;
  if (sink == NULL || !sink->score_floor(&floor)) return false;
  uint64_t const query_key = worker->score_key;
  if (query_key >= bound_capacity) return false;

  double remaining_bound;
  if (!load_score_bound(query_key, &remaining_bound)) return false;
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

void DfsAnagramSearch::visit_fitting_class(SearchWorker* worker,
    uint32_t class_index, FitClassMetadata metadata, size_t letters_left,
    double representative_log_score, DfsSolutionSink* sink) {

  if (DFS_UNLIKELY(worker->path.size() >= max_depth)) {
    return;
  }
  double const class_score = best_member_log_scores[class_index];
  size_t const candidate_length =
      hot_letter_length(metadata.packed_length_and_count);
  assert(candidate_length <= letters_left);
  size_t const next_letters_left = letters_left - candidate_length;
  bool const first_class = worker->path.empty();

  worker->path.push_back(class_index);
  double const next_log_score =
      first_class
          ? class_score
          : score_model.append_log_score(representative_log_score, class_score);

  if (DFS_UNLIKELY(next_letters_left == 0)) {
    ++worker->solutions;
    if (sink != NULL) sink->emit(worker->path, next_log_score);
    worker->path.pop_back();
    return;
  }

  uint32_t const* requirements = packed_letters.get() + metadata.letters_offset;
  uint32_t const requirement_count =
      hot_requirement_count(metadata.packed_length_and_count);
  uint64_t const parent_bag_mask = worker->bag_mask;
  for (uint32_t i = 0; i < requirement_count; ++i) {
    uint32_t const requirement = requirements[i];
    uint32_t const requirement_rank = packed_rank(requirement);
    uint32_t& remaining = worker->bag[requirement_rank];
    remaining -= packed_count(requirement);
    worker->bag_mask &= ~(uint64_t(remaining == 0) << requirement_rank);
  }
  worker->score_key -= score_key_deltas.get()[class_index];
  if (DFS_UNLIKELY(worker->split_depth != 0)
      && worker->path.size() <= worker->split_depth) {
    SearchTask task;
    task.bag = worker->bag;
    task.bag_mask = worker->bag_mask;
    task.score_key = worker->score_key;
    assert(worker->path.size() <= MAX_SPLIT_DEPTH);
    for (size_t i = 0; i < worker->path.size(); ++i)
      task.path[i] = uint32_t(worker->path[i]);
    task.path_size = uint32_t(worker->path.size());
    task.entry_point = class_index;
    task.letters_left = uint32_t(next_letters_left);
    task.representative_log_score = next_log_score;
    assert(worker->produced != NULL);
    worker->produced->push_back(task);
  } else {
    walk(worker, next_letters_left, class_index, next_log_score, sink);
  }
  worker->score_key += score_key_deltas.get()[class_index];
  for (uint32_t i = 0; i < requirement_count; ++i) {
    uint32_t const requirement = requirements[i];
    uint32_t const requirement_rank = packed_rank(requirement);
    worker->bag[requirement_rank] += packed_count(requirement);
  }
  worker->bag_mask = parent_bag_mask;
  worker->path.pop_back();
}

bool DfsAnagramSearch::visit_fitting_range(
    SearchWorker* worker, size_t begin, size_t end, size_t letters_left,
    double representative_log_score, DfsSolutionSink* sink) {
  for (size_t class_index = begin; class_index < end; ++class_index) {
    uint32_t const id = uint32_t(class_index);
    FitClass const& fit = fit_classes.get()[id];
    if ((fit.support_mask & ~worker->bag_mask) != 0) continue;
    FitClassMetadata const& metadata = fit.metadata;
    if (!hot_class_multiplicity_fits(metadata, *worker)) continue;
    visit_fitting_class(
        worker, id, metadata, letters_left, representative_log_score, sink);
    if (DFS_UNLIKELY(sink != NULL && sink->should_stop())) return true;
  }
  return false;
}

void DfsAnagramSearch::walk(SearchWorker* worker, size_t letters_left,
                            size_t entry_point,
                            double representative_log_score,
                            DfsSolutionSink* sink) {
  ++worker->nodes;
  if (DFS_UNLIKELY(progress_enabled &&
                   worker->nodes == worker->next_progress))
    report_search_progress(worker);

  if (DFS_UNLIKELY(worker->bag_mask == 0)) return;
  if (DFS_UNLIKELY(should_prune(
          worker, representative_log_score, sink, letters_left))) {
    ++worker->bound_prunes;
    return;
  }

  int const rank = __builtin_ctzll(worker->bag_mask);
  int const forced_symbol =
      class_list->rank_to_symbol()[size_t(rank)];
  size_t const begin = first_length_candidate(
      class_list->candidate_begin(forced_symbol),
      class_list->candidate_end(forced_symbol),
      letters_left);
  size_t const end = class_list->candidate_end(forced_symbol);

  size_t const start = std::max(begin, entry_point);
  if (DFS_UNLIKELY(length_certificate_ready) &&
      !worker->path.empty()) {
    double floor;
    if (sink != NULL && sink->score_floor(&floor)) {
      walk_certified(worker, rank, start, end, letters_left,
                     representative_log_score, floor, sink);
      return;
    }
  }
  visit_fitting_range(
      worker, start, end, letters_left, representative_log_score, sink);
}

void DfsAnagramSearch::walk_certified(
    SearchWorker* worker, int rank, size_t start, size_t end,
    size_t letters_left, double representative_log_score,
    double floor, DfsSolutionSink* sink) {
  size_t const base = size_t(rank) * certificate_stride;
  size_t class_index = start;
  while (class_index < end) {
    size_t const length = hot_letter_length(
        fit_classes.get()[class_index].metadata.packed_length_and_count);
    size_t const group_end =
        size_t(certificate_group_end[base + length]);
    assert(group_end > class_index && group_end <= end);
    ++worker->certificate_group_tests;
    bool const rejected = length_certificate_rejects(
        base, length, letters_left, representative_log_score, floor);
    if (rejected) {
      ++worker->certificate_group_rejects;
      worker->certificate_scans_skipped += group_end - class_index;
      if (!length_certificate_shadow) {
        class_index = group_end;
        continue;
      }
    } else {
      worker->certificate_scans_kept += group_end - class_index;
    }
    if (visit_fitting_range(
            worker, class_index, group_end, letters_left,
            representative_log_score, sink))
      return;
    class_index = group_end;
  }
}

void DfsAnagramSearch::start_search_worker(SearchWorker* worker) {
  worker->bag = bag;
  worker->bag_mask = bag_mask;
  worker->score_key = current_score_key;
  worker->exact_key = exact_root_key;
  worker->path.clear();
  worker->path.reserve(letters.size());
  worker->nodes = 0;
  worker->solutions = 0;
  worker->bound_prunes = 0;
  worker->certificate_group_tests = 0;
  worker->certificate_group_rejects = 0;
  worker->certificate_scans_skipped = 0;
  worker->certificate_scans_kept = 0;
  worker->split_depth = 0;
  worker->produced = NULL;
  worker->next_progress =
      progress_enabled ? progress_interval : INT64_MAX;
  worker->reported_solutions = 0;
  worker->exact_memo_states = 0;
  worker->exact_memo_hits = 0;
  worker->exact_lookahead_full_windows = 0;
  worker->exact_lookahead_known_true_wins = 0;
  worker->exact_lookahead_reprobes_decided = 0;
  worker->exact_lookahead_recursive_expansions = 0;
}

void DfsAnagramSearch::report_search_progress(
    SearchWorker* worker) {
  worker->next_progress =
      worker->next_progress <= INT64_MAX - progress_interval
          ? worker->next_progress + progress_interval
          : INT64_MAX;
  int64_t const total_nodes =
      progress_nodes.fetch_add(
          progress_interval, std::memory_order_relaxed) +
      progress_interval;
  int64_t const new_solutions =
      worker->solutions - worker->reported_solutions;
  worker->reported_solutions = worker->solutions;
  int64_t const total_solutions =
      progress_solutions.fetch_add(
          new_solutions, std::memory_order_relaxed) +
      new_solutions;
  dfs_diagnostic("phase 2: %lld nodes, %lld solutions\n",
                 (long long) total_nodes, (long long) total_solutions);
}

void DfsAnagramSearch::merge_search_worker(
    SearchWorker const& worker) {
  nodes += worker.nodes;
  solutions += worker.solutions;
  bound_prunes += worker.bound_prunes;
  certificate_group_tests += worker.certificate_group_tests;
  certificate_group_rejects += worker.certificate_group_rejects;
  certificate_scans_skipped +=
      worker.certificate_scans_skipped;
  certificate_scans_kept += worker.certificate_scans_kept;
}

bool DfsAnagramSearch::run_parallel_search(
    DfsSolutionSink* sink, size_t threads, size_t target_tasks,
    uint64_t task_progress_factor, bool verbose) {
  assert(threads > 1);
  assert(target_tasks > 0);

  std::vector<SearchTask> tasks;
  SearchWorker seed;
  start_search_worker(&seed);
  seed.split_depth = 1;
  seed.produced = &tasks;
  walk(&seed, letters.size(), 0, 0.0, sink);
  if (verbose)
    dfs_diagnostic(
        "info: added %zu child tasks (%zu total)\n",
        tasks.size(), tasks.size());

  size_t head = 0;
  std::vector<SearchTask> children;
  while (tasks.size() - head < target_tasks &&
         head < tasks.size() &&
         tasks[head].path_size < MAX_SPLIT_DEPTH) {
    SearchTask const task = tasks[head++];
    children.clear();
    seed.bag = task.bag;
    seed.bag_mask = task.bag_mask;
    seed.score_key = task.score_key;
    seed.path.assign(
        task.path.begin(), task.path.begin() + task.path_size);
    seed.split_depth = task.path_size + 1;
    seed.produced = &children;
    walk(&seed, task.letters_left, task.entry_point,
         task.representative_log_score, sink);
    tasks.insert(tasks.end(), children.begin(), children.end());
    if (verbose)
      dfs_diagnostic(
          "info: added %zu child tasks (%zu total)\n",
          children.size(), tasks.size() - head);
  }
  merge_search_worker(seed);
  search_tasks_created = uint64_t(tasks.size() - head);
  if (verbose)
    dfs_diagnostic(
        "info: task splitting complete (%llu total)\n",
        (unsigned long long) search_tasks_created);
  if (head >= tasks.size()) return true;

  size_t const worker_count =
      std::min(threads, tasks.size() - head);
  std::atomic<size_t> next_task(head);
  std::vector<SearchWorker> workers(worker_count);
  std::vector<std::thread> pool;
  pool.reserve(worker_count - 1);
  for (size_t i = 0; i < worker_count; ++i)
    start_search_worker(&workers[i]);
  auto const body = [&](size_t index) {
    SearchWorker& worker = workers[index];
    bool claimed_task = false;
    for (;;) {
      size_t const slot =
          next_task.fetch_add(1, std::memory_order_relaxed);
      if (slot >= tasks.size()) {
        if (verbose)
          dfs_diagnostic(
              "info: search worker %zu exited\n", index);
        break;
      }
      if (!claimed_task) {
        claimed_task = true;
        if (verbose)
          dfs_diagnostic(
              "info: worker %zu claimed first task\n", index);
      }
      size_t const tasks_left = tasks.size() - slot - 1;
      if (verbose && uint64_t(tasks_left) % task_progress_factor == 0)
        dfs_diagnostic(
            "info: worker %zu popped task (%zu total)\n",
            index, tasks_left);
      SearchTask const& task = tasks[slot];
      worker.bag = task.bag;
      worker.bag_mask = task.bag_mask;
      worker.score_key = task.score_key;
      worker.path.assign(
          task.path.begin(), task.path.begin() + task.path_size);
      walk(&worker, task.letters_left, task.entry_point,
           task.representative_log_score, sink);
    }
  };

  try {
    for (size_t i = 1; i < worker_count; ++i)
      pool.push_back(std::thread(body, i));
  } catch (...) {
    // The caller and successfully launched workers share the same cursor, so
    // they complete every task even if a later thread cannot be constructed.
  }
  actual_search_threads = 1 + pool.size();
  body(0);
  for (size_t i = 0; i < pool.size(); ++i)
    pool[i].join();
  for (size_t i = 0; i < actual_search_threads; ++i)
    merge_search_worker(workers[i]);
  return true;
}

void DfsAnagramSearch::visit_unoptimized_class(
    size_t class_index, size_t letters_left, int rank,
    double representative_log_score, DfsSolutionSink* sink) {
  double const candidate_log_score = best_member_log_scores[class_index];
  double const next_log_score =
      path.empty()
          ? candidate_log_score
          : score_model.append_log_score(
                representative_log_score, candidate_log_score);
  size_t const candidate_length = class_list->class_length(class_index);
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
  uint16_t decoded[DFS_SYMBOL_COUNT];
  size_t const count = class_list->decode_class_letters(class_index, decoded);
  for (size_t i = 0; i < count; ++i) {
    size_t const requirement_rank = size_t(
        symbol_to_rank[size_t(dfs_class_letter_symbol(decoded[i]))]);
    bag[requirement_rank] -= dfs_class_letter_count(decoded[i]);
  }
  walk_unoptimized(next_letters_left, rank, class_index,
                   next_log_score, sink);
  for (size_t i = 0; i < count; ++i) {
    size_t const requirement_rank = size_t(
        symbol_to_rank[size_t(dfs_class_letter_symbol(decoded[i]))]);
    bag[requirement_rank] += dfs_class_letter_count(decoded[i]);
  }
  path.pop_back();
}

void DfsAnagramSearch::walk_unoptimized(
    size_t letters_left, int old_rarest_rank, size_t entry_point,
    double representative_log_score, DfsSolutionSink* sink) {
  ++nodes;
  if (progress_enabled && nodes == next_progress) {
    dfs_diagnostic("phase 2: %lld nodes, %lld solutions\n",
                   (long long) nodes, (long long) solutions);
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
  uint16_t decoded[DFS_SYMBOL_COUNT];
  for (size_t class_index = start; class_index < end; ++class_index) {
    size_t const count =
        class_list->decode_class_letters(class_index, decoded);
    bool fits = true;
    for (size_t i = 0; i < count; ++i) {
      size_t const requirement_rank = size_t(
          symbol_to_rank[size_t(dfs_class_letter_symbol(decoded[i]))]);
      if (bag[requirement_rank] < dfs_class_letter_count(decoded[i])) {
        fits = false;
        break;
      }
    }
    if (!fits) continue;
    visit_unoptimized_class(class_index, letters_left, rank,
                            representative_log_score, sink);
  }
}
