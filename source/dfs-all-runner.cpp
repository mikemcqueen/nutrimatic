#include <assert.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>

#include <algorithm>
#include <thread>

#include "dfs-diagnostic.h"
#include "dfs-search-internal.h"
#include "dfs-search.h"

namespace {

size_t search_task_target(size_t threads) {
  size_t target = threads > SIZE_MAX / 64 ? SIZE_MAX : threads * 64;
  char const* forced = getenv("NUTRIMATIC_SEARCH_TASKS");
  if (forced == NULL || forced[0] == '\0') return target;
  errno = 0;
  char* end = NULL;
  unsigned long long const parsed = strtoull(forced, &end, 10);
  if (errno == 0 && end != forced && *end == '\0' && parsed != 0
      && parsed <= SIZE_MAX)
    return size_t(parsed);
  return target;
}

}  // namespace

DfsAnagramSearch::DfsAllSolutionsRunner::DfsAllSolutionsRunner(
    DfsAnagramSearch& search)
    : search(search) {
}

bool DfsAnagramSearch::DfsAllSolutionsRunner::multiplicity_fits(
    FitClassMetadata metadata, Worker const& worker) const {
  uint32_t const* requirements =
      search.packed_letters.get() + metadata.letters_offset;
  uint32_t const repeated =
      hot_repeated_count(metadata.packed_length_and_count);
  for (uint32_t i = 0; i < repeated; ++i) {
    uint32_t const requirement = requirements[i];
    if (worker.bag[packed_rank(requirement)] < packed_count(requirement))
      return false;
  }
  return true;
}

bool DfsAnagramSearch::DfsAllSolutionsRunner::should_prune(Worker* worker,
    double representative_log_score, DfsSolutionSink* sink,
    size_t letters_left) {
  Reachability const reachability =
      search.cached_reachability(worker->score_key, worker->path.empty());
  if (reachability == REACHABILITY_NO) return true;
  if (worker->path.empty()) return false;

  double floor;
  if (sink == NULL || !sink->score_floor(&floor)) return false;
  uint64_t const query_key = worker->score_key;
  if (query_key >= search.search_stats.score_bounds.capacity) return false;

  double remaining_bound;
  if (!search.load_score_bound(query_key, &remaining_bound)) return false;
  if (remaining_bound == -HUGE_VAL) return true;
  long double const upper = static_cast<long double>(representative_log_score)
                            + static_cast<long double>(remaining_bound);
  long double const magnitude =
      fabsl(static_cast<long double>(representative_log_score))
      + fabsl(static_cast<long double>(remaining_bound))
      + fabsl(static_cast<long double>(floor)) + 1.0L;
  long double const padding =
      magnitude * static_cast<long double>(DBL_EPSILON)
      * (static_cast<long double>(search.max_depth) + 2.0L) * 16.0L;
  return upper + padding <= static_cast<long double>(floor);
}

void DfsAnagramSearch::DfsAllSolutionsRunner::visit_fitting_class(
    Worker* worker, uint32_t class_index, FitClassMetadata metadata,
    size_t letters_left, double representative_log_score,
    DfsSolutionSink* sink) {
  if (DFS_UNLIKELY(worker->path.size() >= search.max_depth)) return;
  double const class_score = search.best_member_log_scores[class_index];
  size_t const candidate_length =
      hot_letter_length(metadata.packed_length_and_count);
  assert(candidate_length <= letters_left);
  size_t const next_letters_left = letters_left - candidate_length;
  bool const first_class = worker->path.empty();
  worker->path.push_back(class_index);
  double const next_log_score =
      first_class ? class_score
                  : search.score_model.append_log_score(
                        representative_log_score, class_score);
  if (DFS_UNLIKELY(next_letters_left == 0)) {
    ++worker->stats.solutions;
    if (sink != NULL) sink->emit(worker->path, next_log_score);
    worker->path.pop_back();
    return;
  }

  uint32_t const* requirements =
      search.packed_letters.get() + metadata.letters_offset;
  uint32_t const requirement_count =
      hot_requirement_count(metadata.packed_length_and_count);
  uint64_t const parent_bag_mask = worker->bag_mask;
  for (uint32_t i = 0; i < requirement_count; ++i) {
    uint32_t const requirement = requirements[i];
    uint32_t const rank = packed_rank(requirement);
    uint32_t& remaining = worker->bag[rank];
    remaining -= packed_count(requirement);
    worker->bag_mask &= ~(uint64_t(remaining == 0) << rank);
  }
  worker->score_key -= search.score_key_deltas.get()[class_index];
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
  worker->score_key += search.score_key_deltas.get()[class_index];
  for (uint32_t i = 0; i < requirement_count; ++i) {
    uint32_t const requirement = requirements[i];
    worker->bag[packed_rank(requirement)] += packed_count(requirement);
  }
  worker->bag_mask = parent_bag_mask;
  worker->path.pop_back();
}

bool DfsAnagramSearch::DfsAllSolutionsRunner::visit_fitting_range(
    Worker* worker, size_t begin, size_t end, size_t letters_left,
    double representative_log_score, DfsSolutionSink* sink) {
  for (size_t class_index = begin; class_index < end; ++class_index) {
    uint32_t const id = uint32_t(class_index);
    FitClass const& fit = search.fit_classes.get()[id];
    if ((fit.support_mask & ~worker->bag_mask) != 0
        || !multiplicity_fits(fit.metadata, *worker))
      continue;
    visit_fitting_class(
        worker, id, fit.metadata, letters_left, representative_log_score, sink);
    if (DFS_UNLIKELY(sink != NULL && sink->should_stop())) return true;
  }
  return false;
}

void DfsAnagramSearch::DfsAllSolutionsRunner::walk(Worker* worker,
    size_t letters_left, size_t entry_point, double representative_log_score,
    DfsSolutionSink* sink) {
  ++worker->stats.nodes;
  if (DFS_UNLIKELY(search.progress_enabled
                   && worker->stats.nodes == worker->next_progress))
    report_progress(worker);
  if (DFS_UNLIKELY(worker->bag_mask == 0)) return;
  if (DFS_UNLIKELY(
          should_prune(worker, representative_log_score, sink, letters_left))) {
    ++worker->stats.bound_prunes;
    return;
  }
  int const rank = __builtin_ctzll(worker->bag_mask);
  int const forced_symbol = search.class_list->rank_to_symbol()[size_t(rank)];
  size_t const begin = search.first_length_candidate(
      search.class_list->candidate_begin(forced_symbol),
      search.class_list->candidate_end(forced_symbol), letters_left);
  size_t const end = search.class_list->candidate_end(forced_symbol);
  size_t const start = std::max(begin, entry_point);
  if (DFS_UNLIKELY(search.length_certificate_ready) && !worker->path.empty()) {
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

void DfsAnagramSearch::DfsAllSolutionsRunner::walk_certified(Worker* worker,
    int rank, size_t start, size_t end, size_t letters_left,
    double representative_log_score, double floor, DfsSolutionSink* sink) {
  size_t const base = size_t(rank) * search.certificate_stride;
  for (size_t class_index = start; class_index < end;) {
    size_t const length = hot_letter_length(
        search.fit_classes.get()[class_index].metadata.packed_length_and_count);
    size_t const group_end =
        size_t(search.certificate_group_end[base + length]);
    assert(group_end > class_index && group_end <= end);
    ++worker->stats.certificate.group_tests;
    bool const rejected = search.length_certificate_rejects(
        base, length, letters_left, representative_log_score, floor);
    if (rejected) {
      ++worker->stats.certificate.group_rejects;
      worker->stats.certificate.scans_skipped += group_end - class_index;
      if (!search.length_certificate_shadow) {
        class_index = group_end;
        continue;
      }
    } else {
      worker->stats.certificate.scans_kept += group_end - class_index;
    }
    if (visit_fitting_range(worker, class_index, group_end, letters_left,
            representative_log_score, sink))
      return;
    class_index = group_end;
  }
}

void DfsAnagramSearch::DfsAllSolutionsRunner::start_worker(Worker* worker) {
  worker->bag = search.bag;
  worker->bag_mask = search.bag_mask;
  worker->score_key = search.current_score_key;
  worker->path.clear();
  worker->path.reserve(search.letters.size());
  worker->stats.clear();
  worker->split_depth = 0;
  worker->produced = NULL;
  worker->next_progress = search.progress_enabled ? search.progress_interval
                                                  : INT64_MAX;
  worker->reported_solutions = 0;
}

void DfsAnagramSearch::DfsAllSolutionsRunner::report_progress(Worker* worker) {
  worker->next_progress = worker->next_progress
                                  <= INT64_MAX - search.progress_interval
                              ? worker->next_progress + search.progress_interval
                              : INT64_MAX;
  int64_t const total_nodes = progress_nodes.fetch_add(search.progress_interval,
                                  std::memory_order_relaxed)
                              + search.progress_interval;
  int64_t const new_solutions =
      worker->stats.solutions - worker->reported_solutions;
  worker->reported_solutions = worker->stats.solutions;
  int64_t const total_solutions =
      progress_solutions.fetch_add(new_solutions, std::memory_order_relaxed)
      + new_solutions;
  dfs_diagnostic("phase 2: %lld nodes, %lld solutions\n",
      (long long)total_nodes, (long long)total_solutions);
}

void DfsAnagramSearch::DfsAllSolutionsRunner::merge_worker(
    Worker const& worker) {
  search.search_stats.all_solutions.add(worker.stats);
}

void DfsAnagramSearch::DfsAllSolutionsRunner::run_parallel(
    DfsSolutionSink* sink, size_t threads, size_t target_tasks,
    uint64_t task_progress_factor, bool verbose) {
  std::vector<SearchTask> tasks;
  Worker seed;
  start_worker(&seed);
  seed.split_depth = 1;
  seed.produced = &tasks;
  walk(&seed, search.letters.size(), 0, 0.0, sink);
  if (verbose)
    dfs_diagnostic("info: added %zu child tasks (%zu total)\n", tasks.size(),
        tasks.size());
  size_t head = 0;
  std::vector<SearchTask> children;
  while (tasks.size() - head < target_tasks && head < tasks.size()
         && tasks[head].path_size < MAX_SPLIT_DEPTH) {
    SearchTask const& task = tasks[head++];
    children.clear();
    seed.bag = task.bag;
    seed.bag_mask = task.bag_mask;
    seed.score_key = task.score_key;
    seed.path.assign(task.path.begin(), task.path.begin() + task.path_size);
    seed.split_depth = task.path_size + 1;
    seed.produced = &children;
    walk(&seed, task.letters_left, task.entry_point,
        task.representative_log_score, sink);
    tasks.insert(tasks.end(), children.begin(), children.end());
    if (verbose)
      dfs_diagnostic("info: added %zu child tasks (%zu total)\n",
          children.size(), tasks.size() - head);
  }
  merge_worker(seed);
  search.search_stats.run.search_tasks = uint64_t(tasks.size() - head);
  if (verbose)
    dfs_diagnostic("info: task splitting complete (%llu total)\n",
        (unsigned long long)search.search_stats.run.search_tasks);
  if (head >= tasks.size()) return;

  size_t const worker_count = std::min(threads, tasks.size() - head);
  std::atomic<size_t> next_task(head);
  std::vector<Worker> workers(worker_count);
  std::vector<std::thread> pool;
  pool.reserve(worker_count - 1);
  for (size_t i = 0; i < worker_count; ++i) start_worker(&workers[i]);
  auto const body = [&](size_t index) {
    Worker& worker = workers[index];
    bool claimed_task = false;
    for (;;) {
      size_t const slot = next_task.fetch_add(1, std::memory_order_relaxed);
      if (slot >= tasks.size()) {
        if (verbose) dfs_diagnostic("info: search worker %zu exited\n", index);
        break;
      }
      if (!claimed_task) {
        claimed_task = true;
        if (verbose)
          dfs_diagnostic("info: worker %zu claimed first task\n", index);
      }
      size_t const tasks_left = tasks.size() - slot - 1;
      if (verbose && uint64_t(tasks_left) % task_progress_factor == 0)
        dfs_diagnostic(
            "info: worker %zu popped task (%zu total)\n", index, tasks_left);
      SearchTask const& task = tasks[slot];
      worker.bag = task.bag;
      worker.bag_mask = task.bag_mask;
      worker.score_key = task.score_key;
      worker.path.assign(task.path.begin(), task.path.begin() + task.path_size);
      walk(&worker, task.letters_left, task.entry_point,
          task.representative_log_score, sink);
    }
  };
  try {
    for (size_t i = 1; i < worker_count; ++i)
      pool.push_back(std::thread(body, i));
  } catch (...) {}
  search.search_stats.run.search_threads = 1 + pool.size();
  body(0);
  for (std::thread& thread : pool) thread.join();
  for (size_t i = 0; i < search.search_stats.run.search_threads; ++i)
    merge_worker(workers[i]);
}

void DfsAnagramSearch::DfsAllSolutionsRunner::run(
    DfsSolutionSink* sink, int64_t progress_factor, bool verbose) {
  search.search_stats.all_solutions.clear();
  progress_nodes.store(0, std::memory_order_relaxed);
  progress_solutions.store(0, std::memory_order_relaxed);
  bool const parallel_eligible =
      search.requested_search_threads > 1
      && (sink == NULL || sink->supports_parallel_search());
  if (parallel_eligible) {
    run_parallel(sink, search.requested_search_threads,
        search_task_target(search.requested_search_threads),
        uint64_t(std::max<int64_t>(progress_factor, 1)), verbose);
    return;
  }
  Worker worker;
  start_worker(&worker);
  walk(&worker, search.letters.size(), 0, 0.0, sink);
  merge_worker(worker);
}
