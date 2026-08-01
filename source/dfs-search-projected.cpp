#include "dfs-search.h"

#include "dfs-diagnostic.h"
#include "dfs-search-internal.h"

#include <assert.h>
#include <fenv.h>
#include <float.h>
#include <math.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <new>
#include <thread>
#include <type_traits>
#include <vector>

#include <immintrin.h>

namespace {
// One projected action's contribution to a contiguous run of wildcard counts.
//
// The child key is base_key + wild - score_key_delta with `wild` running
// contiguously, so `children` is an ascending contiguous float range; that is
// what lets a vector kernel load whole lane groups.
//
// This scalar helper handles the AVX2 kernel's short tail. The expressions
// and their association order are fixed: partial_score + child for the score,
// and
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

}  // namespace

bool projected_bound_requirements(
    uint64_t state_count, size_t value_bytes, size_t* bytes) {
  return state_count <= SIZE_MAX / value_bytes &&
      dfs_round_up_alignment(size_t(state_count) * value_bytes, bytes);
}

bool projected_score_bound_arithmetic_supported() {
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

namespace {

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

double get_score_bound(
    double best, double max_rounding_error, uint64_t* nextafter_calls) {
  if (best == -HUGE_VAL) return -HUGE_VAL;
  return round_score_bound_up(
      static_cast<long double>(best) +
          static_cast<long double>(max_rounding_error),
      nextafter_calls);
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

}  // namespace

void ScoreBounds::clear() {
  stats_ = {};
  float_values_.reset();
  plain_float_values_.reset();
  root_score_bound_ = HUGE_VAL;
  root_score_bound_ready_ = false;
}

bool ScoreBounds::prepare(
    size_t state_count, size_t cache_budget, bool bottom_up_eligible) {
  static_assert(sizeof(AtomicFloatWord) == sizeof(uint32_t),
                "atomic float bound words must remain four bytes");
  static_assert(std::is_trivially_destructible<AtomicFloatWord>::value,
                "atomic float bound words must be trivially destructible");
  clear();
  size_t float_bytes = 0;
  if (state_count == 0 ||
      !projected_bound_requirements(
          state_count, sizeof(float), &float_bytes) ||
      float_bytes > cache_budget)
    return false;
  if (bottom_up_eligible) {
    float* values = static_cast<float*>(dfs_allocate_aligned(float_bytes));
    if (values == NULL) return false;
    plain_float_values_.reset(values);
  } else {
    AtomicFloatWord* values = static_cast<AtomicFloatWord*>(
        dfs_allocate_aligned(float_bytes));
    if (values == NULL) return false;
    for (size_t i = 0; i < state_count; ++i) {
      new (&values[i]) AtomicFloatWord;
      values[i].value.store(FLOAT_BOUND_UNSEEN, std::memory_order_relaxed);
    }
    float_values_.reset(values);
  }
  stats_.capacity = state_count;
  stats_.value_bytes = sizeof(float);
  stats_.complete = true;
  stats_.mode = DFS_SCORE_BOUND_PROJECTED;
  stats_.bytes_charged = float_bytes;
  return true;
}

bool ScoreBounds::build(
    BoundStateView root, ScoreKeyLayout const& layout,
    ProjectedActions const& actions, size_t budget, size_t threads,
    DfsSearchStats* stats) {
  assert(stats != NULL);
  clear();
  bool const bottom_up_eligible =
      layout.wild_span != 0 &&
      layout.effective_state_count / layout.wild_span <= UINT32_MAX;
  if (!prepare(
          size_t(layout.effective_state_count), budget,
          bottom_up_eligible))
    return false;

  dfs_diagnostic(
      "phase 2 preflight: score-bound mode projected dense "
      "(%zu-byte values, capacity %zu, %s coverage)\n",
      stats_.value_bytes, stats_.capacity,
      stats_.complete ? "complete effective" : "partial");
  dfs_diagnostic(
      "phase 2 preflight: projected evaluator %s\n",
      plain_float_values_.get() != NULL
          ? "bottom-up plain"
          : "top-down atomic");

  bool computed = false;
  try {
    computed = plain_float_values_.get() != NULL
        ? compute_projected_score_bounds_bottom_up(
              root, layout, actions, stats, threads)
        : compute_projected_score_bounds_top_down(
              root, layout, actions, stats, threads);
  } catch (...) {
    clear();
    return false;
  }
  if (!computed) {
    clear();
    return false;
  }
  return true;
}

bool ScoreBounds::lookup(uint64_t key, double* value) const {
  if (stats_.mode == DFS_SCORE_BOUND_OFF || key >= stats_.capacity)
    return false;
  assert(stats_.value_bytes == sizeof(float));
  if (plain_float_values_.get() != NULL) {
    *value = double(plain_float_values_.get()[size_t(key)]);
    return true;
  }
  uint32_t const stored =
      float_values_.get()[size_t(key)].value.load(
          std::memory_order_relaxed);
  if (stored == FLOAT_BOUND_UNSEEN || stored == FLOAT_BOUND_COMPUTING)
    return false;
  *value = double(bits_to_float(stored));
  return true;
}

bool ScoreBounds::root_lookup(double* value) const {
  if (!root_score_bound_ready_) return false;
  *value = root_score_bound_;
  return true;
}

void ScoreBounds::set_root(double value) {
  root_score_bound_ = value;
  root_score_bound_ready_ = true;
}

void ScoreBounds::publish_top_down(uint64_t key, double value) {
  assert(key < stats_.capacity);
  assert(stats_.value_bytes == sizeof(float));
  assert(plain_float_values_.get() == NULL);
  float_values_.get()[size_t(key)].value.store(
      float_to_bits(round_float_score_bound_up(value)),
      std::memory_order_release);
}

// A finite projected bound proves a completion exists only when no wildcard
// letters were merged; otherwise it bounds a relaxation of the real problem.
Reachability
DfsSearchData::cached_reachability(
    uint64_t key, bool original_root) const {
  double value;
  if (original_root) {
    if (!score_bounds.root_lookup(&value)) return REACHABILITY_UNKNOWN;
  } else if (!score_bounds.lookup(key, &value)) {
    return REACHABILITY_UNKNOWN;
  }
  if (value == -HUGE_VAL) return REACHABILITY_NO;
  if (score_wild_letters == 0) return REACHABILITY_YES;
  return REACHABILITY_UNKNOWN;
}

bool ProjectedActions::build(
    DfsSearchData const& data, ScoreKeyLayout const& layout,
    uint16_t const* wild_lengths, ProjectedActions* result) {
  static_assert(sizeof(ProjectedAction) == 48,
                "ProjectedAction must remain three 16-byte blocks, the last "
                "one partly free");
  *result = ProjectedActions();
  ProjectedActions built;

  DfsClassSpan const classes = data.class_list->classes();
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
      entry.delta = data.score_key_deltas.get()[i];
      entry.class_id = uint32_t(i);
      by_delta.push_back(entry);
    }
    std::sort(
        by_delta.begin(), by_delta.end(),
        [&](DeltaClass const& a, DeltaClass const& b) {
          if (a.delta != b.delta) return a.delta < b.delta;
          double const a_score = data.best_member_log_scores[a.class_id];
          double const b_score = data.best_member_log_scores[b.class_id];
          if (a_score != b_score) return a_score > b_score;
          return a.class_id < b.class_id;
        });

    std::vector<uint32_t> representatives;
    representatives.reserve(by_delta.size());
    for (size_t i = 0; i < by_delta.size(); ++i) {
      if (i == 0 || by_delta[i].delta != by_delta[i - 1].delta)
        representatives.push_back(by_delta[i].class_id);
    }

    static size_t const WILDCARD_BUCKET = DFS_SYMBOL_COUNT;
    std::array<size_t, DFS_SYMBOL_COUNT + 1> bucket_counts;
    bucket_counts.fill(0);
    for (size_t i = 0; i < representatives.size(); ++i) {
      uint32_t const id = representatives[i];
      uint64_t const exact_support =
          data.fit_classes.get()[id].support_mask & layout.exact_mask;
      size_t const bucket = exact_support == 0
          ? WILDCARD_BUCKET
          : size_t(__builtin_ctzll(exact_support));
      ++bucket_counts[bucket];
    }

    size_t offset = 0;
    for (size_t bucket = 0; bucket <= WILDCARD_BUCKET; ++bucket) {
      built.bucket_starts_[bucket] = offset;
      offset += bucket_counts[bucket];
    }
    built.bucket_starts_[WILDCARD_BUCKET + 1] = offset;
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
      write[bucket] = built.bucket_starts_[bucket];
    for (size_t i = 0; i < representatives.size(); ++i) {
      uint32_t const id = representatives[i];
      uint64_t const exact_support =
          data.fit_classes.get()[id].support_mask & layout.exact_mask;
      size_t const bucket = exact_support == 0
          ? WILDCARD_BUCKET
          : size_t(__builtin_ctzll(exact_support));
      FitClass const& fit = data.fit_classes.get()[id];
      ProjectedAction action;
      action.score_key_delta = data.score_key_deltas.get()[id];
      double const class_score = data.best_member_log_scores[id];
      action.partial_score =
          class_score + data.segment_boundary_log_score;
      action.rounding_error_base =
          fabs(class_score) + fabs(data.segment_boundary_log_score);
      action.repeated_offset =
          uint32_t(built.repeated_requirements_.size());
      action.packed_lengths =
          hot_letter_length(fit.metadata.packed_length_and_count) |
          (uint32_t(wild_lengths[id]) << 16);
      action.repeated_count = 0;
      uint32_t const* requirements =
          data.packed_letters.get() + fit.metadata.letters_offset;
      uint32_t const repeated =
          hot_repeated_count(fit.metadata.packed_length_and_count);
      for (uint32_t repeated_index = 0;
           repeated_index < repeated; ++repeated_index) {
        uint32_t const requirement = requirements[repeated_index];
        uint32_t const rank = packed_rank(requirement);
        if ((layout.exact_mask & (UINT64_C(1) << rank)) == 0)
          continue;
        built.repeated_requirements_.push_back(requirement);
        ++action.repeated_count;
      }
      ordered[write[bucket]].action = action;
      ordered[write[bucket]].exact_support = exact_support;
      ++write[bucket];
    }

    for (size_t bucket = 0; bucket <= WILDCARD_BUCKET; ++bucket) {
      std::sort(
          ordered.begin() + built.bucket_starts_[bucket],
          ordered.begin() + built.bucket_starts_[bucket + 1],
          [](SortableAction const& a, SortableAction const& b) {
            uint32_t const a_length =
                projected_total_length(a.action.packed_lengths);
            uint32_t const b_length =
                projected_total_length(b.action.packed_lengths);
            if (a_length != b_length) return a_length > b_length;
            return a.action.score_key_delta < b.action.score_key_delta;
          });
    }

    built.actions_.resize(offset);
    built.exact_supports_.resize(offset);
    for (size_t i = 0; i < offset; ++i) {
      built.actions_[i] = ordered[i].action;
      built.exact_supports_[i] = ordered[i].exact_support;
    }
  } catch (...) {
    return false;
  }
  assert(built.exact_supports_.size() == built.actions_.size());

  *result = std::move(built);
  return true;
}

size_t ProjectedActions::bucket_begin(size_t bucket) const {
  return bucket_starts_[bucket];
}

size_t ProjectedActions::bucket_end(size_t bucket) const {
  return bucket_starts_[bucket + 1];
}

ProjectedAction const& ProjectedActions::action(size_t index) const {
  return actions_[index];
}

uint64_t ProjectedActions::exact_support(size_t index) const {
  return exact_supports_[index];
}

uint32_t const* ProjectedActions::repeated_begin(
    ProjectedAction const& action) const {
  return action.repeated_count == 0
      ? NULL
      : &repeated_requirements_[action.repeated_offset];
}

bool ProjectedActions::fits(
    size_t action_index, BoundStateView state) const {
  ProjectedAction const& action = actions_[action_index];
  if (projected_wild_length(action.packed_lengths) >
      state.wild_left)
    return false;
  if ((exact_supports_[action_index] &
       ~state.letter_bag.support_mask) != 0)
    return false;
  uint32_t const* repeated = repeated_begin(action);
  for (uint32_t i = 0; i < action.repeated_count; ++i) {
    uint32_t const requirement = repeated[i];
    if (state.letter_bag.counts[packed_rank(requirement)] <
        packed_count(requirement))
      return false;
  }
  return true;
}

size_t ProjectedActions::first_length_candidate(
    size_t begin, size_t end, size_t letters_left) const {
  while (begin < end) {
    size_t const middle = begin + (end - begin) / 2;
    size_t const candidate_length = projected_total_length(
        actions_[middle].packed_lengths);
    if (candidate_length > letters_left)
      begin = middle + 1;
    else
      end = middle;
  }
  return begin;
}

size_t ProjectedActions::size() const {
  return actions_.size();
}

void ScoreBounds::consider_projected_top_down_candidate(
    ProjectedActions const& actions, size_t action_index,
    TopDownWorker* worker, double* best,
    double* max_rounding_error) {
  ProjectedAction const& action = actions.action(action_index);
  uint64_t const exact_support = actions.exact_support(action_index);
  ++worker->stats.fitting_transitions;
  size_t const candidate_length =
      projected_total_length(action.packed_lengths);
  size_t const wild_length =
      projected_wild_length(action.packed_lengths);
  uint64_t const parent_bag_mask = worker->bag_mask;
  uint64_t single_support = exact_support;
  uint32_t const* repeated = actions.repeated_begin(action);
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
  double const child = compute_projected_score_bound_top_down(
      actions, worker);
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
  ++worker->stats.transitions;
  double const candidate_bound = action.partial_score + child;
  *best = std::max(*best, candidate_bound);
  double rounding_error = action.rounding_error_base;
  rounding_error += fabs(child);
  rounding_error += 1.0;
  rounding_error *= DBL_EPSILON * 4.0;
  *max_rounding_error = std::max(
      *max_rounding_error, rounding_error);
}

double ScoreBounds::compute_projected_score_bound_top_down(
    ProjectedActions const& actions, TopDownWorker* worker) {
  assert(stats_.mode == DFS_SCORE_BOUND_PROJECTED);
  assert(worker->score_key < stats_.capacity);
  unsigned int wait_spins = 0;
  AtomicFloatWord& slot =
      float_values_.get()[size_t(worker->score_key)];
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
    publish_top_down(worker->score_key, 0.0);
    ++worker->stats.states_computed;
    return 0.0;
  }

  double best = -HUGE_VAL;
  double max_rounding_error = 0.0;
  size_t const bucket = worker->bag_mask == 0
      ? size_t(DFS_SYMBOL_COUNT)
      : size_t(__builtin_ctzll(worker->bag_mask));
  size_t const end = actions.bucket_end(bucket);
  size_t const begin = actions.first_length_candidate(
      actions.bucket_begin(bucket), end, worker->letters_left);
  for (size_t action = begin; action < end; ++action) {
    ++worker->stats.candidate_tests;
    if (!actions.fits(action, bound_state_view(*worker))) continue;
    consider_projected_top_down_candidate(
        actions,
        action, worker, &best, &max_rounding_error);
  }

  double const result = get_score_bound(
      best, max_rounding_error, &worker->stats.nextafter_calls);
  publish_top_down(worker->score_key, result);
  ++worker->stats.states_computed;
  return result;
}

bool ScoreBounds::compute_projected_score_bounds_bottom_up(
    BoundStateView root_state, ScoreKeyLayout const& layout,
    ProjectedActions const& actions,
    DfsSearchStats* stats, size_t requested_threads) {
  FILE* const progress = dfs_diagnostic_stream();
  if (stats_.mode != DFS_SCORE_BOUND_PROJECTED ||
      !stats_.complete ||
      stats_.capacity == 0 ||
      plain_float_values_.get() == NULL ||
      layout.wild_span == 0 ||
      stats_.capacity % layout.wild_span != 0)
    return false;

  size_t const exact_bag_count =
      stats_.capacity / layout.wild_span;
  if (exact_bag_count == 0 || exact_bag_count > UINT32_MAX)
    return false;

  if (progress != NULL) {
    dfs_diagnostic(
        "phase 2: projected wildcard update kernel avx2\n");
  }

  float* const values = plain_float_values_.get();
  size_t const wildcard_bucket = DFS_SYMBOL_COUNT;
  size_t const wildcard_begin = actions.bucket_begin(wildcard_bucket);
  size_t const wildcard_end = actions.bucket_end(wildcard_bucket);

  DfsSearchStats::Bounds::Projected total_stats;

  // The exact-empty vector is its own base layer. Wildcard-only actions point
  // to smaller indexes in the same vector.
  values[0] = 0.0f;
  for (size_t wild = 1; wild < layout.wild_span; ++wild) {
    double best = -HUGE_VAL;
    double max_rounding_error = 0.0;
    for (size_t action_index = wildcard_begin;
         action_index < wildcard_end; ++action_index) {
      ProjectedAction const& action =
          actions.action(action_index);
      ++total_stats.candidate_tests;
      size_t const wild_length =
          projected_wild_length(action.packed_lengths);
      assert(wild_length != 0);
      if (wild_length > wild) continue;
      ++total_stats.fitting_transitions;
      double const child = double(values[wild - wild_length]);
      if (child == -HUGE_VAL) continue;
      ++total_stats.transitions;
      best = std::max(best, action.partial_score + child);
      double rounding_error = action.rounding_error_base;
      rounding_error += fabs(child);
      rounding_error += 1.0;
      rounding_error *= DBL_EPSILON * 4.0;
      max_rounding_error =
          std::max(max_rounding_error, rounding_error);
    }
    double const result = get_score_bound(
        best, max_rounding_error, &total_stats.nextafter_calls);
    values[wild] = round_float_score_bound_up(result);
  }

  size_t max_exact_total = 0;
  for (size_t rank = 0; rank < DFS_SYMBOL_COUNT; ++rank) {
    if ((layout.exact_mask & (UINT64_C(1) << rank)) != 0)
      max_exact_total += root_state.letter_bag.counts[rank];
  }
  if (layout.exact_mask != 0) {
    assert(max_exact_total != 0);
    --max_exact_total;
  }

  std::vector<std::vector<uint32_t> > exact_layers;
  std::vector<ScoreBounds::BottomUpWorker> workers;
  size_t worker_count = 1;
  try {
    exact_layers.resize(max_exact_total + 1);
    for (size_t exact_key = 1;
         exact_key < exact_bag_count; ++exact_key) {
      size_t exact_total = 0;
      for (size_t rank = 0; rank < DFS_SYMBOL_COUNT; ++rank) {
        if ((layout.exact_mask & (UINT64_C(1) << rank)) == 0)
          continue;
        uint64_t const multiplier = layout.multipliers[rank];
        uint64_t const radix =
            uint64_t(root_state.letter_bag.counts[rank]) + 1;
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
      workers.emplace_back(layout.wild_span);
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
      ScoreBounds::BottomUpWorker* worker = &workers[worker_index];
      // One set of accumulators for every data->bag this worker drains from this
      // layer, folded into the worker fields once before returning. The worker
      // stats stay cumulative across layers because the same BottomUpWorker
      // reused, so the final aggregation is unchanged.
      DfsSearchStats::Bounds::Projected local_stats;

      auto scan = [&](ScoreBounds::BottomUpWorker* worker,
                      uint64_t exact_mask,
                      uint64_t base_key,
                      size_t begin, size_t end) {
        for (size_t action_index = begin;
             action_index < end; ++action_index) {
          ++local_stats.candidate_tests;
          // Support filtering rejects most scanned actions at depth, and the
          // sidecar rejection reads eight contiguous bytes without touching
          // the cold record.
          if ((actions.exact_support(action_index) & ~exact_mask) != 0)
            continue;
          ProjectedAction const& action =
              actions.action(action_index);
          uint32_t const* repeated = actions.repeated_begin(action);
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
          // Every class fits the letter data->bag, so an action cannot want more
          // wildcard letters than the data->bag holds, and the span is that total
          // plus one. The update below therefore always covers
          // layout.wild_span - wild_length wildcard counts and its
          // fitting-transition count is exact in closed form. A violation
          // means fit-class or exact-mask construction is broken, not that
          // this action should be skipped: skipping it would drop a
          // transition from an upper bound and silently lower it.
          DFS_CHECK(wild_length < layout.wild_span);
          size_t const count = layout.wild_span - wild_length;

          // The whole span shares one range check: parent keys ascend with
          // `wild`, so the smallest one bounds them all.
          uint64_t const first_parent_key = base_key + wild_length;
          DFS_CHECK(action.score_key_delta <= first_parent_key);
          size_t const children_offset =
              size_t(first_parent_key - action.score_key_delta);
          DFS_CHECK(children_offset + count <=
                    stats_.capacity);
          float const* const children = values + children_offset;

          double* const best = &worker->best[wild_length];
          double* const error =
              &worker->max_rounding_error[wild_length];
          local_stats.fitting_transitions += count;
          local_stats.transitions += projected_wild_update_avx2(
              action.partial_score, action.rounding_error_base,
              children, best, error, count);
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
          if ((layout.exact_mask & (UINT64_C(1) << rank)) == 0)
            continue;
          uint64_t const multiplier = layout.multipliers[rank];
          uint64_t const radix =
              uint64_t(root_state.letter_bag.counts[rank]) + 1;
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
        size_t const begin = actions.bucket_begin(bucket);
        size_t const end = actions.bucket_end(bucket);
        uint64_t const base_key =
            exact_key * uint64_t(layout.wild_span);

        scan(worker, exact_mask, base_key, begin, end);

        for (size_t wild = 0; wild < layout.wild_span; ++wild) {
          double const best = worker->best[wild];
          double const result = get_score_bound(
              best, worker->max_rounding_error[wild],
              &local_stats.nextafter_calls);
          values[size_t(base_key + wild)] =
              round_float_score_bound_up(result);
        }
      }

      worker->stats.add(local_stats);
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
    total_stats.add(workers[i].stats);
  }

  TopDownWorker root;
  std::copy(
      root_state.letter_bag.counts,
      root_state.letter_bag.counts + DFS_SYMBOL_COUNT,
      root.bag.begin());
  root.bag_mask = root_state.letter_bag.support_mask & layout.exact_mask;
  root.score_key = root_state.score_key;
  root.letters_left = root_state.letters_left;
  root.wild_left = root_state.wild_left;
  root.stats.clear();
  root.best = -HUGE_VAL;
  root.max_rounding_error = 0.0;

  size_t const root_bucket = root.bag_mask == 0
      ? wildcard_bucket
      : size_t(__builtin_ctzll(root.bag_mask));
  size_t const root_end = actions.bucket_end(root_bucket);
  size_t const root_begin = actions.first_length_candidate(
      actions.bucket_begin(root_bucket),
      root_end, root.letters_left);
  total_stats.candidate_tests += root_end - root_begin;
  double root_best = -HUGE_VAL;
  double root_max_rounding_error = 0.0;
  for (size_t action_index = root_begin;
       action_index < root_end; ++action_index) {
    if (!actions.fits(action_index, bound_state_view(root))) continue;
    ProjectedAction const& action =
        actions.action(action_index);
    ++total_stats.fitting_transitions;
    assert(action.score_key_delta <= root.score_key);
    uint64_t const child_key =
        root.score_key - action.score_key_delta;
    assert(child_key < stats_.capacity);
    double const child = double(values[size_t(child_key)]);
    if (child == -HUGE_VAL) continue;
    ++total_stats.transitions;
    root_best = std::max(
        root_best, action.partial_score + child);
    double rounding_error = action.rounding_error_base;
    rounding_error += fabs(child);
    rounding_error += 1.0;
    rounding_error *= DBL_EPSILON * 4.0;
    root_max_rounding_error = std::max(
        root_max_rounding_error, rounding_error);
  }

  set_root(get_score_bound(
      root_best, root_max_rounding_error, &total_stats.nextafter_calls));
  stats_.entries = stats_.capacity;
  total_stats.states_computed = stats_.capacity;
  stats_.projected = total_stats;
  stats->execution.preprocess_threads = actual_workers;
  return true;
}

bool ScoreBounds::compute_projected_score_bounds_top_down(
    BoundStateView root_state, ScoreKeyLayout const& layout,
    ProjectedActions const& actions,
    DfsSearchStats* stats, size_t requested_threads) {
  FILE* const progress = dfs_diagnostic_stream();
  if (stats_.mode != DFS_SCORE_BOUND_PROJECTED ||
      !stats_.complete || stats_.capacity == 0)
    return false;

  TopDownWorker root;
  std::copy(
      root_state.letter_bag.counts,
      root_state.letter_bag.counts + DFS_SYMBOL_COUNT,
      root.bag.begin());
  root.bag_mask = root_state.letter_bag.support_mask & layout.exact_mask;
  root.score_key = root_state.score_key;
  root.letters_left = root_state.letters_left;
  root.wild_left = root_state.wild_left;
  root.stats.clear();
  root.best = -HUGE_VAL;
  root.max_rounding_error = 0.0;

  std::vector<uint32_t> root_candidates;
  size_t const root_bucket = root.bag_mask == 0
      ? size_t(DFS_SYMBOL_COUNT)
      : size_t(__builtin_ctzll(root.bag_mask));
  size_t const root_end = actions.bucket_end(root_bucket);
  size_t const root_begin = actions.first_length_candidate(
      actions.bucket_begin(root_bucket),
      root_end, root.letters_left);
  uint64_t const root_candidate_tests = root_end - root_begin;
  for (size_t action = root_begin; action < root_end; ++action) {
    if (actions.fits(action, bound_state_view(root)))
      root_candidates.push_back(uint32_t(action));
  }

  if (root_candidates.empty()) {
    set_root(-HUGE_VAL);
    root.stats.candidate_tests = root_candidate_tests;
    stats_.projected = root.stats;
    stats->execution.preprocess_threads = 1;
    return true;
  }

  size_t const worker_count = std::min(
      std::max(size_t(1), requested_threads),
      root_candidates.size());
  std::vector<TopDownWorker> workers(worker_count, root);
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
    TopDownWorker* worker = &workers[worker_index];
    for (;;) {
      size_t const index =
          next_candidate.fetch_add(1, std::memory_order_relaxed);
      if (index >= root_candidates.size()) break;
      consider_projected_top_down_candidate(
          actions,
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

  root.stats.candidate_tests = root_candidate_tests;
  double best = -HUGE_VAL;
  double max_rounding_error = 0.0;
  size_t const active_workers = background.size() + 1;
  for (size_t i = 0; i < active_workers; ++i) {
    root.stats.add(workers[i].stats);
    best = std::max(best, workers[i].best);
    max_rounding_error = std::max(
        max_rounding_error, workers[i].max_rounding_error);
  }

  set_root(get_score_bound(
      best, max_rounding_error, &root.stats.nextafter_calls));
  stats_.entries = root.stats.states_computed;
  stats_.projected = root.stats;
  stats->execution.preprocess_threads = active_workers;
  return true;
}
