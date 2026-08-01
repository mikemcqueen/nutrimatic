# Refactoring the DFS search refactor

`findings/dfs-search-refactor.md` describes a target shape for phase 2. Part of
it landed. This records what the partial landing left behind, because the
intermediate state is worse to read than what preceded it, and the reasons are
specific rather than a matter of taste.

Two problems. Both are now fixed; each section records the state that
prompted it, then the resolution. They turned out not to be independent — see
the note on the positional aggregate init in problem 2.

## 1. `DfsAnagramSearch` re-exports its members' members, field by field

**Fixed.** Recorded below as it stood, because the shape is worth recognising
again. Resolution at the end of the section.

`dfs-search.h` declares 35 public one-line accessors on `DfsAnagramSearch`. They
are not a curated interface. They are a mechanical field-by-field re-export of
structs that already have accessors of their own:

| contained struct | fields | facade accessors |
| --- | --- | --- |
| `ScoreBounds::Stats` | 6 | 6 (`score_bound_mode`, `_entries`, `_capacity`, `_value_bytes`, `_bytes_charged`, `_complete`) |
| `ScoreBounds::ProjectedStats` | 5 | 5 (`score_bound_states_computed`, `_transitions`, `_candidate_tests`, `_fitting_transitions`, `_nextafter_calls`) |
| `CertificateStats` | 4 | 4 (`length_certificate_group_tests`, `_group_rejects`, `_scans_skipped`, `_scans_kept`) |
| `RunStats` | 5 | 5 (`phase_two_setup_seconds`, `_search_seconds`, `preprocess_threads_used`, `search_threads_used`, `search_tasks_generated`) |

One accessor per field, in declaration order. Adding a field to `ProjectedStats`
currently implies adding a forwarding method to the facade's public interface,
which is not a relationship that should exist between a statistics struct and
the class that owns the thing that owns it.

`ScoreBounds::stats()` is **already public** (`dfs-search.h:80`). The eleven
bound accessors exist only because the `score_bounds` member is private. A
single `ScoreBounds const& score_bounds() const` replaces all eleven and the
call sites read `search.score_bounds().stats().entries`.

`DfsAnagramSearch::stats()` is likewise already public and already returns the
whole `DfsSearchStats`. Every accessor that forwards to `search_stats.…` is
therefore redundant on arrival — `nodes_visited()`, `solutions_found()`,
`score_bound_prunes()`, the four certificate counters, the five `RunStats`
ones, and the `completable_*` / `exact_memo_*` group.

Six have no callers at all, anywhere in the tree, including tests:

    completable_classes_checked        completable_bound_rejects
    completable_exact_bound_accepts    completable_exact_validations
    exact_memo_states_computed         exact_memo_hits

Of the remaining 29, most are called only from `source/test-dfs-search.cpp`.
Only `nodes_visited`, `solutions_found`, `score_bound_mode`, and
`search_threads_used` have more than a couple of non-test callers.

Three accessors are not pure forwarding and need somewhere to live rather than
simple deletion:

- `length_certificate_enabled()` / `length_certificate_skipping()` read facade
  configuration state (`length_certificate_ready`, `_shadow`), not statistics.
- `length_certificate_table_bytes()` computes a size across three facade
  vectors.
- `score_bound_exact_letters()`, `_wild_letters()`, `_projected_actions()`
  describe the projection's shape and belong with the bound table, not with the
  facade. They are natural fields of `ScoreBounds::Stats`.

### Resolution

All 35 accessors are gone. The first step replaced them with three methods
(`stats()`, `bounds()`, `certificate()`); problem 2 then removed those three as
well, because statistics became the *result* of a call rather than state on the
search object. See "Stats became the result of a call" below.

- The six dead accessors were deleted outright.
- Everything forwarding to `search_stats.…` is reached through `stats()`.
- The eleven bound accessors are reached through `bounds().stats()`, which was
  already public on `ScoreBounds`.
- `exact_letters`, `wild_letters`, and `projected_actions` moved to the bound
  statistics, assigned once in `prepare_phase_two()` after the last
  `ScoreBounds::clear()` — `clear()` resets the whole stats block, so an
  earlier assignment would be silently wiped when bound computation fails and
  falls back.
- The three non-forwarding certificate accessors became `CertificateInfo`
  (`ready`, `shadow`, `table_bytes`, and a `skipping()` derived from the first
  two), returned by value.

Call sites updated: `dfs-anagrams.cpp`, `query-index.cpp`, `test-dfs-search.cpp`,
`test-dfs-output.cpp`. Where a block compared the same field across two search
objects, a local `Stats const&` alias replaced the repeated chain rather than
inlining `a.bounds().stats().x == b.bounds().stats().x` at every line.

`measure-f.cpp` and `dfs-class-list.cpp` also call `nodes_visited()` and
`solutions_found()`, but on `CollapseDFS` and the class extractor — different
classes with their own accessors, untouched.

Full suite passes (7/7, none skipped, with `$IDX` set).

## 2. `DfsSearchData` borrows where it was specified to own

**Fixed.** Recorded as it stood; resolution at the end of the section.

`findings/dfs-search-refactor.md:88-93` specifies `DfsSearchData` as a component
that **owns** its data, produced by a factory:

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

None of that exists. `PreparationResult`, `LengthCertificate`,
`DfsSearchOptions`, `build()`, and `build_if_requested()` have zero occurrences
in `source/`. `DfsSearchData` itself is not in HEAD either — it is entirely
uncommitted working-tree change. HEAD's `dfs-search.h:417` carries only a
placeholder: *"Prepared query data and bound lookup remain on the facade until
the DfsSearchData extraction."*

That comment is also the goal the step actually pursued, and it is a narrower
goal than the plan's. "Runners stop holding `DfsAnagramSearch&`" was achieved by
the cheapest route available — a struct of references and by-value copies
pointed at facade members that never moved. Because the facade's members were
never the target, none was deleted. `DfsAnagramSearch` still carries all 43, and
`DfsSearchData` restates a subset of them.

The by-value storage in the runners is separately deliberate and measured
(`findings/dfs-search-refactor.md:695-704`: a reference member cost ~2% on the
42-letter single-threaded `--require-completable` benchmark). That is a fact
about how runners hold the struct, not a reason for the struct to borrow.

Two consequences:

**The same name means two different things with nothing to tell them apart.**
`length_certificate_ready` and `length_certificate_shadow` name a facade member
(the source of truth, written by `prepare_length_certificate()`) *and* a
`DfsSearchData` member (a by-value copy). Reading `dfs-all-runner.cpp:162` or
`:190` there is no way to know which one is in scope or which is authoritative.
The tables themselves — `certificate_max_score`, `certificate_group_end`,
`length_tail_bounds` — are `const&` in the view and genuinely not duplicated,
but a reader cannot tell that from the declarations either, because the
reference members and the value members are formatted identically in the same
block.

**`search_data()` is a 22-field positional aggregate init.**
`dfs-search.cpp:135-163` initialises `DfsSearchData` by position with no
designators. The project sets `cpp_std=c++17` in `source/meson.build:5`, and
designated initialisers are C++20 — available as a compiler extension, but not
something to rely on here. Adjacent same-typed fields make this silently wrong
under reordering:

- `length_certificate_ready` and `length_certificate_shadow` — two adjacent
  `bool`s.
- `bag_mask`, `score_key`, `exact_root_key` — three adjacent `uint64_t`s.
- `max_depth`, `score_wild_letters`, `certificate_stride`,
  `requested_search_threads` — four `size_t`s in the same list.

Reordering the declarations in `dfs-search.h:423-465` compiles cleanly and
misassigns. This is a latent defect, not a style preference.

It is also not an independent defect, which an earlier draft of this document
got wrong. Reference members cannot be default-initialised, so an aggregate
holding them *must* be brace-initialised positionally, and with no constructor
there is nowhere to name a field. The 22-field positional list exists **because**
the struct borrows. Give `DfsSearchData` its data and it acquires a `build()`,
and the hazard stops existing rather than being papered over with a comment.

## Why `build()` was not written

Three things are in the way. Only the first is easy, and it is the one that
looks like the blocker.

**Returning data plus an error is not the problem.** `prepare_phase_two()`
returns `bool`, and every failure path prints a diagnostic and returns false.
`std::optional<DfsSearchData>`, an out-parameter, or a small result struct all
cover it.

**The sketch is circular as written.** `ScoreBounds::build(search_data, ...)`
consumes `search_data`, but `DfsSearchData` holds `ScoreBounds const&
score_bounds` — read at `dfs-all-runner.cpp:41-42` — and the derived
`score_bounds_active` flag. So search_data needs bounds and bounds needs
search_data. `PreparationResult` is exactly the resolution: the runners receive
data, certificate, and bounds as three separate members, and the bound reference
is not a field of the thing bounds are built from. The working tree instead
collapsed all three roles into one struct, which makes `build()` unwritable in
that shape. Splitting the bound reference back out is a precondition, not a
detail.

**`ScoreBounds::build()` has nothing to build from yet.** The projected
evaluators (`compute_projected_score_bounds_top_down()` and `_bottom_up()`) and
the projected-action table they walk are still `DfsAnagramSearch` members
reading facade state. `findings/dfs-search-refactor.md` names this as the
`HotClassIndex` step; it has not happened either.

## What keeps the data on the facade

One further constraint, easy to miss: `dfs-anagrams.cpp` reports bound and
certificate statistics *after* `run()` has returned. If the preparation products
were locals inside `run()`, they would be destroyed before the reporting code
reads them. Any fix has to answer this — either the facade holds the
`PreparationResult`, or `run()` hands it back to the caller.

## Target shape

`DfsAnagramSearch` should be an orchestration layer holding almost nothing: the
borrowed class list, the constructor's query parameters, and the accumulated
statistics. The plan's own decomposition is the target, and it is reached in
this order:

1. **Split the bound reference out of `DfsSearchData`.** Runners take data and
   bounds separately. This breaks the circularity and is a prerequisite for
   anything else.
2. **Extract `HotClassIndex`** — the hot arrays, projected actions, and the
   projected evaluators — off the facade, so `ScoreBounds::build()` has an
   input.
3. **Give each component a `build()`** that owns what it produces, deleting the
   corresponding facade members as each one moves. The positional aggregate init
   disappears with the first one.
4. **Introduce `PreparationResult`** holding the three components, stored by the
   facade so post-`run()` reporting still works.

Step 3 is where the 43 members actually go away. Steps 1 and 2 produce no
visible simplification on their own, which is likely why the previous step
stopped short of them.

## Resolution of problem 2

`DfsSearchData` now owns its data. Every member is a value, a vector, or a
`unique_ptr`; the only borrowed thing is `class_list`, whose owner outlives
every phase-2 call. Preparation writes directly into the `DfsSearchData` it is
building, so the facade no longer declares any of it.

The runner takes the data **by move**, which was the option the borrowing design
could not express. That removes both copies (the local in `run()` and the
runner's member) while keeping fields at a fixed offset from the runner's
`this` — the property the by-value copy had been bought for. The 22-field
positional aggregate init is gone with the reference members that forced it.

### Stats became the result of a call

The reason the facade still held `ScoreBounds` and the certificate tables was
post-run reporting: every use of the old `bounds()` accessor was
`.bounds().stats()`, and nothing ever touched the table itself. So statistics
moved out of the search object entirely:

```cpp
struct Stats {
  Bounds       bounds;         // snapshot; the table moves into the runner
  Certificate  certificate;    // ready/shadow/table_bytes + Counters
  AllSolutions all_solutions;
  AnySolution  any_solution;   // + Memo, Lookahead
  Execution    execution;
};

bool run(DfsSolutionSink*, Stats*, ...);
bool find_completable_classes(std::vector<bool>*, Stats*, ...);
```

Two mis-groupings were fixed on the way: the certificate counters used to live
inside `AllSolutionsStats`, away from the certificate's own configuration, and
the bound statistics were split between `ScoreBounds::Stats` and
`ProjectedStats`. Each component now reports one struct.

With statistics returned rather than retained, nothing needs to outlive the
runner, which is what let the bound table and the certificate tables move into
`DfsSearchData`.

### Result

- Facade: 43 data members to 23, of which 10 are the query itself.
- Public interface: `run()` and `find_completable_classes()`.
  `stats()`, `bounds()`, `certificate()`, and the projected wildcard test hook
  are all gone.
- `git diff --stat`: 1064 insertions, 1438 deletions across 11 files.
- Full suite passes (7/7, none skipped, with `$IDX` set), and `dfs-anagrams -v`
  diagnostics are byte-identical to before the change.

### Statistics moved to their own header

`Stats` was still declared inside `DfsAnagramSearch`, which meant the phase-2
counters were declared in the same file as the class that produces them and
could only be named through it. They now live in `source/dfs-search-stats.h` as
a file-scope `DfsSearchStats`, with `Bounds`, `Certificate`, `AllSolutions`,
`AnySolution`, and `Execution` nested inside it. `dfs-search.h` includes that
header; nothing else about the ownership story changed.

The point of the move is the invariant it makes checkable: **every phase-2
counter is declared exactly once, in that file.** Three sets of duplicates were
removed to get there.

- `ScoreBounds::Stats` and `ScoreBounds::ProjectedStats` restated the same 14
  fields as `Stats::Bounds`, which `prepare_phase_two()` then copied across
  field by field, flattening `projected` on the way. `ScoreBounds` now holds a
  `DfsSearchStats::Bounds` directly and the copy is one assignment. Callers read
  `stats.bounds.projected.transitions` rather than `stats.bounds.transitions`.
- `ScoreBounds::Stats` carried `exact_letters`, `wild_letters`, and
  `projected_actions`, which nothing inside `ScoreBounds` ever wrote — the
  search filled its own copy. There is now one set of fields, still assigned
  after the last `clear()` for the reason recorded above.
- Both runners returned a `Results` struct whose `search_threads` and
  `search_tasks` were a second declaration of the `Execution` fields the facade
  copied them into. `Results` is gone: `DfsAllSolutionsRunner::run()` and
  `DfsAnySolutionRunner::run()` take a `DfsSearchStats*` and accumulate into it,
  so a worker's running total and the reported result are the same declaration.

`ScoreBounds::Mode` moved with them, as a file-scope `DfsScoreBoundMode`
(`DFS_SCORE_BOUND_OFF`, `DFS_SCORE_BOUND_PROJECTED`): the statistics record the
mode and outlive the table, so the enum cannot stay on the table's class without
`dfs-search-stats.h` depending on `dfs-search.h`.

Full suite passes (7/7, none skipped, with `$IDX` set), and `dfs-anagrams -v`
reports the same diagnostics with the same counter values.

### The runners left the facade class

`DfsAllSolutionsRunner` and `DfsAnySolutionRunner` were nested inside
`DfsAnagramSearch`, which read as a coupling that was not there. Checked
against the code: **neither runner body touches a single facade member.** Every
access goes through `data.*`, the `DfsSearchData` each one takes by move. The
one apparent counterexample was `find_completable_classes()`, which merely
happened to be defined in `dfs-any-runner.cpp`; it is facade code and now lives
in `dfs-search.cpp` beside `run()`.

What the runners needed from the enclosing scope was four private nested
declarations, none of which is facade state: `DfsSearchData`, `FitClass` /
`FitClassMetadata`, `Reachability`, and `MAX_SPLIT_DEPTH` — the last used only
by the exhaustive runner, so it moved into that class.

`DfsSearchData` holds a `ScoreBounds` **by value**, so moving it to file scope
required `ScoreBounds` to be namable without `dfs-search.h`. That is the only
hard edge in the split; everything else would have worked with forward
declarations. `ScoreBounds` keeps its `friend class DfsAnagramSearch`, which
needs only a forward declaration, so the bound header does not depend on the
facade.

The resulting headers, each self-contained (verified by compiling each one
alone), with arrows pointing from includer to included:

    dfs-search-stats.h    (no project includes)
    dfs-solution-sink.h   (no project includes)
    dfs-score-bounds.h  → dfs-alloc.h, dfs-class-list.h, dfs-search-stats.h
    dfs-search-data.h   → dfs-alloc.h, dfs-class-list.h, dfs-score-bounds.h,
                          dfs-score.h
    dfs-all-runner.h    → dfs-class-list.h, dfs-search-data.h,
                          dfs-search-stats.h, dfs-solution-sink.h
    dfs-any-runner.h    → dfs-alloc.h, dfs-class-list.h, dfs-search-data.h,
                          dfs-search-stats.h
    dfs-search.h        → the six above except the two runner headers

Two edges are the point of the split rather than a side effect of it:

- **`dfs-search.h` does not include either runner header.** Both runners are
  only ever instantiated as locals inside `run()` and
  `find_completable_classes()`, so the runner headers are included by `.cpp`
  files alone. `Worker`, `SearchTask`, `Memo`, `ResultSource`, and
  `ChildResult` are out of the facade header entirely.
- **`dfs-output.h` includes `dfs-solution-sink.h`, not `dfs-search.h`.** It
  used one name (`DfsSolutionSink`, for `DfsTopN`'s base) and was dragging in
  the whole search facade, and through it every `.cpp` that includes
  `dfs-output.h`.

`dfs-search.h` is 590 lines lighter and now declares `DfsAnagramSearch` and
`ProjectedAction`, nothing else.

Not addressed, and worth naming: `DfsSearchData`'s three member functions are
still defined in `dfs-search.cpp` (`first_length_candidate`,
`certificate_rejects`) and `dfs-search-projected.cpp` (`cached_reachability`),
because a `dfs-search-data.cpp` would be a new `meson.build` entry for three
functions. The declarations moved; those definitions did not.

Full suite passes (7/7, none skipped, with `$IDX` set). `dfs-anagrams -v` on 14
letters reports byte-identical diagnostics to before the split, and
`query-index --require-completable` still reports the full completability,
memo, and lookahead statistics through the relocated facade method.

### What is left on the facade

The remaining 13 non-query members are the projection layout
(`score_multipliers`, `score_exact_mask`, `score_exact_letters`,
`score_wild_span`, `score_wild_lengths`) and the projected-action table
(`projected_actions`, `projected_action_support`,
`projected_repeated_requirements`, `projected_bucket_starts`,
`projected_actions_ready`), plus `hot_classes_ready`, `empty_class_list`, and
`unsupported_reason`. These are inputs the score-bound builder consumes, not
results a call returns. Moving them is the `HotClassIndex` extraction, which is
also what `ScoreBounds::build()` is waiting on.
