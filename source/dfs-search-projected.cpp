#include "dfs-search.h"

#include "dfs-diagnostic.h"

#include <assert.h>
#include <float.h>
#include <math.h>

#include <algorithm>
#include <atomic>
#include <thread>
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

bool DfsAnagramSearch::prepare_projected_actions() {
  static_assert(sizeof(ProjectedAction) == 48,
                "ProjectedAction must remain three 16-byte blocks, the last "
                "one partly free");
  projected_actions.clear();
  projected_action_support.clear();
  projected_repeated_requirements.clear();
  projected_bucket_starts.fill(0);
  projected_actions_ready = false;

  DfsClassSpan const classes = class_list->classes();
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
    std::sort(
        by_delta.begin(), by_delta.end(),
        [&](DeltaClass const& a, DeltaClass const& b) {
          if (a.delta != b.delta) return a.delta < b.delta;
          double const a_score = best_member_log_scores[a.class_id];
          double const b_score = best_member_log_scores[b.class_id];
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
          hot_letter_length(fit.metadata.packed_length_and_count) |
          (uint32_t(score_wild_lengths.get()[id]) << 16);
      action.repeated_count = 0;
      uint32_t const* requirements =
          packed_letters.get() + fit.metadata.letters_offset;
      uint32_t const repeated =
          hot_repeated_count(fit.metadata.packed_length_and_count);
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
    size_t count) {
  return projected_wild_update_avx2(
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
    uint64_t candidate_tests;
    uint64_t fitting_transitions;
    uint64_t transitions;
    uint64_t nextafter_calls;

    explicit VectorWorker(size_t wild_span):
        best(wild_span),
        max_rounding_error(wild_span),
        candidate_tests(0),
        fitting_transitions(0),
        transitions(0),
        nextafter_calls(0) {
      bag.fill(0);
    }
  };

  if (progress != NULL) {
    dfs_diagnostic(
        "phase 2: projected wildcard update kernel avx2\n");
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

      auto scan = [&](VectorWorker* worker, uint64_t exact_mask,
                      uint64_t base_key,
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
          local_transitions += projected_wild_update_avx2(
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

        scan(worker, exact_mask, base_key, begin, end);

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

