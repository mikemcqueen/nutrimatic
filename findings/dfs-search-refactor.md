# Splitting `dfs-search` into coherent subsystems

## Scope

This is design analysis only. It does not propose changing search semantics,
score arithmetic, candidate order, environment-variable behavior, diagnostics,
or public command-line behavior as part of the initial split.

`source/dfs-search.cpp` is currently 3,744 lines and
`source/dfs-search.h` is 507 lines. The size is a symptom of a more important
problem: `DfsAnagramSearch` currently owns all of these responsibilities:

1. encoding the input bag and projected score key;
2. constructing the hot class layout;
3. constructing an optional length certificate;
4. selecting and allocating dense, prefix, or projected score bounds;
5. building those bounds serially or in parallel;
6. running the ordinary solution-enumerating DFS;
7. splitting that DFS into parallel tasks;
8. running the separate exact-completion DFS used by `query-index`;
9. owning both score-bound and exact-completion memo tables;
10. selecting two unrelated SIMD kernels;
11. collecting every subsystem's counters and exposing them as individual
    getters.

The implementation has recognizable internal subsystems, but their state is
flattened into one class and their functions are interleaved in one
translation unit. The most useful refactor is therefore a two-stage one:

1. make low-risk physical splits while preserving the existing class and
   public API;
2. once those boundaries have settled, replace the shared private-state
   surface with small internal components and explicit inputs and outputs.

The first stage is mostly code movement. The second is where the cleaner API
and smaller header come from. They should not be combined into one large
behavioral change.

## Current anatomy

Approximate sizes count related anonymous-namespace helpers as part of the
subsystem that uses them. Some shared helpers are counted separately.

| Current area | Principal line ranges | Approximate size | Responsibility |
|---|---:|---:|---|
| Shared arithmetic, packing, and selection helpers | 29-149, 379-494 | 240 | bit conversion, upward rounding, environment choices, configuration parsing |
| Exact support-mask SIMD | 318-378 | 61 | scalar and AVX2 first-fitting-candidate scan |
| Construction and common preparation | 496-749, 1225-1643 | 675 | constructor, hot layout, phase-two configuration, public `run()` |
| Length certificate | 750-861, 3459-3493 | 147 | certificate tables, rejection calculation, grouped scan |
| Projected action preparation | 862-1011 | 150 | quotienting, bucketing, sorting, repeated requirements |
| Bound storage | 1013-1224 | 212 | table choice, allocation, load/store/publish, reachability |
| Exact completability | 1644-2178 | 535 | exact memo, lookahead, exact recurrence, batch validation |
| Shared fit operations | 2180-2293 | 114 | fit tests and first-length binary searches |
| Dense/prefix bound evaluation | 2295-2508, 3183-3339 | 484 | serial/parallel recurrence, worker launch, pruning |
| Projected bound evaluation | 2509-3182 | 674 | recursive and bottom-up projected evaluators |
| Ordinary DFS and scheduling | 3340-3658 | 319 | recursion, solution emission, task splitting, worker pool |
| Unoptimized fallback | 3659-3744 | 86 | decoded-class fallback currently described as unreachable |

The largest coherent extraction is projected bounds, at roughly 1,000 lines
once its action builder, configuration, and SIMD kernel are included. Exact
completability is the second cleanest boundary at roughly 600 lines including
its support scan.

## Problems visible in the current design

### `prepare_phase_two()` is an orchestration function and six builders

The function currently:

- resets all counters and mutable state;
- encodes the exact input bag;
- chooses a projected score-key layout;
- builds hot classes and projected actions;
- reports size preflight diagnostics;
- configures the length certificate;
- validates cache-fallback policy;
- allocates a score table;
- computes complete bounds;
- initializes the subsequent search.

This makes preparation policy hard to test or change independently. It also
means projected layout details occupy the main path even when dense bounds are
selected.

The eventual orchestration function should read more like:

```cpp
PreparationResult prepare_phase_two(DfsSearchOptions const& options) {
  DfsSearchData search_data = DfsSearchData::build(...);
  LengthCertificate certificate =
      LengthCertificate::build_if_requested(search_data, ...);
  ScoreBounds bounds = ScoreBounds::build(search_data, ...);
  return PreparationResult(
      std::move(search_data), std::move(certificate), std::move(bounds));
}
```

The real interfaces need failure reasons and statistics, described below, but
the top-level control flow should remain this short.

### Ordinary DFS and exact completion share an oversized worker

`SearchWorker` carries fields for:

- ordinary solution search;
- task production;
- score pruning;
- length-certificate counters;
- exact memoization;
- exact lookahead diagnostics.

Ordinary search does not need the exact fields, and exact validation does not
need solution paths, task production, certificate counters, or score-floor
pruning. Reusing worker initialization and progress reporting saved some code
but coupled two distinct algorithms.

A cleaner design has:

```cpp
class DfsSearchRunner {
 private:
  struct Worker;
};
class DfsCompletionRunner {
 private:
  struct Worker;
};
```

Both may contain a small `MutableLetterBagState`, but they should otherwise
contain only their algorithm's state and counters.

### Score-bound storage and score-bound evaluation are conflated

The current class owns:

- double atomic tables;
- float atomic tables;
- plain bottom-up float tables;
- unseen/computing sentinels;
- allocation policy;
- serial recurrence state borrowed from the main object's bag;
- parallel recurrence workers;
- projected action data;
- search-time lookup and lazy prefix construction.

This makes a bound lookup appear to be a property of the search object rather
than an operation on an explicit bound table. It also causes prefix-mode
pruning to copy a worker's bag into the object's shared scratch state before
calling the legacy serial builder.

The storage object should own representation and synchronization. Evaluators
should receive explicit state instead of borrowing `DfsAnagramSearch::bag`,
`bag_mask`, `current_score_key`, and `current_letters_left`.

### Statistics flatten every subsystem into the facade

The public class has many individual getters and corresponding scalar data
members. This obscures ownership and makes adding a counter a change to the
central class even when the counter belongs entirely to one subsystem.

Statistics naturally form four groups:

```cpp
struct ScoreBoundStats;
struct LengthCertificateStats;
class DfsSearchRunner {
 public:
  struct Stats;
};
class DfsCompletionRunner {
 public:
  struct Stats;
};
```

The facade can retain existing getters as compatibility wrappers while new
code reads grouped result structures.

### Anonymous-namespace helpers prevent a naive file split

The following helpers are used by multiple proposed translation units:

- `packed_rank()` and `packed_count()`;
- packed hot-class length/count accessors;
- float and double bit conversions;
- score-bound upward rounding;
- checked dense-table sizing;
- branch prediction annotations.

Simply copying these helpers into multiple `.cpp` files would create drift
risk. This is especially undesirable for the floating-point routines, whose
association and upward rounding are part of the admissible-bound contract.

The physical split needs a small private header containing only stable inline
primitives. It should not become a second monolithic header containing all
subsystem state.

## Stage 1: low-risk physical split

The first stage keeps `DfsAnagramSearch` and its private members intact.
Member-function definitions may live in separate translation units while
retaining private access, so no public API change is required.

### Proposed files

#### `dfs-search.cpp`

Keep only the facade and high-level orchestration:

- constructor;
- `prepare_phase_two()`;
- `run()`;
- common reset and final statistics aggregation;
- environment-independent selection between ordinary and exact entry points.

Initially `prepare_phase_two()` may remain large. Once the subsystem
functions have moved, its projected-layout selection can be extracted into a
helper without mixing that change into the file split.

Expected size after all mechanical extractions: roughly 600-850 lines.

#### `dfs-search-hot.cpp`

Own:

- `prepare_hot_classes()`;
- common hot fit operations;
- first-length binary search;
- common subtract/restore primitives if they can be expressed without
  algorithm-specific counters;
- `require_hot_classes()`;
- the unreachable decoded-class fallback, if it must remain.

This keeps the packed representation and the operations that interpret it
together. It also provides a natural eventual home for `HotClassIndex`.

Expected size: 300-450 lines.

#### `dfs-search-bounds.cpp`

Own the non-projected bound subsystem:

- score arithmetic support checks and rounding helpers;
- table allocation and mode selection;
- load, store, and parallel publish;
- cached reachability;
- serial dense/prefix recurrence;
- parallel dense recurrence and worker scheduling;
- search-time upper-bound lookup and pruning.

Expected size: 650-800 lines.

`should_prune()` belongs here during the mechanical split because it contains
the lazy prefix-bound bridge. In the component design, the sink-independent
upper-bound calculation moves into `ScoreBounds` and the final comparison can
live in the ordinary search runner.

#### `dfs-search-projected.cpp`

Own all projected-bound behavior:

- projected configuration parsing;
- `ProjectedAction` construction and quotienting;
- projected fit tests and length selection;
- recursive atomic projected evaluator;
- bottom-up layered projected evaluator;
- projected parallel setup;
- mandatory AVX2 wildcard kernel;
- projected diagnostics and counters;
- the projected wildcard test hook.

Expected size: 900-1,050 lines.

This is the best first extraction because nearly all of its helpers are
projected-specific and it has a clear correctness contract.

#### `dfs-search-completion.cpp`

Own:

- exact memo hash, lookup, and store;
- exact class subtract/restore;
- child classification;
- immediate and lookahead candidate loops;
- recursive exact remainder validation;
- batch worker scheduling;
- `find_completable_classes()`;
- exact-completion diagnostics and counters;
- scalar/AVX2 support-mask scans;
- `NUTRIMATIC_SUPPORT_SIMD`;
- `NUTRIMATIC_EXACT_MEMO_LOOKAHEAD`.

Expected size: 550-650 lines.

Moving the support SIMD kernel here makes its scope explicit: it accelerates
the exact-completion candidate scan, not ordinary DFS and not projected score
preprocessing.

#### `dfs-search-walk.cpp`

Own:

- `visit_fitting_class()`;
- `walk()`;
- normal worker initialization and aggregation;
- progress reporting;
- task generation;
- task worker scheduling;
- `NUTRIMATIC_SEARCH_TASKS`.

Expected size: 300-400 lines.

The unoptimized fallback could remain in `dfs-search-hot.cpp`, live here, or
be removed in a later behavior change. It should not get a dedicated file.

#### `dfs-search-certificate.cpp`

Own:

- length-certificate mode selection;
- table construction;
- bound test;
- grouped candidate traversal, initially still as a
  `DfsAnagramSearch` member;
- certificate counters if they are later grouped.

Expected size: 140-180 lines.

This file is optional. If the certificate is stable and unlikely to grow,
keeping it in `dfs-search-walk.cpp` is reasonable. It is a clean conceptual
boundary, but not necessary merely to reduce file size.

#### `dfs-search-internal.h`

During the mechanical split, contain only:

- packed-field inline accessors;
- bit-preserving float/double conversion helpers;
- upward-rounding helpers shared by bound implementations;
- small non-owning bag/class views if needed;
- compiler branch annotations.

Do not put owning vectors, worker structures, the full prepared problem, or
subsystem classes in this catch-all header. Those should move to narrow
component headers in stage 2.

### Build implications

All new `.cpp` files should remain in the existing `dfs-class-list` static
library. The project enables LTO, which permits cross-translation-unit
inlining, but hot paths still need measurement after movement.

Particular risks are:

- `projected_wild_update_scalar()` is deliberately inline and its kernel
  shape has measured performance consequences;
- `next_support_fit()` is on a scan that processes hundreds of billions of
  rejected candidates in the reference exact workload;
- hot packed-field accessors and fit checks should remain inline;
- moving `walk()` or its direct callees may change inlining decisions despite
  LTO.

The first commit for each extraction should contain only moves, include
changes, and Meson source-list changes. Renaming, API redesign, and
deduplication should follow separately.

## Stage 2: clean internal component API

The mechanical split improves navigation and ownership by convention, but
every translation unit still has access to every private member of
`DfsAnagramSearch`. A cleaner internal API makes dependencies explicit.

The intended dependency direction is:

```text
                         DfsAnagramSearch facade
                                  |
                                  v
                            DfsSearchData
                          /       |       \
                         v        v        v
                 HotClassIndex  ScoreKeyLayout  score model/data
                         \        /
                          v      v
                          ScoreBounds
                              |
                  +-----------+-----------+
                  |                       |
                  v                       v
          DfsSearchRunner          DfsCompletionRunner
                  |
                  v
          LengthCertificate
```

`DfsSearchRunner` and `DfsCompletionRunner` consume the same immutable search
data and bound-query interface. They do not call each other and do not share
worker types.

### `DfsSearchData`

This object owns the immutable, precomputed data for one DFS search: the input
letter bag, hot-class representation, score-key projection, and scoring data.
Projection choice belongs here because `score_key_deltas` and wildcard lengths
depend on it.

Suggested shape:

```cpp
struct DfsSearchOptions {
  bool score_bounds_requested;
  bool allow_cache_fallback;
  bool request_dense;
  int exact_letters;
  size_t score_cache_bytes;
  size_t preprocess_threads;
  int progress_factor;
};

struct LetterBagEncoding {
  std::array<uint32_t, DFS_SYMBOL_COUNT> counts;
  uint64_t support_mask;
  uint64_t exact_root_key;
  size_t letters;
};

struct ScoreKeyLayout {
  std::array<uint64_t, DFS_SYMBOL_COUNT> multipliers;
  uint64_t exact_mask;
  uint64_t state_count;
  uint64_t effective_states;
  uint64_t root_key;
  size_t exact_ranks;
  size_t wildcard_letters;
  size_t wildcard_span;
  bool projected;
};

class DfsSearchData {
 public:
  static PreparationResult build(
      DfsClassList const& classes,
      std::string const& letters,
      DfsScoreModel const& score_model,
      DfsSearchOptions const& options);

  DfsClassList const& source_classes() const;
  LetterBagEncoding const& root_letter_bag() const;
  ScoreKeyLayout const& score_keys() const;
  HotClassIndex const& hot_classes() const;
  double best_member_score(uint32_t class_id) const;
  double segment_boundary_score() const;
  size_t max_depth() const;
};
```

`PreparationResult` should contain either `DfsSearchData` or a precise
failure reason. The facade remains responsible for translating failure into
the current diagnostics and return value, so error wording need not change
during the refactor.

`DfsSearchData` is immutable after construction. That is important because
both ordinary-search and completion worker pools read it concurrently.

### `HotClassIndex`

This object owns the packed hot representation currently spread across:

- `fit_classes`;
- `class_supports`;
- `score_key_deltas`;
- `score_wild_lengths`;
- `packed_letters`.

Suggested hot-path API:

```cpp
using ClassId = uint32_t;

struct LetterBagView {
  uint32_t const* counts;
  uint64_t support_mask;
};

struct PackedRequirements {
  uint32_t const* data;
  uint32_t count;
  uint32_t repeated_count;
};

struct HotClassView {
  uint64_t support_mask;
  uint64_t score_key_delta;
  uint32_t letter_length;
  uint16_t wildcard_length;
  PackedRequirements requirements;
};

class HotClassIndex {
 public:
  static HotClassBuildResult build(
      DfsClassList const& classes,
      LetterBagEncoding const& letter_bag,
      ScoreKeyLayout const& score_keys);

  size_t size() const;
  HotClassView get(ClassId id) const;
  uint64_t const* contiguous_supports() const;

  bool fits(ClassId id, LetterBagView letter_bag) const;
  size_t first_length_candidate(
      size_t begin, size_t end, size_t letters_left) const;
};
```

`get()` and `fits()` must be inline views over the existing flat arrays, not
virtual calls and not allocations. A view eliminates the current overload set
for the root letter bag, `SearchWorker`, and `BoundWorker`: all three can
provide the same `LetterBagView`.

It may still be faster for the exact support scan to read
`contiguous_supports()` directly. The clean API should preserve that explicit
escape hatch rather than hide the scan behind a per-candidate abstraction.

Letter-bag subtraction and restoration can be handled in either of two ways:

1. inline methods on `HotClassIndex` operating on a small
   `MutableLetterBagState`;
2. algorithm-local loops over `HotClassView::requirements`.

The first avoids duplicating delicate mask maintenance. It should not use an
RAII guard in the hot recurrence unless measurement shows it compiles to the
same code; explicit subtract/restore makes mutation order and exceptional
behavior clearer.

### `ScoreBounds`

`ScoreBounds` owns representation, lookup, and construction. It should not
know about `DfsSolutionSink`; score-floor policy belongs to the search runner.

Suggested query API:

```cpp
enum class Reachability {
  unknown,
  impossible,
  reachable,
};

struct BoundStateView {
  LetterBagView letter_bag;
  uint64_t score_key;
  size_t letters_left;
  size_t wild_left;
};

struct ScoreBoundOptions {
  bool requested;
  bool allow_cache_fallback;
  size_t cache_bytes;
  size_t threads;
};

class ScoreBounds {
 public:
  static ScoreBoundBuildResult build(
      DfsSearchData const& search_data,
      ScoreBoundOptions const& options);

  ScoreBoundMode mode() const;
  bool complete() const;

  // Read-only and safe for concurrent completion workers.
  Reachability reachability(uint64_t score_key, bool root) const;

  // Returns false when no upper bound is available.
  bool upper_bound(
      BoundStateView state, bool root, double* value);

  ScoreBoundStats const& stats() const;
};
```

This separates two meanings currently combined in
`cached_reachability()`:

- completion search asks whether the table proves impossible or, for an
  exact table, reachable;
- score pruning asks for a numeric upper bound.

For projected tables with wildcard letters, a finite value is an upper bound
but does not prove exact reachability. Keeping `reachability()` separate from
`upper_bound()` makes that distinction part of the API instead of a caller
convention.

Suggested grouped statistics:

```cpp
struct ScoreBoundStats {
  ScoreBoundMode mode;
  size_t entries;
  size_t states_computed;
  size_t capacity;
  size_t value_bytes;
  size_t bytes_charged;
  size_t exact_letters;
  size_t wildcard_letters;
  size_t projected_actions;
  bool projected_quotient;
  bool complete;
  uint64_t candidate_tests;
  uint64_t fitting_transitions;
  uint64_t successful_transitions;
  uint64_t nextafter_calls;
};
```

Search-time prune count does not belong here; it is an execution statistic,
because it counts how the DFS consumed the bounds rather than how the bound
table was built.

### Projected-bound internals

Projected actions should not be exposed through the general bound interface.
They are an implementation detail of constructing a projected table.

Suggested narrow internal types:

```cpp
class ProjectedActionTable {
 public:
  static ProjectedActionBuildResult build(
      DfsSearchData const& search_data);

  ProjectedAction const& action(size_t index) const;
  uint64_t support(size_t index) const;
  CandidateRange bucket(size_t rank) const;
  size_t size() const;
  bool quotient_enabled() const;
};

struct ProjectedComputeOptions {
  size_t threads;
  ProjectedKernelMode kernel;
  bool bottom_up;
};

ProjectedComputeResult compute_projected_bounds(
    DfsSearchData const& search_data,
    ProjectedActionTable const& actions,
    ProjectedComputeOptions const& options,
    ScoreBoundStorage* destination);
```

The mandatory AVX2 kernel can remain a free function in
`dfs-search-projected.cpp`.

After a complete projected table has been constructed, the full action table
is no longer required during ordinary search. A later memory cleanup could
release it and retain only `projected_actions` and quotient status in
`ScoreBoundStats`. That is a behavior-neutral memory improvement, but it
should not be mixed into the file split.

The projected wildcard test hook currently appears on the public
`DfsAnagramSearch` class. Longer term it can move to a private
`dfs-search-test-hooks.h` interface linked only by the test executable. That
keeps kernel testing direct without making a low-level span update part of
the production facade.

### `LengthCertificate`

The certificate is immutable after preparation. Counters belong to workers,
not to the certificate itself.

Suggested API:

```cpp
class LengthCertificate {
 public:
  static LengthCertificateBuildResult build(
      DfsSearchData const& search_data,
      LengthCertificateMode mode);

  bool enabled() const;
  bool shadow() const;
  size_t bytes() const;

  size_t group_end(size_t rank, size_t length) const;
  bool rejects(
      size_t rank,
      size_t length,
      size_t letters_left,
      double accumulated_score,
      double floor) const;
};
```

The ordinary search runner remains responsible for iterating a group,
incrementing its worker-local counters, honoring shadow mode, and visiting
fitting classes. This avoids making the certificate depend on the sink or on
callbacks into the DFS.

### `DfsSearchRunner`

This component owns ordinary enumeration and parallel task scheduling.

Suggested API:

```cpp
class DfsSearchRunner {
 public:
  struct Options {
    size_t threads;
    size_t task_target;
    size_t task_progress_factor;
    int64_t progress_interval;
    bool verbose;
  };

  struct Stats {
    int64_t nodes;
    int64_t solutions;
    int64_t bound_prunes;
    size_t threads_used;
    uint64_t tasks_created;
    double elapsed_seconds;
    LengthCertificateStats certificate;
  };

  struct Results {
    Stats stats;
  };

  static Results run(
      DfsSearchData const& search_data,
      ScoreBounds* bounds,
      LengthCertificate const* certificate,
      Options const& options,
      DfsSolutionSink* sink);

 private:
  struct Worker;
};
```

`Results` initially contains only `Stats`. Keeping the return envelope distinct
from the retained statistics leaves room for a later execution outcome without
putting transient result state into `DfsSearchStats`.

The private `DfsSearchRunner::Worker` then needs only:

- mutable bag counts and mask;
- score key;
- solution path;
- node, solution, and prune counters;
- certificate counters;
- task-production fields;
- progress fields.

It no longer carries exact keys, exact memo counters, or lookahead counters.

### `DfsCompletionRunner`

This component owns the separate exact boolean search used by
`query-index --require-completable`.

Suggested API:

```cpp
class DfsCompletionRunner {
 public:
  struct Options {
    size_t threads;
    size_t memo_lookahead;
    int64_t progress_interval;
  };

  struct Stats {
    size_t classes_checked;
    size_t bound_rejects;
    size_t exact_bound_accepts;
    size_t exact_validations;
    size_t memo_states;
    size_t memo_hits;
    uint64_t lookahead_full_windows;
    uint64_t lookahead_known_true_wins;
    uint64_t lookahead_reprobes_decided;
    uint64_t lookahead_recursive_expansions;
    int64_t nodes;
    size_t threads_used;
    double elapsed_seconds;
  };

  struct Results {
    std::vector<bool> completable;
    Stats stats;
  };

  static Results run(
      DfsSearchData const& search_data,
      ScoreBounds const& bounds,
      Options const& options);

 private:
  struct Worker;
};
```

Its private `DfsCompletionRunner::Worker` needs:

- mutable bag counts and mask;
- projected score key when bounds are active;
- exact mixed-radix key;
- exact memo and lookahead counters;
- node and progress counters.

It does not need a solution path, sink, length certificate, task splitting,
or representative score.

The exact memo table should be owned by this component rather than retained
on `DfsAnagramSearch` after validation. Its memory can be released when
`run()` returns; only `DfsCompletionRunner::Stats` needs to survive for
diagnostics and getters.

### Facade and public API

The public behavior can remain source-compatible:

```cpp
class DfsAnagramSearch {
 public:
  DfsAnagramSearch(...);
  ~DfsAnagramSearch();

  bool run(...);
  bool find_completable_classes(...);

  DfsSearchStats const& stats() const;

  // Existing getters remain as wrappers during migration.

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
```

A Pimpl is appropriate only after the internal components exist. It would
shrink `dfs-search.h` substantially and stop every consumer from parsing
atomic table types, worker records, projected actions, and aligned storage.
Public calls are coarse-grained, so the indirection is not on a hot recursive
path.

The class is already effectively non-copyable because of atomic members.
The Pimpl conversion should explicitly delete copy construction and
assignment and add an out-of-line destructor. It should not silently add move
semantics as part of the refactor.

Suggested top-level grouped result:

```cpp
struct DfsSearchStats {
  ScoreBoundStats score_bounds;
  DfsSearchRunner::Stats search;
  DfsCompletionRunner::Stats completion;
  double setup_seconds;
  size_t preprocess_threads_used;
};
```

Existing getters in `dfs-anagrams`, `query-index`, and tests can delegate to
this structure until a separate API cleanup is desired.

## Ownership and lifetime

The intended ownership rules are:

- `DfsAnagramSearch` retains the constructor inputs and the most recent
  grouped statistics.
- `DfsSearchData` owns all query-specific immutable hot data for one call.
- `ScoreBounds` owns its table and construction statistics.
- `ProjectedActionTable` is temporary construction state unless the recursive
  atomic evaluator still needs it while filling the table.
- `LengthCertificate` is immutable and lives for one ordinary search.
- `DfsSearchRunner` owns normal workers and tasks for the duration of `run()`.
- `DfsCompletionRunner` owns its exact memo and workers for the duration
  of `find_completable_classes()`.
- No worker points at mutable state owned by another worker.
- The only deliberately shared mutable structures are atomic bound tables,
  the exact memo during exact validation, the task/class cursors, and
  aggregate progress counters.

This makes it possible to reason about memory reclamation. Today many
algorithm-specific vectors and counters remain members because all state has
the lifetime of the facade.

## Error and diagnostic API

The current implementation mixes:

- boolean build failure;
- `unsupported_reason`;
- direct `fprintf`;
- timestamped `dfs_diagnostic`;
- fallback policy.

The refactor should preserve current output initially, but component builds
need structured failure internally:

```cpp
enum class PreparationFailure {
  none,
  state_key_overflow,
  too_many_classes,
  packed_requirements_overflow,
  allocation_failed,
  score_table_too_large,
  score_cache_too_small,
  arithmetic_unsupported,
  kernel_verification_failed,
};

struct BuildFailure {
  PreparationFailure reason;
  size_t required_bytes;
  char const* detail;
};
```

The facade translates this to existing diagnostics. Components may still
emit progress diagnostics during long computations, but policy errors should
not be formatted independently in several files.

Environment variables should be resolved into typed options before entering
worker code:

| Variable | Owning component |
|---|---|
| `NUTRIMATIC_SUPPORT_SIMD` | exact-completion support scan |
| `NUTRIMATIC_EXACT_MEMO_LOOKAHEAD` | exact-completion search |
| `NUTRIMATIC_LENGTH_CERTIFICATE` | length certificate |
| `NUTRIMATIC_SEARCH_TASKS` | ordinary search runner |

This table clarifies that the support-scan SIMD switch has its own owner.

## Dependency rules

To prevent the new files from recreating the monolith through includes:

1. `DfsSearchData` may depend on `DfsClassList`, `DfsScoreModel`, and
   `HotClassIndex`.
2. `ScoreBounds` may depend on `DfsSearchData`; `DfsSearchData` must not
   depend on `ScoreBounds`.
3. Projected-bound internals may depend on the bound-storage interface, but
   generic bound code must not depend on projected worker types.
4. The two search runners may depend on `DfsSearchData` and the narrow
   `ScoreBounds` query API.
5. `ScoreBounds` must not depend on `DfsSolutionSink`.
6. `LengthCertificate` must not depend on a search worker or sink.
7. Ordinary-search and completion worker types must not be shared.
8. SIMD kernels operate on flat spans and primitive values, not on owning
   search objects.
9. Diagnostics and statistics cross component boundaries as value types, not
   as access to the facade's scalar members.

## Suggested implementation sequence

### 1. Establish baselines

Record:

- current `dfs-search` and `dfs-cli` smoke-test results;
- deterministic DFS and bound counters for scalar and default SIMD modes;
- one exact-completion workload;
- one projected bottom-up workload;
- binary size and compilation time if build-time improvement is a goal.

Accurate timing runs must first check the host process table for concurrent
`query-index` and `dfs-anagrams` instances, per repository instructions.

### 2. Extract projected bounds mechanically

Move projected-only free functions and member definitions, update Meson, and
make no API or algorithm changes. This removes the largest coherent block and
forces the shared-helper boundary to become explicit.

### 3. Extract exact completion mechanically

Move the exact memo, recurrence, batch scheduling, and
related environment parsing.

### 4. Extract generic bounds

Move allocation, storage, dense/prefix recurrence, and pruning. Keep
`prepare_phase_two()` as the facade until this move is stable.

Verify:

- float prefix mode only
- serial and parallel preprocessing;
- exact upward-rounded results and `nextafter` counts;

### 5. Extract ordinary traversal and certificate

Move normal DFS and task scheduling. Split the certificate only if the
resulting walk file remains large or certificate development is active.

Verify:

- serial counter-only search;
- serial and parallel sinks;
- stop behavior;
- task generation and deterministic retained spellings.

### 6. Introduce value types and grouped statistics

Add `LetterBagView`, `BoundStateView`, and grouped statistics while retaining
the existing public getters. Convert one subsystem at a time.

### 7. Separate ordinary-search and completion workers

This is the first meaningful state redesign. Do it after physical ownership
is clear so changes stay local to the two search files.

### 8. Introduce `DfsSearchData` and component ownership

Replace direct access to facade fields with explicit immutable inputs.
Convert bounds before the runners so both runners can consume the same narrow
query interface.

### 9. Add Pimpl last

Once private types no longer need to be declared on `DfsAnagramSearch`, move
them out of `dfs-search.h` and hide the implementation. This should be a
mostly mechanical public-header cleanup, not the step that invents component
boundaries.

## Testing and performance guardrails

The repository prioritizes implementation over broad test expansion, so the
refactor should use the existing smoke tests and a small number of targeted
differentials rather than add extensive new suites.

Important invariants are:

- identical retained spellings and scores;
- identical DFS node and solution counts;
- identical bound states, transitions, and `nextafter` counts;
- identical exact memo states and hits where scheduling does not make them
  intentionally nondeterministic;
- unchanged candidate order in exact lookahead;
- bit-identical scalar and AVX2 projected wildcard outputs;
- no AVX2 execution on unsupported hardware;
- unchanged cache-budget accounting and fallback behavior;
- unchanged diagnostics except where a later, explicit diagnostics change is
  intended.

Performance-sensitive differentials should cover:

- default versus `NUTRIMATIC_SUPPORT_SIMD=0`;
- projected bottom-up construction;
- one single-thread and one multi-thread preparation;
- one `--require-completable` exact-validation run;
- one ordinary phase-two search with a score-aware sink.

Because LTO is enabled, a source split should usually preserve inlining, but
that is not a guarantee. Any repeatable regression in the projected wildcard
kernel, support-mask scan, fit checks, or `walk()` should be treated as a
failed mechanical split until the generated call boundary is understood.

## Recommended stopping points

There are three useful stopping points, depending on the objective.

### Navigation-only improvement

Stop after the stage-1 physical split. This reduces file size and gives
subsystems recognizable homes with minimal semantic risk. The large private
class and header remain.

### Maintainable internal architecture

Continue through separate worker types, `DfsSearchData`, `HotClassIndex`,
`ScoreBounds`, and the two runner APIs. This removes most cross-subsystem
private-state access and is the best long-term balance.

### Small public header and explicit ABI

Add the Pimpl and grouped public statistics after componentization. This is
useful if build isolation or header readability matters, but it is not
required merely to make implementation work tractable.

## Recommendation

Start with `dfs-search-projected.cpp`, followed by
`dfs-search-completion.cpp`. They are the two largest clean boundaries and
each has its own entry behavior, configuration knobs, counters, and SIMD
implementation.

Then extract generic bound storage/evaluation. At that point
`dfs-search.cpp` should mostly describe the phase-two lifecycle, and the
remaining coupling will be visible enough to decide whether the second-stage
component APIs justify their cost.

Do not begin with the Pimpl, worker redesign, or statistics cleanup. Those are
valuable only after the algorithms have physical owners; doing them first
would turn a tractable source split into a large state-and-API rewrite.
