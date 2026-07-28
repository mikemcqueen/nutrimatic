# Plan: short-circuit `DfsAnagramSearch` for a `query-index` completability filter

> Superseded by `plans/fast-query-index.md`. The per-candidate class-list and
> search design below was replaced by one full-bag phase-2 preparation plus
> shared exact-remainder memoization.

## Goal

Let `query-index` (phase 1 only today) optionally show only classes whose
removal from the letter bag still leaves a remainder that phase 2 could
*fully* turn into a valid anagram (subject to `-m`/min-word-length). Today
`query-index` ranks vocabulary by raw corpus frequency without checking that
any complete anagram actually exists — a top-ranked entry may be a dead end.

## Settled decisions

- **Reuse `DfsAnagramSearch`'s real backtracking, don't reimplement it.**
  The engine's `hot_class_fits`/`visit_fitting_class` machinery already is a
  letters-exact, min-word-len-aware, entry-point-deduped DFS over whole
  classes — precisely the feasibility question this needs. A hand-written
  parallel feasibility search would have to reproduce all of that (rarest-
  letter selection, repeated-symbol dedup, dictionary-filtered classes,
  max_depth) with no guarantee it stays in sync with the real solver as it
  evolves. Discarded as more surface area for the same answer.
- **Existence check, not a ranked score.** `query-index`'s filter only needs
  "does at least one completion exist," not the best one. This means the new
  sink does not need `supports_score_pruning()`, which in turn means
  `prepare_score_bounds` and the length certificate stay off automatically
  (`!sink->supports_score_pruning()` already short-circuits that setup at
  `dfs-search.cpp:948`) — no new "existence mode" constructor flag required.
- **`should_stop()` on the sink, checked once per candidate in `walk`'s and
  `walk_certified`'s loops.** This is the minimal change that lets recursion
  unwind as soon as one solution is found, without touching pruning logic,
  scoring, or the hot-class fit tests themselves.
- **Force `search_threads = 1` for this use.** `run_parallel_search`'s task
  splitting has no concept of a cross-worker stop signal, and propagating one
  through already-dispatched `SearchTask`s is real added complexity for a
  query that's supposed to be cheap by construction (first-solution search,
  not exhaustive top-N). Not in scope. If a future workload needs a fast
  parallel existence check, that's a separate plan.
- **Per-class caching in `query-index`, not per-member.** All members of a
  `DfsAnagramClass` share one `key` (sorted letters), so "is the rest of the
  bag completable after removing this class" only needs to be computed once
  per class and reused across every member printed from it.
- **Default off.** The new `query-index` flag changes what gets filtered and
  costs extra search time per candidate class; existing invocations and
  `test-query-index.sh` must be unaffected without passing it.

## Non-goals

- Parallelizing the first-solution search.
- Reusing the score-bound cache (dense/projected) for this check. That cache
  answers a related but different question (an admissible score upper bound)
  and pulling it in means standing up `prepare_hot_classes` +
  `prepare_projected_actions` machinery this query doesn't need. The plain
  hot-class backtracking search is already the cheap path once the sink opts
  out of score pruning.
- Changing `dfs-anagrams`' own default behavior or output.

## Implementation

### 1. `DfsSolutionSink::should_stop()`

In `dfs-search.h`, add:

```cpp
// Search checks this after every emitted solution at each recursion level
// and unwinds as soon as it returns true. Default false preserves today's
// exhaustive enumeration for every existing sink.
virtual bool should_stop() const { return false; }
```

### 2. Check it in the two class-iteration loops

`dfs-search.cpp:2792` (`walk`'s loop over `[start, end)`):

```cpp
for (size_t class_index = start; class_index < end; ++class_index) {
  uint32_t const id = uint32_t(class_index);
  if (!hot_class_fits(id, *worker)) continue;
  visit_fitting_class(worker, id, letters_left, representative_log_score, sink);
  if (sink != NULL && sink->should_stop()) return;
}
```

And the equivalent loop in `walk_certified` (`dfs-search.cpp:2800` on). Both
loops call `visit_fitting_class`, which is where `sink->emit()` actually
happens (`dfs-search.cpp:2701`) and where the recursive `walk()` call for a
partial match lives (`dfs-search.cpp:2745`) — checking right after that call
returns is what lets a deep solution unwind all enclosing loop frames, not
just the innermost one.

No change to `visit_fitting_class`, `should_prune`, `hot_class_fits`, or any
scoring/bound code.

### 3. `DfsFirstSolutionSink`

A small sink (new class, likely alongside `DfsTopN` in `dfs-output.h`/`.cpp`,
or local to `query-index.cpp` if it has no other caller):

```cpp
class DfsFirstSolutionSink : public DfsSolutionSink {
 public:
  void emit(std::vector<size_t> const&, double) override { found_ = true; }
  bool should_stop() const override { return found_; }
  bool found() const { return found_; }
 private:
  bool found_ = false;
};
```

### 4. `query-index` flag

- New option, e.g. `--require-completable` (off by default).
- When set, before ranking: for each `DfsAnagramClass`, compute
  `remaining = args.letters - class.key`, build (or reuse) a
  `DfsAnagramSearch` over `remaining` with `search_threads = 1`, run it with a
  `DfsFirstSolutionSink`, and drop the class from `flattened` if no solution
  is found.
- Cache the boolean result per class key within one `query-index` run (per
  settled decision above); no cross-run persistence needed.
- `usage()` gets one line documenting the flag and noting it costs extra time
  per candidate class.

## Verification

- Existing `test-query-index.sh` and `test-dfs-search.cpp`/
  `test-dfs-cli.sh` must pass unmodified (default `should_stop() == false`
  means today's sinks enumerate exactly as before).
- New smoke test (kept minimal per project convention): a small letter bag
  and dictionary, run with and without `--require-completable`, confirm the
  flag removes at least one class that phase 2 genuinely can't complete
  (e.g. leaves a remainder shorter than `-m`) and keeps a class that can be
  completed.
- `/code-review` before committing, per `CLAUDE.md`.

## Open questions for review, not blocking the plan

- Whether `--require-completable` should imply a default `-m` derived from
  the *remaining* letters the same way `dfs-anagrams` derives `max_words`
  (`dfs-anagrams.cpp:224-233`), or should require the caller to pass `-m`
  explicitly since `query-index`'s current default min-word-length is 1, not
  4.
