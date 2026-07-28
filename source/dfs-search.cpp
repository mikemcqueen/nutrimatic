#include "dfs-search.h"

#include "dfs-diagnostic.h"

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
#include <functional>
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

uint64_t const BOUND_UNSEEN = UINT64_C(0x7ff8000000000001);
uint64_t const BOUND_COMPUTING = UINT64_C(0x7ff8000000000002);
uint32_t const FLOAT_BOUND_UNSEEN = UINT32_C(0x7fc00001);
uint32_t const FLOAT_BOUND_COMPUTING = UINT32_C(0x7fc00002);
size_t const CACHE_ALIGNMENT = 64;
size_t const MIB = size_t(1024) * size_t(1024);

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

static uint32_t projected_total_length(uint32_t packed) {
  return packed & 0xffffU;
}

static uint32_t projected_wild_length(uint32_t packed) {
  return packed >> 16;
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

// One projected action's contribution to a contiguous run of wildcard counts.
//
// The child key is base_key + wild - score_key_delta with `wild` running
// contiguously, so `children` is an ascending contiguous float range; that is
// what lets a vector kernel load whole lane groups.
//
// This is the portable baseline, and the reference every other kernel must
// reproduce bit for bit. The expressions and their association order are
// fixed: partial_score + child for the score, and
// ((rounding_error_base + fabs(child)) + 1.0) * (DBL_EPSILON * 4.0) for the
// error envelope. A -HUGE_VAL child is dead: it must not reach either maximum
// (fabs would poison the envelope with inf) and must not be counted.
//
// Returns the number of finite children, that is, successful transitions.
inline __attribute__((always_inline)) static uint64_t
projected_wild_update_scalar(
    double partial_score, double rounding_error_base,
    float const* children, double* best, double* max_rounding_error,
    size_t count) {
  uint64_t finite = 0;
  for (size_t i = 0; i < count; ++i) {
    double const child = double(children[i]);
    if (child == -HUGE_VAL) continue;
    ++finite;
    best[i] = std::max(best[i], partial_score + child);
    double rounding_error = rounding_error_base;
    rounding_error += fabs(child);
    rounding_error += 1.0;
    rounding_error *= DBL_EPSILON * 4.0;
    max_rounding_error[i] =
        std::max(max_rounding_error[i], rounding_error);
  }
  return finite;
}

// Which wildcard-update kernel the bottom-up evaluator runs. Resolved once per
// preprocessing run, never per bag or per action.
enum ProjectedKernel {
  PROJECTED_KERNEL_SCALAR,
  PROJECTED_KERNEL_AVX2,
  PROJECTED_KERNEL_VERIFY  // run both and compare, for tests and debugging
};

#if defined(__x86_64__)

// Four wildcard counts at a time, in the scalar kernel's exact arithmetic.
//
// std::max(current, candidate) is (current < candidate) ? candidate : current,
// which maxpd reproduces exactly as _mm256_max_pd(candidate, current):
// MAXPD returns its second operand on ties, on either operand being NaN, and
// for +0.0 against -0.0, which is what std::max returns in each of those
// cases. The dead-child test is _CMP_NEQ_UQ rather than the ordered form so
// that a NaN child counts as live, matching the scalar `child == -HUGE_VAL`.
// A NaN child then loses both maxima anyway, exactly as it does scalar-side.
__attribute__((target("avx2"))) static uint64_t
projected_wild_update_avx2(
    double partial_score, double rounding_error_base,
    float const* children, double* best, double* max_rounding_error,
    size_t count) {
  __m256d const partial = _mm256_set1_pd(partial_score);
  __m256d const error_base = _mm256_set1_pd(rounding_error_base);
  __m256d const one = _mm256_set1_pd(1.0);
  __m256d const epsilon = _mm256_set1_pd(DBL_EPSILON * 4.0);
  __m256d const dead = _mm256_set1_pd(-HUGE_VAL);
  __m256d const sign_bit = _mm256_set1_pd(-0.0);
  uint64_t finite = 0;
  size_t i = 0;
  for (; i + 4 <= count; i += 4) {
    // float -> double is exact, so the widened lanes hold the same values the
    // scalar kernel's conversion produces.
    __m256d const child =
        _mm256_cvtps_pd(_mm_loadu_ps(children + i));
    __m256d const live =
        _mm256_cmp_pd(child, dead, _CMP_NEQ_UQ);
    __m256d const candidate = _mm256_add_pd(partial, child);
    __m256d const rounding_error = _mm256_mul_pd(
        _mm256_add_pd(
            _mm256_add_pd(
                error_base, _mm256_andnot_pd(sign_bit, child)),
            one),
        epsilon);
    // The std::vector data is not guaranteed 32-byte aligned and the range
    // starts at wild_length, so every access is unaligned.
    __m256d const best_now = _mm256_loadu_pd(best + i);
    __m256d const error_now = _mm256_loadu_pd(max_rounding_error + i);
    // A dead lane contributes its own current value, so it cannot move either
    // maximum; blending before the max also keeps inf out of the envelope.
    _mm256_storeu_pd(
        best + i,
        _mm256_max_pd(
            _mm256_blendv_pd(best_now, candidate, live), best_now));
    _mm256_storeu_pd(
        max_rounding_error + i,
        _mm256_max_pd(
            _mm256_blendv_pd(error_now, rounding_error, live),
            error_now));
    finite += uint64_t(__builtin_popcount(
        unsigned(_mm256_movemask_pd(live))));
  }
  // score_wild_span is not a multiple of four on any real workload, so the
  // tail always runs.
  return finite + projected_wild_update_scalar(
      partial_score, rounding_error_base, children + i, best + i,
      max_rounding_error + i, count - i);
}

#endif  // defined(__x86_64__)

// The non-scalar kernels, kept out of line so the scalar path stays a plain
// inlined loop. `mismatch` is set, never cleared, when verification fails.
static uint64_t projected_wild_update_checked(
    ProjectedKernel kernel, double partial_score,
    double rounding_error_base, float const* children, double* best,
    double* max_rounding_error, size_t count, double* scratch_best,
    double* scratch_error, bool* mismatch) {
#if defined(__x86_64__)
  if (kernel == PROJECTED_KERNEL_AVX2)
    return projected_wild_update_avx2(
        partial_score, rounding_error_base, children, best,
        max_rounding_error, count);
  size_t const bytes = count * sizeof(double);
  memcpy(scratch_best, best, bytes);
  memcpy(scratch_error, max_rounding_error, bytes);
  uint64_t const vector_finite = projected_wild_update_avx2(
      partial_score, rounding_error_base, children, best,
      max_rounding_error, count);
  uint64_t const scalar_finite = projected_wild_update_scalar(
      partial_score, rounding_error_base, children, scratch_best,
      scratch_error, count);
  if (vector_finite != scalar_finite ||
      memcmp(best, scratch_best, bytes) != 0 ||
      memcmp(max_rounding_error, scratch_error, bytes) != 0)
    *mismatch = true;
  return scalar_finite;
#else
  (void)kernel;
  (void)scratch_best;
  (void)scratch_error;
  (void)mismatch;
  return projected_wild_update_scalar(
      partial_score, rounding_error_base, children, best,
      max_rounding_error, count);
#endif
}

// NUTRIMATIC_PROJECTED_SIMD selects the wildcard-update kernel: unset or "1"
// takes the best available vector kernel, "verify" runs it against the scalar
// kernel and fails preprocessing on any difference, and anything else, "0"
// included, stays scalar.
static ProjectedKernel projected_kernel_choice() {
  char const* mode = getenv("NUTRIMATIC_PROJECTED_SIMD");
  bool const verify = mode != NULL && strcmp(mode, "verify") == 0;
  if (!verify && mode != NULL && mode[0] != '\0' &&
      strcmp(mode, "1") != 0)
    return PROJECTED_KERNEL_SCALAR;
#if defined(__x86_64__)
  if (__builtin_cpu_supports("avx2"))
    return verify ? PROJECTED_KERNEL_VERIFY : PROJECTED_KERNEL_AVX2;
#endif
  return PROJECTED_KERNEL_SCALAR;
}

static char const* projected_kernel_name(ProjectedKernel kernel) {
  if (kernel == PROJECTED_KERNEL_AVX2) return "avx2";
  if (kernel == PROJECTED_KERNEL_VERIFY) return "avx2 verify";
  return "scalar";
}

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

static bool projected_score_experiment(
    size_t* forced_exact_letters, bool* has_forced_exact_letters) {
  *forced_exact_letters = 0;
  *has_forced_exact_letters = false;
  char const* forced = getenv("NUTRIMATIC_PROJECTED_SCORE_D");
  if (forced != NULL && forced[0] != '\0') {
    errno = 0;
    char* end = NULL;
    unsigned long long const parsed = strtoull(forced, &end, 10);
    if (errno == 0 && end != forced && *end == '\0' &&
        parsed <= SIZE_MAX) {
      *forced_exact_letters = size_t(parsed);
      *has_forced_exact_letters = true;
      return true;
    }
  }
  char const* enabled = getenv("NUTRIMATIC_PROJECTED_SCORE");
  return enabled != NULL && enabled[0] != '\0' &&
      strcmp(enabled, "0") != 0;
}

static bool projected_action_quotient_enabled() {
  char const* enabled =
      getenv("NUTRIMATIC_PROJECTED_ACTION_QUOTIENT");
  return enabled == NULL || enabled[0] == '\0' ||
      strcmp(enabled, "0") != 0;
}

static bool projected_bottom_up_enabled() {
  char const* enabled =
      getenv("NUTRIMATIC_PROJECTED_BOTTOM_UP");
  return enabled == NULL || enabled[0] == '\0' ||
      strcmp(enabled, "0") != 0;
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
    case DfsAnagramSearch::SCORE_BOUND_DENSE:
      return "dense";
    case DfsAnagramSearch::SCORE_BOUND_PREFIX:
      return "dense prefix";
    case DfsAnagramSearch::SCORE_BOUND_PROJECTED:
      return "projected dense";
    case DfsAnagramSearch::SCORE_BOUND_OFF:
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
    current_letters_left(0),
    score_exact_mask(0),
    score_state_count(0),
    score_effective_states(0),
    score_exact_letters(0),
    score_wild_letters(0),
    score_wild_span(1),
    score_projection_requested(false),
    exact_state_encodable(false),
    projected_actions_ready(false),
    projected_quotient_enabled(true),
    hot_classes_ready(false),
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
    completable_checked(0),
    completable_bound_reject_count(0),
    completable_exact_bound_accept_count(0),
    completable_exact_validation_count(0),
    exact_memo_states(0),
    exact_memo_hit_count(0),
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

  std::vector<DfsAnagramClass> const& all_classes = class_list->classes();
  best_member_log_scores.reserve(all_classes.size());
  for (size_t i = 0; i < all_classes.size(); ++i) {
    assert(!all_classes[i].members.empty());
    DfsClassMember const& best = all_classes[i].members[0];
    assert(best.count > 0);
    best_member_log_scores.push_back(score_model.first_segment_log_score(
        best.count, best.word_count > 1));
  }
}

bool DfsAnagramSearch::prepare_hot_classes() {
  static_assert(sizeof(FitClass) == 16,
                "FitClass must remain four per cache line");
  fit_classes.reset();
  score_key_deltas.reset();
  score_wild_lengths.reset();
  packed_letters.reset();

  std::vector<DfsAnagramClass> const& classes = class_list->classes();
  if (classes.empty() || classes.size() > UINT32_MAX ||
      classes.size() > SIZE_MAX / sizeof(FitClass) ||
      classes.size() > SIZE_MAX / sizeof(uint16_t) ||
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

  uint64_t* score_deltas = static_cast<uint64_t*>(
      allocate_aligned(classes.size() * sizeof(uint64_t)));
  if (score_deltas == NULL) return false;
  std::unique_ptr<uint64_t, AlignedFree> new_score_deltas(score_deltas);

  uint16_t* wild_lengths = static_cast<uint16_t*>(
      allocate_aligned(classes.size() * sizeof(uint16_t)));
  if (wild_lengths == NULL) return false;
  std::unique_ptr<uint16_t, AlignedFree> new_wild_lengths(wild_lengths);

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
    uint64_t score_delta = 0;
    uint32_t wild_length = 0;
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
      if ((score_exact_mask & (UINT64_C(1) << rank)) != 0) {
        if (!checked_multiply_u64(
                count, score_multipliers[rank], &term) ||
            !checked_add_u64(score_delta, term, &next))
          return false;
        score_delta = next;
      } else {
        wild_length += count;
      }
    }

    fit[ci].support_mask = support;
    fit[ci].letters_offset = uint32_t(offset);
    fit[ci].packed_length_and_count =
        uint32_t(source.key.size()) |
        (uint32_t(source.letters.size()) << 16) |
        (repeated << 24);
    uint64_t flat_score_delta;
    if (!checked_multiply_u64(
            score_delta, uint64_t(score_wild_span), &flat_score_delta) ||
        !checked_add_u64(
            flat_score_delta, uint64_t(wild_length),
            &flat_score_delta))
      return false;
    score_deltas[ci] = flat_score_delta;
    wild_lengths[ci] = uint16_t(wild_length);
    offset = write;
  }

  fit_classes = std::move(new_fit);
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
    std::vector<DfsAnagramClass> const& classes =
        class_list->classes();
    size_t max_length = 0;
    for (size_t i = 0; i < classes.size(); ++i)
      max_length = std::max(max_length, classes[i].key.size());
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
        size_t const length = classes[i].key.size();
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

bool DfsAnagramSearch::prepare_projected_actions() {
  static_assert(sizeof(ProjectedAction) == 48,
                "ProjectedAction must remain three 16-byte blocks, the last "
                "one partly free");
  projected_actions.clear();
  projected_action_support.clear();
  projected_repeated_requirements.clear();
  projected_bucket_starts.fill(0);
  projected_actions_ready = false;
  projected_quotient_enabled =
      projected_action_quotient_enabled();

  std::vector<DfsAnagramClass> const& classes =
      class_list->classes();
  if (classes.size() > UINT32_MAX) return false;

  struct DeltaClass {
    uint64_t delta;
    uint32_t class_id;
  };
  std::vector<DeltaClass> by_delta;
  try {
    by_delta.reserve(classes.size());
    for (size_t i = 0; i < classes.size(); ++i) {
      DeltaClass entry;
      entry.delta = score_key_deltas.get()[i];
      entry.class_id = uint32_t(i);
      by_delta.push_back(entry);
    }
    if (projected_quotient_enabled) {
      std::sort(
          by_delta.begin(), by_delta.end(),
          [&](DeltaClass const& a, DeltaClass const& b) {
            if (a.delta != b.delta) return a.delta < b.delta;
            double const a_score =
                best_member_log_scores[a.class_id];
            double const b_score =
                best_member_log_scores[b.class_id];
            if (a_score != b_score) return a_score > b_score;
            return a.class_id < b.class_id;
          });
    }

    std::vector<uint32_t> representatives;
    representatives.reserve(by_delta.size());
    for (size_t i = 0; i < by_delta.size(); ++i) {
      if (!projected_quotient_enabled || i == 0 ||
          by_delta[i].delta != by_delta[i - 1].delta)
        representatives.push_back(by_delta[i].class_id);
    }

    static size_t const WILDCARD_BUCKET = DFS_SYMBOL_COUNT;
    std::array<size_t, DFS_SYMBOL_COUNT + 1> bucket_counts;
    bucket_counts.fill(0);
    for (size_t i = 0; i < representatives.size(); ++i) {
      uint32_t const id = representatives[i];
      uint64_t const exact_support =
          fit_classes.get()[id].support_mask & score_exact_mask;
      size_t const bucket = exact_support == 0
          ? WILDCARD_BUCKET
          : size_t(__builtin_ctzll(exact_support));
      ++bucket_counts[bucket];
    }

    size_t offset = 0;
    for (size_t bucket = 0; bucket <= WILDCARD_BUCKET; ++bucket) {
      projected_bucket_starts[bucket] = offset;
      offset += bucket_counts[bucket];
    }
    projected_bucket_starts[WILDCARD_BUCKET + 1] = offset;
    // The mask no longer lives in the action, so carry it alongside while the
    // buckets are sorted and split the two arrays apart afterwards. That keeps
    // the sidecar index-parallel by construction rather than by convention.
    struct SortableAction {
      ProjectedAction action;
      uint64_t exact_support;
    };
    std::vector<SortableAction> ordered(offset);
    std::array<size_t, DFS_SYMBOL_COUNT + 1> write;
    for (size_t bucket = 0; bucket <= WILDCARD_BUCKET; ++bucket)
      write[bucket] = projected_bucket_starts[bucket];
    for (size_t i = 0; i < representatives.size(); ++i) {
      uint32_t const id = representatives[i];
      uint64_t const exact_support =
          fit_classes.get()[id].support_mask & score_exact_mask;
      size_t const bucket = exact_support == 0
          ? WILDCARD_BUCKET
          : size_t(__builtin_ctzll(exact_support));
      FitClass const& fit = fit_classes.get()[id];
      ProjectedAction action;
      action.score_key_delta = score_key_deltas.get()[id];
      double const class_score = best_member_log_scores[id];
      action.partial_score = class_score + segment_boundary_log_score;
      action.rounding_error_base =
          fabs(class_score) + fabs(segment_boundary_log_score);
      action.repeated_offset =
          uint32_t(projected_repeated_requirements.size());
      action.packed_lengths =
          hot_letter_length(fit.packed_length_and_count) |
          (uint32_t(score_wild_lengths.get()[id]) << 16);
      action.repeated_count = 0;
      uint32_t const* requirements =
          packed_letters.get() + fit.letters_offset;
      uint32_t const repeated =
          hot_repeated_count(fit.packed_length_and_count);
      for (uint32_t repeated_index = 0;
           repeated_index < repeated; ++repeated_index) {
        uint32_t const requirement = requirements[repeated_index];
        uint32_t const rank = packed_rank(requirement);
        if ((score_exact_mask & (UINT64_C(1) << rank)) == 0)
          continue;
        projected_repeated_requirements.push_back(requirement);
        ++action.repeated_count;
      }
      ordered[write[bucket]].action = action;
      ordered[write[bucket]].exact_support = exact_support;
      ++write[bucket];
    }

    for (size_t bucket = 0; bucket <= WILDCARD_BUCKET; ++bucket) {
      std::sort(
          ordered.begin() + projected_bucket_starts[bucket],
          ordered.begin() + projected_bucket_starts[bucket + 1],
          [](SortableAction const& a, SortableAction const& b) {
            uint32_t const a_length =
                projected_total_length(a.action.packed_lengths);
            uint32_t const b_length =
                projected_total_length(b.action.packed_lengths);
            if (a_length != b_length) return a_length > b_length;
            return a.action.score_key_delta < b.action.score_key_delta;
          });
    }

    projected_actions.resize(offset);
    projected_action_support.resize(offset);
    for (size_t i = 0; i < offset; ++i) {
      projected_actions[i] = ordered[i].action;
      projected_action_support[i] = ordered[i].exact_support;
    }
  } catch (...) {
    projected_actions.clear();
    projected_action_support.clear();
    projected_repeated_requirements.clear();
    projected_bucket_starts.fill(0);
    return false;
  }
  assert(projected_action_support.size() == projected_actions.size());

  projected_actions_ready = true;
  return true;
}

void DfsAnagramSearch::clear_score_bounds() {
  bound_mode = SCORE_BOUND_OFF;
  bound_values.reset();
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

void DfsAnagramSearch::prepare_score_bounds(
    uint64_t state_count, bool requested) {
  clear_score_bounds();
  if (!hot_classes_ready || !requested ||
      !score_bound_arithmetic_supported() ||
      score_cache_budget < CACHE_ALIGNMENT)
    return;

  if (score_projection_requested) {
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
    if (projected_bottom_up_enabled() && bottom_up_eligible) {
      float* values = static_cast<float*>(
          allocate_aligned(float_bytes));
      if (values == NULL) return;
      bound_plain_float_values.reset(values);
    } else {
      AtomicFloatWord* values = static_cast<AtomicFloatWord*>(
          allocate_aligned(float_bytes));
      if (values == NULL) return;
      for (size_t i = 0; i < size_t(score_effective_states); ++i) {
        new (&values[i]) AtomicFloatWord;
        values[i].value.store(
            FLOAT_BOUND_UNSEEN, std::memory_order_relaxed);
      }
      bound_float_values.reset(values);
    }
    bound_capacity = size_t(score_effective_states);
    bound_value_bytes = sizeof(float);
    bound_complete = true;
    bound_mode = SCORE_BOUND_PROJECTED;
    bound_charged_bytes = float_bytes;
    return;
  }

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
  if (double_size_ok && double_bytes <= score_cache_budget) {
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
        float_size_ok && float_bytes <= score_cache_budget;
    uint64_t const available_entries =
        score_cache_budget / sizeof(float);
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
  if (bound_plain_float_values.get() != NULL) {
    bound_plain_float_values.get()[size_t(key)] =
        round_float_score_bound_up(value);
    return true;
  }
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
    assert(bound_plain_float_values.get() == NULL);
    bound_float_values.get()[size_t(key)].value.store(
        float_to_bits(round_float_score_bound_up(value)),
        std::memory_order_release);
  }
}

bool DfsAnagramSearch::prepare_phase_two(
    int progress_factor, bool allow_cache_fallback, bool dense_cache,
    int exact_letters, bool score_bounds_requested) {
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
  exact_multipliers.fill(0);
  if (encodable) {
    for (int rank = DFS_SYMBOL_COUNT - 1; rank >= 0; --rank) {
      exact_multipliers[size_t(rank)] = state_count;
      uint64_t const radix = uint64_t(bag[size_t(rank)]) + 1;
      if (!checked_multiply_u64(state_count, radix, &state_count)) {
        encodable = false;
        break;
      }
    }
  }
  exact_state_encodable = encodable;
  bag_mask = 0;
  for (int rank = 0; rank < DFS_SYMBOL_COUNT; ++rank)
    if (bag[size_t(rank)] != 0)
      bag_mask |= UINT64_C(1) << rank;

  size_t forced_exact_letters = 0;
  bool has_forced_exact_letters = false;
  bool const projection_experiment = projected_score_experiment(
      &forced_exact_letters, &has_forced_exact_letters);
  if (exact_letters >= 0) {
    forced_exact_letters = size_t(exact_letters);
    has_forced_exact_letters = true;
  }
  score_projection_requested = !dense_cache || projection_experiment;
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

  if (encodable && score_projection_requested) {
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
        bool const states_ok = checked_multiply_u64(
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
          if (!checked_multiply_u64(
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
      if (!checked_multiply_u64(
              exact_product, uint64_t(bag[size_t(rank)]) + 1,
              &exact_product)) {
        encodable = false;
        break;
      }
    }
    if (encodable &&
        !checked_multiply_u64(
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
  } else if (encodable) {
    score_multipliers.fill(0);
    uint64_t exact_score_states = 1;
    for (int rank = DFS_SYMBOL_COUNT - 1; rank >= 0; --rank) {
      score_multipliers[size_t(rank)] = exact_score_states;
      uint64_t const radix = uint64_t(bag[size_t(rank)]) + 1;
      if (!checked_multiply_u64(
              exact_score_states, radix, &exact_score_states)) {
        encodable = false;
        break;
      }
    }
    assert(!encodable || exact_score_states == state_count);
    if (encodable && bag_mask != 0) {
      int const root_rank = __builtin_ctzll(bag_mask);
      uint64_t const root_radix =
          uint64_t(bag[size_t(root_rank)]) + 1;
      score_effective_states =
          (state_count / root_radix) * (root_radix - 1);
    }
  }

  current_score_key = encodable ? score_state_count - 1 : 0;
  current_letters_left = letters.size();
  hot_classes_ready = encodable && prepare_hot_classes();
  if (hot_classes_ready && score_projection_requested)
    prepare_projected_actions();
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
      dfs_diagnostic(
          "phase 2 preflight: theoretical state count exceeds the "
          "supported range; dense score-table size and minimum -C "
          "unavailable\n");
    } else if (bag_mask != 0 && dense_sizes_available) {
      size_t const minimum_mib =
          float_bytes / MIB + size_t(float_bytes % MIB != 0);
      dfs_diagnostic(
          "phase 2 preflight: %llu theoretical states, "
          "%llu effective non-root states\n",
          (unsigned long long) state_count,
          (unsigned long long) effective_states);
      dfs_diagnostic(
          "phase 2 preflight: "
          "%zu double/%zu float dense score-table bytes, "
          "minimum -C %zu MiB (%zu bytes)\n",
          double_bytes, float_bytes, minimum_mib, float_bytes);
    } else if (bag_mask != 0) {
      dfs_diagnostic(
          "phase 2 preflight: %llu theoretical states, "
          "%llu effective non-root states\n",
          (unsigned long long) state_count,
          (unsigned long long) effective_states);
      dfs_diagnostic(
          "phase 2 preflight: dense score-table size and minimum -C "
          "exceed the supported range\n");
    } else {
      dfs_diagnostic(
          "phase 2 preflight: empty bag; no score table needed\n");
    }
    if (score_projection_requested && encodable) {
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
            "%zu projected actions (quotient %s)\n",
            class_list->classes().size(),
            projected_actions.size(),
            projected_quotient_enabled ? "on" : "off");
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
    size_t required_bytes = float_bytes;
    bool size_available = dense_sizes_available;
    char const* mode_name = "dense";
    if (score_projection_requested) {
      mode_name = "projected dense";
      size_available = dense_bound_requirements(
          score_effective_states, sizeof(float), &required_bytes);
    }
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
  prepare_score_bounds(state_count, score_bounds_applicable);
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
    } else {
      bool const ran_parallel =
          compute_score_bound_parallel(requested_preprocess_threads);
      if (!ran_parallel)
        root_score_bound = compute_score_bound();
      if (!ran_parallel) root_score_bound_ready = true;
    }
  }

  path.clear();
  path.reserve(letters.size());
  progress_enabled = progress != NULL;
  progress_interval =
      int64_t(100000) * int64_t(std::max(progress_factor, 1));
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
    if (bound_mode == SCORE_BOUND_PREFIX)
      dfs_diagnostic(
          "phase 2: dense-prefix bounds will be constructed lazily "
          "during search for score keys below %zu once a score floor "
          "is available\n",
          bound_capacity);
  }
  return true;
}

bool DfsAnagramSearch::run(DfsSolutionSink* sink,
                           int progress_factor,
                           bool allow_cache_fallback,
                           bool dense_cache,
                           int exact_letters,
                           bool verbose) {
  bool const score_bounds_requested =
      sink != NULL && sink->supports_score_pruning();
  if (!prepare_phase_two(
          progress_factor, allow_cache_fallback, dense_cache,
          exact_letters, score_bounds_requested))
    return false;

  typedef std::chrono::steady_clock PhaseClock;
  PhaseClock::time_point const search_start = PhaseClock::now();
  if (!hot_classes_ready) {
    walk_unoptimized(letters.size(), 0, 0, 0.0, sink);
  } else {
    bool const parallel_eligible =
        requested_search_threads > 1 &&
        (sink == NULL || sink->supports_parallel_search()) &&
        bound_mode != SCORE_BOUND_PREFIX;
    if (parallel_eligible) {
      run_parallel_search(
          sink, requested_search_threads,
          search_task_target(requested_search_threads),
          size_t(std::max(progress_factor, 1)), verbose);
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

uint64_t DfsAnagramSearch::exact_bag_number(
    SearchWorker const& worker) const {
  assert(exact_state_encodable);
  uint64_t key = 0;
  for (size_t rank = 0; rank < DFS_SYMBOL_COUNT; ++rank)
    key += uint64_t(worker.bag[rank]) * exact_multipliers[rank];
  return key;
}

std::string DfsAnagramSearch::exact_bag_string(
    SearchWorker const& worker) const {
  std::string key;
  key.resize(DFS_SYMBOL_COUNT * sizeof(uint32_t));
  for (size_t rank = 0; rank < DFS_SYMBOL_COUNT; ++rank)
    memcpy(&key[rank * sizeof(uint32_t)], &worker.bag[rank],
           sizeof(uint32_t));
  return key;
}

bool DfsAnagramSearch::exact_memo_lookup(
    SearchWorker* worker, bool* value) {
  if (exact_state_encodable) {
    uint64_t const key = exact_bag_number(*worker);
    size_t const shard =
        std::hash<uint64_t>()(key) % EXACT_MEMO_SHARDS;
    std::lock_guard<std::mutex> lock(exact_memo_mutexes[shard]);
    auto const found = exact_number_memo[shard].find(key);
    if (found == exact_number_memo[shard].end()) return false;
    *value = found->second;
  } else {
    std::string const key = exact_bag_string(*worker);
    size_t const shard =
        std::hash<std::string>()(key) % EXACT_MEMO_SHARDS;
    std::lock_guard<std::mutex> lock(exact_memo_mutexes[shard]);
    auto const found = exact_string_memo[shard].find(key);
    if (found == exact_string_memo[shard].end()) return false;
    *value = found->second;
  }
  ++worker->exact_memo_hits;
  return true;
}

void DfsAnagramSearch::exact_memo_store(
    SearchWorker* worker, bool value) {
  bool inserted;
  if (exact_state_encodable) {
    uint64_t const key = exact_bag_number(*worker);
    size_t const shard =
        std::hash<uint64_t>()(key) % EXACT_MEMO_SHARDS;
    std::lock_guard<std::mutex> lock(exact_memo_mutexes[shard]);
    inserted = exact_number_memo[shard].emplace(key, value).second;
  } else {
    std::string const key = exact_bag_string(*worker);
    size_t const shard =
        std::hash<std::string>()(key) % EXACT_MEMO_SHARDS;
    std::lock_guard<std::mutex> lock(exact_memo_mutexes[shard]);
    inserted = exact_string_memo[shard].emplace(key, value).second;
  }
  if (inserted) ++worker->exact_memo_states;
}

bool DfsAnagramSearch::exact_class_fits(
    size_t class_index, SearchWorker const& worker) const {
  if (hot_classes_ready)
    return hot_class_fits(uint32_t(class_index), worker);

  DfsAnagramClass const& candidate =
      class_list->classes()[class_index];
  std::array<int, DFS_SYMBOL_COUNT> const& symbol_to_rank =
      class_list->symbol_to_rank();
  for (size_t i = 0; i < candidate.letters.size(); ++i) {
    size_t const rank = size_t(
        symbol_to_rank[size_t(candidate.letters[i].first)]);
    if (worker.bag[rank] < candidate.letters[i].second) return false;
  }
  return true;
}

void DfsAnagramSearch::subtract_exact_class(
    size_t class_index, SearchWorker* worker,
    uint64_t* parent_bag_mask) {
  *parent_bag_mask = worker->bag_mask;
  if (hot_classes_ready) {
    FitClass const& fit = fit_classes.get()[class_index];
    uint32_t const* requirements =
        packed_letters.get() + fit.letters_offset;
    uint32_t const requirement_count =
        hot_requirement_count(fit.packed_length_and_count);
    for (uint32_t i = 0; i < requirement_count; ++i) {
      uint32_t const requirement = requirements[i];
      uint32_t const rank = packed_rank(requirement);
      uint32_t& remaining = worker->bag[rank];
      remaining -= packed_count(requirement);
      if (remaining == 0) worker->bag_mask &= ~(UINT64_C(1) << rank);
    }
    worker->score_key -= score_key_deltas.get()[class_index];
    return;
  }

  DfsAnagramClass const& candidate =
      class_list->classes()[class_index];
  std::array<int, DFS_SYMBOL_COUNT> const& symbol_to_rank =
      class_list->symbol_to_rank();
  for (size_t i = 0; i < candidate.letters.size(); ++i) {
    size_t const rank = size_t(
        symbol_to_rank[size_t(candidate.letters[i].first)]);
    worker->bag[rank] -= candidate.letters[i].second;
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
        packed_letters.get() + fit.letters_offset;
    uint32_t const requirement_count =
        hot_requirement_count(fit.packed_length_and_count);
    worker->score_key += score_key_deltas.get()[class_index];
    for (uint32_t i = 0; i < requirement_count; ++i) {
      uint32_t const requirement = requirements[i];
      worker->bag[packed_rank(requirement)] += packed_count(requirement);
    }
  } else {
    DfsAnagramClass const& candidate =
        class_list->classes()[class_index];
    std::array<int, DFS_SYMBOL_COUNT> const& symbol_to_rank =
        class_list->symbol_to_rank();
    for (size_t i = 0; i < candidate.letters.size(); ++i) {
      size_t const rank = size_t(
          symbol_to_rank[size_t(candidate.letters[i].first)]);
      worker->bag[rank] += candidate.letters[i].second;
    }
  }
  worker->bag_mask = parent_bag_mask;
}

bool DfsAnagramSearch::exact_remainder_completable(
    SearchWorker* worker, size_t letters_left,
    ExactResultSource* source) {
  if (letters_left == 0) {
    if (source != NULL) *source = EXACT_RESULT_EMPTY;
    return true;
  }

  bool memoized;
  if (exact_memo_lookup(worker, &memoized)) {
    if (source != NULL) *source = EXACT_RESULT_MEMO;
    return memoized;
  }

  Reachability const reachability =
      cached_reachability(worker->score_key, false);
  if (reachability != REACHABILITY_UNKNOWN) {
    bool const result = reachability == REACHABILITY_YES;
    exact_memo_store(worker, result);
    if (source != NULL)
      *source = result ? EXACT_RESULT_BOUND_YES : EXACT_RESULT_BOUND_NO;
    return result;
  }

  if (source != NULL) *source = EXACT_RESULT_SEARCH;
  ++worker->nodes;
  if (DFS_UNLIKELY(progress_enabled &&
                   worker->nodes == worker->next_progress))
    report_search_progress(worker);

  bool result = false;
  if (worker->bag_mask != 0) {
    int const rank = __builtin_ctzll(worker->bag_mask);
    int const forced_symbol =
        class_list->rank_to_symbol()[size_t(rank)];
    size_t begin = class_list->candidate_begin(forced_symbol);
    size_t const end = class_list->candidate_end(forced_symbol);
    if (hot_classes_ready)
      begin = first_length_candidate(begin, end, letters_left);

    std::vector<DfsAnagramClass> const& classes =
        class_list->classes();
    for (size_t class_index = begin; class_index < end; ++class_index) {
      size_t const candidate_length = classes[class_index].key.size();
      if (candidate_length > letters_left ||
          !exact_class_fits(class_index, *worker))
        continue;
      uint64_t parent_bag_mask;
      subtract_exact_class(class_index, worker, &parent_bag_mask);
      result = exact_remainder_completable(
          worker, letters_left - candidate_length);
      restore_exact_class(
          class_index, worker, parent_bag_mask);
      if (result) break;
    }
  }
  exact_memo_store(worker, result);
  return result;
}

bool DfsAnagramSearch::find_completable_classes(
    std::vector<bool>* completable, int progress_factor,
    bool allow_cache_fallback, bool dense_cache, int exact_letters) {
  assert(completable != NULL);
  if (!prepare_phase_two(
          progress_factor, allow_cache_fallback, dense_cache,
          exact_letters, true))
    return false;

  std::vector<DfsAnagramClass> const& classes =
      class_list->classes();
  for (size_t shard = 0; shard < EXACT_MEMO_SHARDS; ++shard) {
    exact_number_memo[shard].clear();
    exact_string_memo[shard].clear();
    if (exact_state_encodable)
      exact_number_memo[shard].reserve(
          classes.size() / EXACT_MEMO_SHARDS + 1);
    else
      exact_string_memo[shard].reserve(
          classes.size() / EXACT_MEMO_SHARDS + 1);
  }
  completable_checked = 0;
  completable_bound_reject_count = 0;
  completable_exact_bound_accept_count = 0;
  completable_exact_validation_count = 0;
  exact_memo_states = 0;
  exact_memo_hit_count = 0;

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
          &worker, letters.size() - classes[class_index].key.size(),
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
  }
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
    uint32_t class_index, SearchWorker const& worker) const {
  FitClass const& candidate = fit_classes.get()[class_index];
  if ((candidate.support_mask & ~worker.bag_mask) != 0) return false;
  return hot_class_multiplicity_fits(class_index, worker);
}

bool DfsAnagramSearch::hot_class_multiplicity_fits(
    uint32_t class_index, SearchWorker const& worker) const {
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

bool DfsAnagramSearch::projected_action_fits(
    size_t action_index, BoundWorker const& worker) const {
  ProjectedAction const& action = projected_actions[action_index];
  if (projected_wild_length(action.packed_lengths) >
      worker.wild_left)
    return false;
  if ((projected_action_support[action_index] & ~worker.bag_mask) != 0)
    return false;
  uint32_t const* repeated = action.repeated_count == 0
      ? NULL
      : &projected_repeated_requirements[action.repeated_offset];
  for (uint32_t i = 0; i < action.repeated_count; ++i) {
    uint32_t const requirement = repeated[i];
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

size_t DfsAnagramSearch::first_projected_length_candidate(
    size_t begin, size_t end, size_t letters_left) const {
  while (begin < end) {
    size_t const middle = begin + (end - begin) / 2;
    size_t const candidate_length = projected_total_length(
        projected_actions[middle].packed_lengths);
    if (candidate_length > letters_left)
      begin = middle + 1;
    else
      end = middle;
  }
  return begin;
}

void DfsAnagramSearch::consider_bound_candidate(
    uint32_t class_index, double* best, double* max_rounding_error) {
  FitClass const& fit = fit_classes.get()[class_index];
  double const class_score = best_member_log_scores[class_index];
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
  current_score_key -= score_key_deltas.get()[class_index];
  current_letters_left -= candidate_length;
  double const child = compute_score_bound();
  current_letters_left += candidate_length;
  current_score_key += score_key_deltas.get()[class_index];
  for (uint32_t i = 0; i < requirement_count; ++i) {
    uint32_t const requirement = requirements[i];
    bag[packed_rank(requirement)] += packed_count(requirement);
  }
  bag_mask = parent_bag_mask;

  if (child == -HUGE_VAL) return;
  ++bound_transitions;
  double const partial = class_score + segment_boundary_log_score;
  double const candidate_bound = partial + child;
  *best = std::max(*best, candidate_bound);
  *max_rounding_error = std::max(
      *max_rounding_error,
      score_candidate_rounding_error(
          class_score, segment_boundary_log_score, child));
}

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

  double best = -HUGE_VAL;
  double max_rounding_error = 0.0;
  for (size_t class_index = begin;
       class_index < end; ++class_index) {
    uint32_t const id = uint32_t(class_index);
    if (!hot_class_fits(id)) continue;
    consider_bound_candidate(id, &best, &max_rounding_error);
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
    assert(bound_complete &&
           current_score_key == score_state_count - 1);
  }
  return stored_best;
}

void DfsAnagramSearch::consider_parallel_bound_candidate(
    uint32_t class_index, BoundWorker* worker, double* best,
    double* max_rounding_error) {
  FitClass const& fit = fit_classes.get()[class_index];
  double const class_score = best_member_log_scores[class_index];
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
  double const partial = class_score + segment_boundary_log_score;
  double const candidate_bound = partial + child;
  *best = std::max(*best, candidate_bound);
  *max_rounding_error = std::max(
      *max_rounding_error,
      score_candidate_rounding_error(
          class_score, segment_boundary_log_score, child));
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

  double best = -HUGE_VAL;
  double max_rounding_error = 0.0;
  for (size_t class_index = begin; class_index < end; ++class_index) {
    uint32_t const id = uint32_t(class_index);
    if (!hot_class_fits(id, *worker)) continue;
    consider_parallel_bound_candidate(
        id, worker, &best, &max_rounding_error);
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

void DfsAnagramSearch::consider_projected_bound_candidate(
    size_t action_index, BoundWorker* worker, double* best,
    double* max_rounding_error) {
  ProjectedAction const& action = projected_actions[action_index];
  uint64_t const exact_support =
      projected_action_support[action_index];
  ++worker->fitting_transitions;
  size_t const candidate_length =
      projected_total_length(action.packed_lengths);
  size_t const wild_length =
      projected_wild_length(action.packed_lengths);
  uint64_t const parent_bag_mask = worker->bag_mask;
  uint64_t single_support = exact_support;
  uint32_t const* repeated = action.repeated_count == 0
      ? NULL
      : &projected_repeated_requirements[action.repeated_offset];
  for (uint32_t i = 0; i < action.repeated_count; ++i) {
    uint32_t const requirement = repeated[i];
    uint32_t const rank = packed_rank(requirement);
    uint32_t& remaining = worker->bag[rank];
    remaining -= packed_count(requirement);
    worker->bag_mask &=
        ~(uint64_t(remaining == 0) << rank);
    single_support &= ~(UINT64_C(1) << rank);
  }
  while (single_support != 0) {
    uint32_t const rank = uint32_t(__builtin_ctzll(single_support));
    uint32_t& remaining = worker->bag[rank];
    --remaining;
    worker->bag_mask &=
        ~(uint64_t(remaining == 0) << rank);
    single_support &= single_support - 1;
  }
  worker->score_key -= action.score_key_delta;
  worker->letters_left -= candidate_length;
  worker->wild_left -= wild_length;
  double const child = compute_projected_score_bound(worker);
  worker->wild_left += wild_length;
  worker->letters_left += candidate_length;
  worker->score_key += action.score_key_delta;
  single_support = exact_support;
  for (uint32_t i = 0; i < action.repeated_count; ++i) {
    uint32_t const requirement = repeated[i];
    uint32_t const rank = packed_rank(requirement);
    worker->bag[rank] += packed_count(requirement);
    single_support &= ~(UINT64_C(1) << rank);
  }
  while (single_support != 0) {
    uint32_t const rank = uint32_t(__builtin_ctzll(single_support));
    ++worker->bag[rank];
    single_support &= single_support - 1;
  }
  worker->bag_mask = parent_bag_mask;

  if (child == -HUGE_VAL) return;
  ++worker->transitions;
  double const candidate_bound = action.partial_score + child;
  *best = std::max(*best, candidate_bound);
  double rounding_error = action.rounding_error_base;
  rounding_error += fabs(child);
  rounding_error += 1.0;
  rounding_error *= DBL_EPSILON * 4.0;
  *max_rounding_error = std::max(
      *max_rounding_error, rounding_error);
}

double DfsAnagramSearch::compute_projected_score_bound(
    BoundWorker* worker) {
  assert(bound_mode == SCORE_BOUND_PROJECTED);
  assert(worker->score_key < bound_capacity);
  unsigned int wait_spins = 0;
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

  if (worker->bag_mask == 0 && worker->wild_left == 0) {
    publish_parallel_score_bound(worker->score_key, 0.0);
    ++worker->states_computed;
    return 0.0;
  }

  double best = -HUGE_VAL;
  double max_rounding_error = 0.0;
  size_t const bucket = worker->bag_mask == 0
      ? size_t(DFS_SYMBOL_COUNT)
      : size_t(__builtin_ctzll(worker->bag_mask));
  size_t const end = projected_bucket_starts[bucket + 1];
  size_t const begin = first_projected_length_candidate(
      projected_bucket_starts[bucket], end, worker->letters_left);
  for (size_t action = begin; action < end; ++action) {
    ++worker->candidate_tests;
    if (!projected_action_fits(action, *worker)) continue;
    consider_projected_bound_candidate(
        action, worker, &best, &max_rounding_error);
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

uint64_t DfsAnagramSearch::test_projected_wild_update(
    double partial_score, double rounding_error_base,
    float const* children, double* best, double* max_rounding_error,
    size_t count, bool vector) {
#if defined(__x86_64__)
  if (vector && __builtin_cpu_supports("avx2"))
    return projected_wild_update_avx2(
        partial_score, rounding_error_base, children, best,
        max_rounding_error, count);
#else
  (void)vector;
#endif
  return projected_wild_update_scalar(
      partial_score, rounding_error_base, children, best,
      max_rounding_error, count);
}

bool DfsAnagramSearch::compute_projected_score_bound_bottom_up(
    size_t requested_threads) {
  FILE* const progress = dfs_diagnostic_stream();
  if (bound_mode != SCORE_BOUND_PROJECTED || !bound_complete ||
      bound_capacity == 0 ||
      bound_plain_float_values.get() == NULL ||
      score_wild_span == 0 ||
      bound_capacity % score_wild_span != 0)
    return false;

  size_t const exact_bag_count = bound_capacity / score_wild_span;
  if (exact_bag_count == 0 || exact_bag_count > UINT32_MAX)
    return false;

  struct VectorWorker {
    std::array<uint32_t, DFS_SYMBOL_COUNT> bag;
    std::vector<double> best;
    std::vector<double> max_rounding_error;
    // Shadow copies, used only by PROJECTED_KERNEL_VERIFY.
    std::vector<double> verify_best;
    std::vector<double> verify_error;
    uint64_t candidate_tests;
    uint64_t fitting_transitions;
    uint64_t transitions;
    uint64_t nextafter_calls;
    bool verify_mismatch;

    explicit VectorWorker(size_t wild_span):
        best(wild_span),
        max_rounding_error(wild_span),
        verify_best(wild_span),
        verify_error(wild_span),
        candidate_tests(0),
        fitting_transitions(0),
        transitions(0),
        nextafter_calls(0),
        verify_mismatch(false) {
      bag.fill(0);
    }
  };

  // Resolved once, before any worker starts, so no AVX2 instruction can be
  // reached on a machine that lacks it and no bag pays for the decision.
  ProjectedKernel const kernel = projected_kernel_choice();
  if (progress != NULL && kernel != PROJECTED_KERNEL_SCALAR) {
    dfs_diagnostic(
        "phase 2: projected wildcard update kernel %s\n",
        projected_kernel_name(kernel));
  }

  float* const values = bound_plain_float_values.get();
  size_t const wildcard_bucket = DFS_SYMBOL_COUNT;
  size_t const wildcard_begin =
      projected_bucket_starts[wildcard_bucket];
  size_t const wildcard_end =
      projected_bucket_starts[wildcard_bucket + 1];

  uint64_t candidate_tests = 0;
  uint64_t fitting_transitions = 0;
  uint64_t transitions = 0;
  uint64_t nextafter_calls = 0;

  // The exact-empty vector is its own base layer. Wildcard-only actions point
  // to smaller indexes in the same vector.
  values[0] = 0.0f;
  for (size_t wild = 1; wild < score_wild_span; ++wild) {
    double best = -HUGE_VAL;
    double max_rounding_error = 0.0;
    for (size_t action_index = wildcard_begin;
         action_index < wildcard_end; ++action_index) {
      ProjectedAction const& action =
          projected_actions[action_index];
      ++candidate_tests;
      size_t const wild_length =
          projected_wild_length(action.packed_lengths);
      assert(wild_length != 0);
      if (wild_length > wild) continue;
      ++fitting_transitions;
      double const child = double(values[wild - wild_length]);
      if (child == -HUGE_VAL) continue;
      ++transitions;
      best = std::max(best, action.partial_score + child);
      double rounding_error = action.rounding_error_base;
      rounding_error += fabs(child);
      rounding_error += 1.0;
      rounding_error *= DBL_EPSILON * 4.0;
      max_rounding_error =
          std::max(max_rounding_error, rounding_error);
    }
    double const result =
        best == -HUGE_VAL
            ? -HUGE_VAL
            : round_score_bound_up(
                  static_cast<long double>(best) +
                  static_cast<long double>(max_rounding_error),
                  &nextafter_calls);
    values[wild] = round_float_score_bound_up(result);
  }

  size_t max_exact_total = 0;
  for (size_t rank = 0; rank < DFS_SYMBOL_COUNT; ++rank) {
    if ((score_exact_mask & (UINT64_C(1) << rank)) != 0)
      max_exact_total += bag[rank];
  }
  if (score_exact_mask != 0) {
    assert(max_exact_total != 0);
    --max_exact_total;
  }

  std::vector<std::vector<uint32_t> > exact_layers;
  std::vector<VectorWorker> workers;
  size_t worker_count = 1;
  try {
    exact_layers.resize(max_exact_total + 1);
    for (size_t exact_key = 1;
         exact_key < exact_bag_count; ++exact_key) {
      size_t exact_total = 0;
      for (size_t rank = 0; rank < DFS_SYMBOL_COUNT; ++rank) {
        if ((score_exact_mask & (UINT64_C(1) << rank)) == 0)
          continue;
        uint64_t const multiplier = score_multipliers[rank];
        uint64_t const radix = uint64_t(bag[rank]) + 1;
        exact_total += size_t(
            (uint64_t(exact_key) / multiplier) % radix);
      }
      assert(exact_total < exact_layers.size());
      exact_layers[exact_total].push_back(uint32_t(exact_key));
    }

    size_t largest_layer = 0;
    for (size_t total = 1; total < exact_layers.size(); ++total)
      largest_layer =
          std::max(largest_layer, exact_layers[total].size());
    worker_count = std::min(
        std::max(size_t(1), requested_threads),
        std::max(size_t(1), largest_layer));
    workers.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i)
      workers.emplace_back(score_wild_span);
  } catch (...) {
    return false;
  }

  bool announced_threads = false;
  size_t actual_workers = 1;
  for (size_t exact_total = 1;
       exact_total < exact_layers.size(); ++exact_total) {
    std::vector<uint32_t> const& layer = exact_layers[exact_total];
    if (layer.empty()) continue;
    size_t const layer_workers =
        std::min(worker_count, layer.size());
    std::atomic<size_t> next_bag(0);

    auto work = [&](size_t worker_index) {
      VectorWorker* worker = &workers[worker_index];
      // One set of accumulators for every bag this worker drains from this
      // layer, folded into the worker fields once before returning. The worker
      // fields stay cumulative across layers because the same VectorWorker is
      // reused, so the final aggregation is unchanged.
      uint64_t local_candidate_tests = 0;
      uint64_t local_fitting_transitions = 0;
      uint64_t local_transitions = 0;
      uint64_t local_nextafter_calls = 0;

      // The kernel choice is constant for the whole run, so specialize the
      // action scan on it instead of branching inside it. With the vector
      // kernels out of line, a runtime branch here made the scalar path spill
      // its loop-invariant registers across a call it never makes, and that
      // cost 14% of setup.
      auto scan = [&](auto kernel_tag, VectorWorker* worker,
                      uint64_t exact_mask, uint64_t base_key,
                      size_t begin, size_t end) {
        for (size_t action_index = begin;
             action_index < end; ++action_index) {
          ++local_candidate_tests;
          // Support filtering rejects most scanned actions at depth, and the
          // sidecar rejection reads eight contiguous bytes without touching
          // the cold record.
          if ((projected_action_support[action_index] & ~exact_mask) != 0)
            continue;
          ProjectedAction const& action =
              projected_actions[action_index];
          uint32_t const* repeated =
              action.repeated_count == 0
                  ? NULL
                  : &projected_repeated_requirements[
                        action.repeated_offset];
          bool fits = true;
          for (uint32_t i = 0; i < action.repeated_count; ++i) {
            uint32_t const requirement = repeated[i];
            if (worker->bag[packed_rank(requirement)] <
                packed_count(requirement)) {
              fits = false;
              break;
            }
          }
          if (!fits) continue;

          size_t const wild_length =
              projected_wild_length(action.packed_lengths);
          // Every class fits the letter bag, so an action cannot want more
          // wildcard letters than the bag holds, and the span is that total
          // plus one. The update below therefore always covers
          // score_wild_span - wild_length wildcard counts and its
          // fitting-transition count is exact in closed form. A violation
          // means fit-class or exact-mask construction is broken, not that
          // this action should be skipped: skipping it would drop a
          // transition from an upper bound and silently lower it.
          DFS_CHECK(wild_length < score_wild_span);
          size_t const count = score_wild_span - wild_length;

          // The whole span shares one range check: parent keys ascend with
          // `wild`, so the smallest one bounds them all.
          uint64_t const first_parent_key = base_key + wild_length;
          DFS_CHECK(action.score_key_delta <= first_parent_key);
          size_t const children_offset =
              size_t(first_parent_key - action.score_key_delta);
          DFS_CHECK(children_offset + count <= bound_capacity);
          float const* const children = values + children_offset;

          double* const best = &worker->best[wild_length];
          double* const error =
              &worker->max_rounding_error[wild_length];
          local_fitting_transitions += count;
          if constexpr (decltype(kernel_tag)::value ==
                        PROJECTED_KERNEL_SCALAR) {
            local_transitions += projected_wild_update_scalar(
                action.partial_score, action.rounding_error_base,
                children, best, error, count);
          } else {
            local_transitions += projected_wild_update_checked(
                decltype(kernel_tag)::value, action.partial_score,
                action.rounding_error_base, children, best, error,
                count, worker->verify_best.data(),
                worker->verify_error.data(),
                &worker->verify_mismatch);
          }
        }
      };

      for (;;) {
        size_t const layer_index =
            next_bag.fetch_add(1, std::memory_order_relaxed);
        if (layer_index >= layer.size()) break;
        uint64_t const exact_key = layer[layer_index];
        uint64_t exact_mask = 0;
        worker->bag.fill(0);
        for (size_t rank = 0; rank < DFS_SYMBOL_COUNT; ++rank) {
          if ((score_exact_mask & (UINT64_C(1) << rank)) == 0)
            continue;
          uint64_t const multiplier = score_multipliers[rank];
          uint64_t const radix = uint64_t(bag[rank]) + 1;
          uint32_t const count = uint32_t(
              (exact_key / multiplier) % radix);
          worker->bag[rank] = count;
          if (count != 0)
            exact_mask |= UINT64_C(1) << rank;
        }
        assert(exact_mask != 0);

        std::fill(
            worker->best.begin(), worker->best.end(), -HUGE_VAL);
        std::fill(
            worker->max_rounding_error.begin(),
            worker->max_rounding_error.end(), 0.0);
        size_t const bucket = size_t(__builtin_ctzll(exact_mask));
        size_t const begin = projected_bucket_starts[bucket];
        size_t const end = projected_bucket_starts[bucket + 1];
        uint64_t const base_key =
            exact_key * uint64_t(score_wild_span);

        if (kernel == PROJECTED_KERNEL_SCALAR) {
          scan(std::integral_constant<
                   ProjectedKernel, PROJECTED_KERNEL_SCALAR>(),
               worker, exact_mask, base_key, begin, end);
        } else if (kernel == PROJECTED_KERNEL_AVX2) {
          scan(std::integral_constant<
                   ProjectedKernel, PROJECTED_KERNEL_AVX2>(),
               worker, exact_mask, base_key, begin, end);
        } else {
          scan(std::integral_constant<
                   ProjectedKernel, PROJECTED_KERNEL_VERIFY>(),
               worker, exact_mask, base_key, begin, end);
        }

        for (size_t wild = 0; wild < score_wild_span; ++wild) {
          double const best = worker->best[wild];
          double const result =
              best == -HUGE_VAL
                  ? -HUGE_VAL
                  : round_score_bound_up(
                        static_cast<long double>(best) +
                        static_cast<long double>(
                            worker->max_rounding_error[wild]),
                        &local_nextafter_calls);
          values[size_t(base_key + wild)] =
              round_float_score_bound_up(result);
        }
      }

      worker->candidate_tests += local_candidate_tests;
      worker->fitting_transitions += local_fitting_transitions;
      worker->transitions += local_transitions;
      worker->nextafter_calls += local_nextafter_calls;
    };

    std::vector<std::thread> background;
    try {
      background.reserve(layer_workers - 1);
      for (size_t i = 1; i < layer_workers; ++i)
        background.emplace_back(work, i);
    } catch (...) {
      // The already-created workers remain useful; the main thread shares
      // their dynamic queue and completes any unclaimed bags.
    }
    size_t const active_workers = background.size() + 1;
    actual_workers = std::max(actual_workers, active_workers);
    if (!announced_threads && progress != NULL &&
        active_workers > 1) {
      dfs_diagnostic(
          "phase 2: using up to %zu threads to calculate projected "
          "score bounds bottom-up\n",
          worker_count);
      announced_threads = true;
    }
    work(0);
    for (size_t i = 0; i < background.size(); ++i)
      background[i].join();
  }

  for (size_t i = 0; i < workers.size(); ++i) {
    if (!workers[i].verify_mismatch) continue;
    if (progress != NULL) {
      dfs_diagnostic(
          "phase 2: projected %s kernel disagreed with the scalar "
          "kernel; abandoning the score bound\n",
          projected_kernel_name(kernel));
    }
    return false;
  }

  for (size_t i = 0; i < workers.size(); ++i) {
    candidate_tests += workers[i].candidate_tests;
    fitting_transitions += workers[i].fitting_transitions;
    transitions += workers[i].transitions;
    nextafter_calls += workers[i].nextafter_calls;
  }

  BoundWorker root;
  root.bag = bag;
  root.bag_mask = bag_mask & score_exact_mask;
  root.score_key = current_score_key;
  root.letters_left = current_letters_left;
  root.wild_left = score_wild_letters;
  root.states_computed = 0;
  root.candidate_tests = 0;
  root.fitting_transitions = 0;
  root.transitions = 0;
  root.nextafter_calls = 0;
  root.best = -HUGE_VAL;
  root.max_rounding_error = 0.0;

  size_t const root_bucket = root.bag_mask == 0
      ? wildcard_bucket
      : size_t(__builtin_ctzll(root.bag_mask));
  size_t const root_end = projected_bucket_starts[root_bucket + 1];
  size_t const root_begin = first_projected_length_candidate(
      projected_bucket_starts[root_bucket],
      root_end, root.letters_left);
  candidate_tests += root_end - root_begin;
  double root_best = -HUGE_VAL;
  double root_max_rounding_error = 0.0;
  for (size_t action_index = root_begin;
       action_index < root_end; ++action_index) {
    if (!projected_action_fits(action_index, root)) continue;
    ProjectedAction const& action =
        projected_actions[action_index];
    ++fitting_transitions;
    assert(action.score_key_delta <= root.score_key);
    uint64_t const child_key =
        root.score_key - action.score_key_delta;
    assert(child_key < bound_capacity);
    double const child = double(values[size_t(child_key)]);
    if (child == -HUGE_VAL) continue;
    ++transitions;
    root_best = std::max(
        root_best, action.partial_score + child);
    double rounding_error = action.rounding_error_base;
    rounding_error += fabs(child);
    rounding_error += 1.0;
    rounding_error *= DBL_EPSILON * 4.0;
    root_max_rounding_error = std::max(
        root_max_rounding_error, rounding_error);
  }

  root_score_bound =
      root_best == -HUGE_VAL
          ? -HUGE_VAL
          : round_score_bound_up(
                static_cast<long double>(root_best) +
                static_cast<long double>(root_max_rounding_error),
                &nextafter_calls);
  root_score_bound_ready = true;
  bound_entries = bound_capacity;
  bound_states_computed = bound_capacity;
  bound_candidate_tests = candidate_tests;
  bound_fitting_transitions = fitting_transitions;
  bound_transitions = transitions;
  bound_nextafter_calls = nextafter_calls;
  actual_preprocess_threads = actual_workers;
  return true;
}

bool DfsAnagramSearch::compute_projected_score_bound_parallel(
    size_t requested_threads) {
  FILE* const progress = dfs_diagnostic_stream();
  if (bound_mode != SCORE_BOUND_PROJECTED || !bound_complete ||
      bound_capacity == 0)
    return false;

  BoundWorker root;
  root.bag = bag;
  root.bag_mask = bag_mask & score_exact_mask;
  root.score_key = current_score_key;
  root.letters_left = current_letters_left;
  root.wild_left = score_wild_letters;
  root.states_computed = 0;
  root.candidate_tests = 0;
  root.fitting_transitions = 0;
  root.transitions = 0;
  root.nextafter_calls = 0;
  root.best = -HUGE_VAL;
  root.max_rounding_error = 0.0;

  std::vector<uint32_t> root_candidates;
  size_t const root_bucket = root.bag_mask == 0
      ? size_t(DFS_SYMBOL_COUNT)
      : size_t(__builtin_ctzll(root.bag_mask));
  size_t const root_end = projected_bucket_starts[root_bucket + 1];
  size_t const root_begin = first_projected_length_candidate(
      projected_bucket_starts[root_bucket],
      root_end, root.letters_left);
  uint64_t const root_candidate_tests = root_end - root_begin;
  for (size_t action = root_begin; action < root_end; ++action) {
    if (projected_action_fits(action, root))
      root_candidates.push_back(uint32_t(action));
  }

  if (root_candidates.empty()) {
    root_score_bound = -HUGE_VAL;
    root_score_bound_ready = true;
    bound_candidate_tests = root_candidate_tests;
    bound_fitting_transitions = 0;
    actual_preprocess_threads = 1;
    return true;
  }

  size_t const worker_count = std::min(
      std::max(size_t(1), requested_threads),
      root_candidates.size());
  std::vector<BoundWorker> workers(worker_count, root);
  std::vector<std::thread> background;
  try {
    background.reserve(worker_count - 1);
  } catch (...) {
    return false;
  }
  if (progress != NULL && worker_count > 1) {
    dfs_diagnostic(
        "phase 2: using %zu threads to calculate projected score bounds\n",
        worker_count);
  }

  std::atomic<size_t> next_candidate(0);
  auto work = [&](size_t worker_index) {
    BoundWorker* worker = &workers[worker_index];
    for (;;) {
      size_t const index =
          next_candidate.fetch_add(1, std::memory_order_relaxed);
      if (index >= root_candidates.size()) break;
      consider_projected_bound_candidate(
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
  uint64_t candidate_tests = root_candidate_tests;
  uint64_t fitting_transitions = 0;
  uint64_t transitions = 0;
  uint64_t nextafter_calls = 0;
  double best = -HUGE_VAL;
  double max_rounding_error = 0.0;
  size_t const active_workers = background.size() + 1;
  for (size_t i = 0; i < active_workers; ++i) {
    states += workers[i].states_computed;
    candidate_tests += workers[i].candidate_tests;
    fitting_transitions += workers[i].fitting_transitions;
    transitions += workers[i].transitions;
    nextafter_calls += workers[i].nextafter_calls;
    best = std::max(best, workers[i].best);
    max_rounding_error = std::max(
        max_rounding_error, workers[i].max_rounding_error);
  }

  root_score_bound =
      best == -HUGE_VAL
          ? -HUGE_VAL
          : round_score_bound_up(
                static_cast<long double>(best) +
                static_cast<long double>(max_rounding_error),
                &nextafter_calls);
  root_score_bound_ready = true;
  bound_entries = states;
  bound_states_computed = states;
  bound_candidate_tests = candidate_tests;
  bound_fitting_transitions = fitting_transitions;
  bound_transitions = transitions;
  bound_nextafter_calls = nextafter_calls;
  actual_preprocess_threads = active_workers;
  return true;
}

bool DfsAnagramSearch::compute_score_bound_parallel(
    size_t requested_threads) {
  FILE* const progress = dfs_diagnostic_stream();
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
  root.wild_left = 0;
  root.states_computed = 0;
  root.candidate_tests = 0;
  root.fitting_transitions = 0;
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
  if (progress != NULL && worker_count > 1) {
    dfs_diagnostic(
        "phase 2: using %zu threads to calculate dense score bounds\n",
        worker_count);
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
  if (!load_score_bound(query_key, &remaining_bound)) {
    if (bound_mode != SCORE_BOUND_PREFIX) return false;
    // A rarest-most-significant prefix is dependency-closed: every fitting
    // subtraction decreases the key. Build its complete bound only after
    // phase 2 both enters the prefix and has a useful score floor. This
    // legacy builder borrows the shared scratch state; prefix mode is serial.
    bag = worker->bag;
    bag_mask = worker->bag_mask;
    current_score_key = query_key;
    current_letters_left = letters_left;
    compute_score_bound();
    if (!load_score_bound(query_key, &remaining_bound))
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

void DfsAnagramSearch::visit_fitting_class(
    SearchWorker* worker, uint32_t class_index, size_t letters_left,
    double representative_log_score, DfsSolutionSink* sink) {
  FitClass const& fit = fit_classes.get()[class_index];
  double const class_score = best_member_log_scores[class_index];
  size_t const candidate_length =
      hot_letter_length(fit.packed_length_and_count);
  assert(candidate_length <= letters_left);
  size_t const next_letters_left = letters_left - candidate_length;
  bool const first_class = worker->path.empty();

  worker->path.push_back(class_index);
  if (DFS_UNLIKELY(next_letters_left == 0)) {
    double const next_log_score =
        first_class
            ? class_score
            : score_model.append_log_score(
                  representative_log_score, class_score);
    ++worker->solutions;
    if (sink != NULL) sink->emit(worker->path, next_log_score);
    worker->path.pop_back();
    return;
  }
  if (DFS_UNLIKELY(worker->path.size() >= max_depth)) {
    worker->path.pop_back();
    return;
  }

  double const next_log_score =
      first_class
          ? class_score
          : score_model.append_log_score(
                representative_log_score, class_score);
  uint32_t const* requirements =
      packed_letters.get() + fit.letters_offset;
  uint32_t const requirement_count =
      hot_requirement_count(fit.packed_length_and_count);
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
  if (DFS_UNLIKELY(worker->split_depth != 0) &&
      worker->path.size() <= worker->split_depth) {
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
  for (size_t class_index = start; class_index < end; ++class_index) {
    uint32_t const id = uint32_t(class_index);
    if (!hot_class_fits(id, *worker)) continue;
    visit_fitting_class(
        worker, id, letters_left, representative_log_score, sink);
    if (DFS_UNLIKELY(sink != NULL && sink->should_stop())) return;
  }
}

void DfsAnagramSearch::walk_certified(
    SearchWorker* worker, int rank, size_t start, size_t end,
    size_t letters_left, double representative_log_score,
    double floor, DfsSolutionSink* sink) {
  size_t const base = size_t(rank) * certificate_stride;
  size_t class_index = start;
  while (class_index < end) {
    size_t const length = hot_letter_length(
        fit_classes.get()[class_index].packed_length_and_count);
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
    for (; class_index < group_end; ++class_index) {
      uint32_t const id = uint32_t(class_index);
      if (!hot_class_fits(id, *worker)) continue;
      visit_fitting_class(
          worker, id, letters_left, representative_log_score, sink);
      if (DFS_UNLIKELY(sink != NULL && sink->should_stop())) return;
    }
  }
}

void DfsAnagramSearch::start_search_worker(SearchWorker* worker) {
  worker->bag = bag;
  worker->bag_mask = bag_mask;
  worker->score_key = current_score_key;
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
    size_t task_progress_factor, bool verbose) {
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
      if (verbose && tasks_left % task_progress_factor == 0)
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
  DfsAnagramClass const& candidate = class_list->classes()[class_index];
  double const candidate_log_score = best_member_log_scores[class_index];
  double const next_log_score =
      path.empty()
          ? candidate_log_score
          : score_model.append_log_score(
                representative_log_score, candidate_log_score);
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
