#include "dfs-search.h"

#include "dfs-all-runner.h"
#include "dfs-any-runner.h"
#include "dfs-diagnostic.h"
#include "dfs-search-internal.h"

#include <assert.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <chrono>

namespace {

size_t const MIB = size_t(1024) * size_t(1024);

static size_t derived_max_depth(DfsClassList const* classes,
                                size_t letter_count) {
  assert(classes != NULL);
  return letter_count / size_t(classes->min_word_length());
}

}  // namespace

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

static int length_certificate_mode() {
  char const* mode = getenv("NUTRIMATIC_LENGTH_CERTIFICATE");
  if (mode == NULL || mode[0] == '\0' || strcmp(mode, "1") == 0)
    return 2;
  if (strcmp(mode, "shadow") == 0) return 1;
  if (strcmp(mode, "0") == 0) return 0;
  return 0;
}

static char const* score_bound_mode_name(
    DfsScoreBoundMode mode) {
  switch (mode) {
    case DFS_SCORE_BOUND_PROJECTED:
      return "projected dense";
    case DFS_SCORE_BOUND_OFF:
      return "off";
  }
  return "unknown";
}

[[noreturn]] static void abort_phase_two(char const* reason) {
  dfs_diagnostic_to_stream(
      stderr, "error: phase 2 cannot search this bag (%s)\n",
      reason != NULL ? reason : "reason not recorded");
  abort();
}

}  // namespace

bool ScoreKeyLayout::choose(
    std::array<uint32_t, DFS_SYMBOL_COUNT> const& bag,
    size_t letter_count, size_t cache_budget, int exact_letters,
    ScoreKeyLayout* result) {
  ScoreKeyLayout selected;
  std::vector<int> present_ranks;
  present_ranks.reserve(DFS_SYMBOL_COUNT);
  for (int rank = 0; rank < DFS_SYMBOL_COUNT; ++rank)
    if (bag[size_t(rank)] != 0) present_ranks.push_back(rank);

  size_t selected_exact_letters = 0;
  if (exact_letters >= 0) {
    selected_exact_letters = std::min(
        size_t(exact_letters), present_ranks.size());
  } else {
    uint64_t exact_product = 1;
    size_t exact_total = 0;
    for (size_t d = 0; d <= present_ranks.size(); ++d) {
      size_t const wild_total = letter_count - exact_total;
      uint64_t projected_states;
      bool const states_ok = dfs_checked_multiply_u64(
          exact_product, uint64_t(wild_total + 1), &projected_states);
      uint64_t effective = projected_states;
      if (states_ok && d != 0) {
        uint64_t const root_radix =
            uint64_t(bag[size_t(present_ranks[0])]) + 1;
        effective =
            (projected_states / root_radix) * (root_radix - 1);
      }
      size_t bytes = 0;
      if (states_ok &&
          projected_bound_requirements(
              effective, sizeof(float), &bytes) &&
          bytes <= cache_budget)
        selected_exact_letters = d;
      if (d != present_ranks.size()) {
        int const rank = present_ranks[d];
        uint64_t next_product;
        if (!dfs_checked_multiply_u64(
                exact_product, uint64_t(bag[size_t(rank)]) + 1,
                &next_product))
          break;
        exact_product = next_product;
        exact_total += bag[size_t(rank)];
      }
    }
  }

  size_t exact_total = 0;
  for (size_t i = 0; i < selected_exact_letters; ++i) {
    int const rank = present_ranks[i];
    selected.exact_mask |= UINT64_C(1) << rank;
    exact_total += bag[size_t(rank)];
  }
  selected.exact_letters = selected_exact_letters;
  selected.wild_letters = letter_count - exact_total;
  selected.wild_span = selected.wild_letters + 1;

  uint64_t exact_product = 1;
  for (int rank = DFS_SYMBOL_COUNT - 1; rank >= 0; --rank) {
    if ((selected.exact_mask & (UINT64_C(1) << rank)) == 0) continue;
    selected.multipliers[size_t(rank)] = exact_product;
    if (!dfs_checked_multiply_u64(
            exact_product, uint64_t(bag[size_t(rank)]) + 1,
            &exact_product))
      return false;
  }
  if (!dfs_checked_multiply_u64(
          exact_product, uint64_t(selected.wild_span),
          &selected.projected_state_count))
    return false;
  selected.effective_state_count = selected.projected_state_count;
  if (selected.exact_mask != 0) {
    int const root_rank = __builtin_ctzll(selected.exact_mask);
    uint64_t const root_radix = uint64_t(bag[size_t(root_rank)]) + 1;
    selected.effective_state_count =
        (selected.projected_state_count / root_radix) * (root_radix - 1);
  }
  selected.root_key = selected.projected_state_count - 1;
  *result = selected;
  return true;
}

DfsAnagramSearch::DfsAnagramSearch(DfsClassList const* classes,
                                   std::string const& letters,
                                   double segment_penalty,
                                   int64_t corpus_total,
                                   size_t score_cache_bytes,
                                   size_t preprocess_threads,
                                   size_t search_threads,
                                   size_t exact_segments,
                                   double word_bonus):
    class_list(classes),
    letters(letters),
    score_model(segment_penalty, corpus_total, word_bonus),
    segment_boundary_log_score(
        score_model.segment_boundary_log_score()),
    exact_segments(exact_segments),
    // A target below the letters' natural depth limit is itself the limit;
    // a target above it leaves the walk unchanged and simply finds nothing.
    max_depth(std::min(derived_max_depth(classes, letters.size()),
                       exact_segments != 0 ? exact_segments : SIZE_MAX)),
    score_cache_budget(score_cache_bytes),
    requested_preprocess_threads(std::max(size_t(1), preprocess_threads)),
    requested_search_threads(std::max(size_t(1), search_threads)),
    support_scan_vector(support_scan_avx2_enabled()) {
  assert(class_list != NULL);
  // members[0] is exactly what a global member sort destroys, and this is the
  // only production read of the member store outside output.
  assert(!class_list->members_invalidated());
  DfsClassSpan const all_classes = class_list->classes();
  best_member_log_scores.reserve(all_classes.size());
  // These are optimistic per-class bounds that phase 2 prunes against, so each
  // must be the best *score* in its class. Members are sorted by count, which
  // makes member 0 the answer only when no bonus is in play; a nonzero bonus
  // can lift a rarer multi-word member above it, so scan when that can happen.
  // Under a zero bonus the scan is skipped and member 0 stands.
  bool const bonus_reorders = score_model.multi_word_log_bonus() != 0.0;
  for (size_t i = 0; i < all_classes.size(); ++i) {
    size_t const members = class_list->member_count(i);
    assert(members > 0);
    DfsMemberView const first = class_list->member(i, 0);
    assert(first.count > 0);
    double best = score_model.first_segment_log_score(
        first.count, first.word_count > 1);
    if (bonus_reorders) {
      for (size_t m = 1; m < members; ++m) {
        DfsMemberView const other = class_list->member(i, m);
        assert(other.count > 0);
        best = std::max(best, score_model.first_segment_log_score(
            other.count, other.word_count > 1));
      }
    }
    best_member_log_scores.push_back(best);
  }
}

bool DfsAnagramSearch::prepare_hot_classes(
    DfsSearchData* data, ScoreKeyLayout const& layout,
    std::unique_ptr<uint16_t[], DfsAlignedFree>* score_wild_lengths,
    char const** failure_reason) {
  static_assert(sizeof(FitClassMetadata) == sizeof(uint64_t),
                "FitClass metadata must remain one register wide");
  static_assert(std::is_trivially_copyable<FitClassMetadata>::value,
                "FitClass metadata must remain trivially copyable");
  static_assert(sizeof(FitClass) == 16,
                "FitClass must remain four per cache line");
  score_wild_lengths->reset();
  *failure_reason = NULL;

  auto const unsupported = [failure_reason](char const* reason) {
    *failure_reason = reason;
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
  std::unique_ptr<FitClass[], DfsAlignedFree> new_fit(fit);

  uint64_t* supports = static_cast<uint64_t*>(
      dfs_allocate_aligned(classes.size() * sizeof(uint64_t)));
  if (supports == NULL)
    return unsupported("could not allocate the support masks");
  std::unique_ptr<uint64_t[], DfsAlignedFree> new_supports(supports);

  uint64_t* score_deltas = static_cast<uint64_t*>(
      dfs_allocate_aligned(classes.size() * sizeof(uint64_t)));
  if (score_deltas == NULL)
    return unsupported("could not allocate the score-key deltas");
  std::unique_ptr<uint64_t[], DfsAlignedFree> new_score_deltas(score_deltas);

  uint16_t* wild_lengths = static_cast<uint16_t*>(
      dfs_allocate_aligned(classes.size() * sizeof(uint16_t)));
  if (wild_lengths == NULL)
    return unsupported("could not allocate the wildcard lengths");
  std::unique_ptr<uint16_t[], DfsAlignedFree> new_wild_lengths(wild_lengths);

  uint32_t* packed = NULL;
  if (requirements != 0) {
    packed = static_cast<uint32_t*>(
        dfs_allocate_aligned(requirements * sizeof(uint32_t)));
    if (packed == NULL)
      return unsupported("could not allocate the packed letters");
  }
  std::unique_ptr<uint32_t[], DfsAlignedFree> new_packed(packed);

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
      if ((layout.exact_mask & (UINT64_C(1) << rank)) != 0) {
        if (!dfs_checked_multiply_u64(
                count, layout.multipliers[rank], &term) ||
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
            score_delta, uint64_t(layout.wild_span), &flat_score_delta) ||
        !dfs_checked_add_u64(
            flat_score_delta, uint64_t(wild_length),
            &flat_score_delta))
      return unsupported("flattened score-key delta overflowed 64 bits");
    score_deltas[ci] = flat_score_delta;
    wild_lengths[ci] = uint16_t(wild_length);
    offset = write;
  }

  data->fit_classes = std::move(new_fit);
  data->class_supports = std::move(new_supports);
  data->score_key_deltas = std::move(new_score_deltas);
  *score_wild_lengths = std::move(new_wild_lengths);
  data->packed_letters = std::move(new_packed);
  return true;
}

bool DfsAnagramSearch::prepare_length_certificate(DfsSearchData* data) {
  auto const fail = [data]() {
    data->certificate_stride = 0;
    data->certificate_max_score.clear();
    data->certificate_group_end.clear();
    data->length_tail_bounds.clear();
    data->certificate_ready = false;
    return false;
  };
  fail();
  if (letters.size() == SIZE_MAX) return false;

  try {
    DfsClassSpan const classes = class_list->classes();
    size_t max_length = 0;
    for (size_t i = 0; i < classes.size(); ++i)
      max_length = std::max<size_t>(max_length, classes[i].key_length);
    if (max_length == SIZE_MAX) return false;
    data->certificate_stride = max_length + 1;
    if (data->certificate_stride != 0 &&
        size_t(DFS_SYMBOL_COUNT) >
            SIZE_MAX / data->certificate_stride)
      return false;
    size_t const group_entries =
        size_t(DFS_SYMBOL_COUNT) * data->certificate_stride;
    data->certificate_max_score.assign(group_entries, -HUGE_VAL);
    data->certificate_group_end.assign(group_entries, 0);

    std::vector<double> best_score(
        letters.size() + 1, -HUGE_VAL);
    for (size_t rank = 0; rank < DFS_SYMBOL_COUNT; ++rank) {
      int const symbol = class_list->rank_to_symbol()[rank];
      size_t const begin = class_list->candidate_begin(symbol);
      size_t const end = class_list->candidate_end(symbol);
      size_t const base = rank * data->certificate_stride;
      size_t previous_length = SIZE_MAX;
      for (size_t i = begin; i < end; ++i) {
        size_t const length = classes[i].key_length;
        if (length >= data->certificate_stride ||
            length > letters.size())
          return fail();
        if (previous_length != SIZE_MAX &&
            length > previous_length)
          return fail();
        previous_length = length;
        data->certificate_max_score[base + length] = std::max(
            data->certificate_max_score[base + length],
            best_member_log_scores[i]);
        data->certificate_group_end[base + length] = uint32_t(i + 1);
        best_score[length] = std::max(
            best_score[length], best_member_log_scores[i]);
      }
    }

    data->length_tail_bounds.assign(letters.size() + 1, -HUGE_VAL);
    data->length_tail_bounds[0] = 0.0;
    uint64_t ignored_nextafter_calls = 0;
    for (size_t left = 1; left <= letters.size(); ++left) {
      double best = -HUGE_VAL;
      double max_rounding_error = 0.0;
      for (size_t length = 1; length <= left; ++length) {
        if (best_score[length] == -HUGE_VAL ||
            data->length_tail_bounds[left - length] == -HUGE_VAL)
          continue;
        double const child = data->length_tail_bounds[left - length];
        double const candidate =
            best_score[length] + segment_boundary_log_score + child;
        best = std::max(best, candidate);
        max_rounding_error = std::max(
            max_rounding_error,
            score_candidate_rounding_error(
                best_score[length], segment_boundary_log_score, child));
      }
      if (best != -HUGE_VAL)
        data->length_tail_bounds[left] = round_score_bound_up(
            static_cast<long double>(best) +
                static_cast<long double>(max_rounding_error),
            &ignored_nextafter_calls);
    }
  } catch (...) {
    return fail();
  }
  data->certificate_ready = true;
  return true;
}

bool DfsSearchData::certificate_rejects(
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

size_t DfsSearchData::first_length_candidate(
    size_t begin, size_t end, size_t letters_left) const {
  while (begin < end) {
    size_t const middle = begin + (end - begin) / 2;
    size_t const candidate_length = hot_letter_length(
        fit_classes[middle].metadata.packed_length_and_count);
    if (candidate_length > letters_left)
      begin = middle + 1;
    else
      end = middle;
  }
  return begin;
}

bool DfsAnagramSearch::prepare_phase_two(
    DfsSearchData* data, DfsSearchStats* stats,
    int64_t progress_factor, bool allow_cache_fallback, int exact_letters,
    bool score_bounds_requested) {
  FILE* const progress = dfs_diagnostic_stream();
  typedef std::chrono::steady_clock PhaseClock;
  PhaseClock::time_point const setup_start = PhaseClock::now();
  *data = DfsSearchData();
  *stats = DfsSearchStats();
  data->class_list = class_list;
  data->score_model = score_model;
  data->best_member_log_scores = best_member_log_scores;
  data->segment_boundary_log_score = segment_boundary_log_score;
  data->letter_count = letters.size();
  data->max_depth = max_depth;
  data->exact_depth = exact_segments;
  data->min_word_length = size_t(class_list->min_word_length());
  data->support_scan_vector = support_scan_vector;
  data->requested_search_threads = requested_search_threads;
  bool certificate_requested = false;
  std::array<int, DFS_SYMBOL_COUNT> const& symbol_to_rank =
      class_list->symbol_to_rank();
  bool encodable = true;
  for (size_t i = 0; i < letters.size(); ++i) {
    int const symbol = dfs_symbol_index((unsigned char) letters[i]);
    assert(symbol >= 0);
    uint32_t& count = data->bag[size_t(symbol_to_rank[size_t(symbol)])];
    if (count == UINT32_MAX) encodable = false;
    ++count;
  }

  uint64_t state_count = 1;
  if (encodable) {
    for (int rank = DFS_SYMBOL_COUNT - 1; rank >= 0; --rank) {
      uint64_t const radix = uint64_t(data->bag[size_t(rank)]) + 1;
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
  data->exact_root_key = state_count - 1;
  data->bag_mask = 0;
  for (int rank = 0; rank < DFS_SYMBOL_COUNT; ++rank)
    if (data->bag[size_t(rank)] != 0)
      data->bag_mask |= UINT64_C(1) << rank;

  ScoreKeyLayout layout;
  if (!ScoreKeyLayout::choose(
          data->bag, letters.size(), score_cache_budget, exact_letters,
          &layout))
    abort_phase_two("data->bag state count exceeds 64 bits");
  data->score_key = layout.root_key;
  data->score_wild_letters = layout.wild_letters;
  bool const empty_class_list = class_list->classes().empty();
  std::unique_ptr<uint16_t[], DfsAlignedFree> score_wild_lengths;
  bool hot_classes_ready = false;
  char const* hot_class_failure = NULL;
  if (!empty_class_list) {
    hot_classes_ready = prepare_hot_classes(
        data, layout, &score_wild_lengths, &hot_class_failure);
    if (!hot_classes_ready) abort_phase_two(hot_class_failure);
  }
  ProjectedActions projected_actions;
  bool projected_actions_ready = false;
  if (hot_classes_ready) {
    projected_actions_ready = ProjectedActions::build(
        *data, layout, score_wild_lengths.get(), &projected_actions);
    if (!projected_actions_ready)
      abort_phase_two("could not build projected actions");
  }
  score_wild_lengths.reset();
  if (progress != NULL) {
    size_t projected_bytes = 0;
    bool const projected_size_ok = projected_bound_requirements(
        layout.effective_state_count, sizeof(float), &projected_bytes);
    dfs_diagnostic(
        "phase 2 preflight: projected score table keeps %zu "
        "rarest letters exact, merges %zu wildcard letters; "
        "%llu states, ",
        layout.exact_letters, layout.wild_letters,
        (unsigned long long) layout.effective_state_count);
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
    fflush(progress);
  }
  bool const score_bounds_applicable =
      hot_classes_ready && projected_actions_ready && score_bounds_requested &&
      projected_score_bound_arithmetic_supported() &&
      data->bag_mask != 0;
  int const certificate_mode = length_certificate_mode();
  certificate_requested =
      certificate_mode != 0 && score_bounds_applicable;
  data->certificate_shadow = certificate_mode == 1;
  if (certificate_requested) {
    if (!prepare_length_certificate(data))
      data->certificate_ready = false;
  }
  size_t required_bytes = 0;
  bool const size_available = !score_bounds_applicable ||
      projected_bound_requirements(
          layout.effective_state_count, sizeof(float), &required_bytes);
  bool const score_bounds_selected =
      score_bounds_applicable && size_available &&
      required_bytes <= score_cache_budget;
  if (!allow_cache_fallback && score_bounds_applicable &&
      !score_bounds_selected) {
    char const* const mode_name =
        score_bound_mode_name(DFS_SCORE_BOUND_PROJECTED);
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
  if (score_bounds_selected) {
    BoundStateView const root = {
        {data->bag.data(), data->bag_mask}, data->score_key,
        data->letter_count, data->score_wild_letters};
    if (!data->score_bounds.build(
            root, layout, projected_actions, score_cache_budget,
            requested_preprocess_threads, stats))
      abort_phase_two("could not build projected score bounds");
  } else if (progress != NULL) {
    dfs_diagnostic("phase 2 preflight: score-bound mode off\n");
    fflush(progress);
  }
  data->score_bounds_active = data->score_bounds.active();
  // Snapshot both components now: the bound table and the certificate tables
  // are moved into the runner and destroyed with it, but their statistics are
  // part of this call's result.
  stats->bounds = data->score_bounds.stats();
  stats->bounds.exact_letters = layout.exact_letters;
  stats->bounds.wild_letters = layout.wild_letters;
  stats->bounds.projected_actions = projected_actions.size();
  stats->certificate.ready = data->certificate_ready;
  stats->certificate.shadow = data->certificate_shadow;
  stats->certificate.table_bytes = data->certificate_table_bytes();

  data->progress_enabled = progress != NULL;
  int64_t const normalized_progress_factor =
      std::max<int64_t>(progress_factor, 1);
  data->progress_interval =
      normalized_progress_factor <= INT64_MAX / INT64_C(100000)
          ? INT64_C(100000) * normalized_progress_factor
          : INT64_MAX;

  PhaseClock::time_point const setup_end = PhaseClock::now();
  stats->execution.setup_seconds =
      std::chrono::duration<double>(setup_end - setup_start).count();
  if (data->progress_enabled) {
    dfs_diagnostic(
        "phase 2: precomputed %zu bounded states in %.1fs\n",
        stats->bounds.projected.states_computed,
        stats->execution.setup_seconds);
  }
  return true;
}

bool DfsAnagramSearch::run(DfsSolutionSink* sink, DfsSearchStats* stats,
                           int64_t progress_factor,
                           bool allow_cache_fallback,
                           int exact_letters,
                           bool verbose) {
  DfsSearchStats discarded;
  if (stats == NULL) stats = &discarded;
  bool const score_bounds_requested =
      sink != NULL && sink->supports_score_pruning();
  DfsSearchData data;
  if (!prepare_phase_two(
          &data, stats, progress_factor, allow_cache_fallback, exact_letters,
          score_bounds_requested))
    return false;

  typedef std::chrono::steady_clock PhaseClock;
  PhaseClock::time_point const search_start = PhaseClock::now();
  if (class_list->classes().empty()) {
    stats->execution.search_seconds = 0.0;
    return true;
  }
  DfsAllSolutionsRunner runner(std::move(data));
  runner.run(sink, progress_factor, verbose, stats);
  stats->execution.search_seconds =
      std::chrono::duration<double>(PhaseClock::now() - search_start).count();
  return true;
}

bool DfsAnagramSearch::find_completable_classes(
    std::vector<bool>* completable, DfsSearchStats* stats,
    int64_t progress_factor, bool allow_cache_fallback, int exact_letters) {
  assert(completable != NULL);
  DfsSearchStats discarded;
  if (stats == NULL) stats = &discarded;
  DfsSearchData data;
  if (!prepare_phase_two(
          &data, stats, progress_factor, allow_cache_fallback, exact_letters,
          true))
    return false;
  typedef std::chrono::steady_clock PhaseClock;
  PhaseClock::time_point const validation_start = PhaseClock::now();
  DfsAnySolutionRunner runner(std::move(data));
  bool const ok = runner.run(completable, stats);
  stats->execution.search_seconds = std::chrono::duration<double>(
      PhaseClock::now() - validation_start).count();
  return ok;
}
