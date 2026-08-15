# Plan: add `--solo-words` candidate-pair scoring

## Status

This is the canonical plan. It supersedes `plans/solo-words-claude.md` and
`plans/solo-words-codex.md`; those files remain only as design history.

Status values are `[ ]` pending, `[-]` in progress, and `[x]` complete.

| Phase | Deliverable | Status | Proposed commit |
|---:|---|:---:|---|
| 1 | Shared pair-profile and assignment helper | [x] | `Add solo-word pair matching` |
| 2 | Shared CLI and complete score integration | [x] | `Score words with solo-word partners` |

## Outcome

Add the shared option:

```text
--solo-words WORD[,WORD...]
```

to `dfs-anagrams` and `query-index`. A supplied solo word is external context:
it consumes no letters, is not an output segment, and is not printed. A
selected single-word entry can instead use it as a two-word partner and earn
the existing word and pair bonuses.

The following requirements are settled:

- A partner edge exists when either ordered pair is present in the aggregate
  index trie **or** the pair is asserted by `--pairs`.
- An index-backed edge earns `--word-bonus`. An edge asserted by `--pairs`
  earns `--word-bonus` and `--pair-bonus`, whether or not the index contains
  that pair. Thus a pair edge is always also a word edge.
- Only single-word selected entries participate. Existing multi-word entry
  bonuses are unchanged and never consume a solo word.
- Each of at most 16 supplied solo words can be used once in a complete DFS
  spelling or one `query-index --score` sequence.
- Solo words do not affect extraction eligibility, dictionaries, minimum word
  length, `--max-extract-words`, or output text.

No option means no changed score, output, search bound, packed-record size, or
profile work.

## Policy decisions

These resolve disagreements between the two earlier plans. Confirmed choices
are marked resolved and are implementation requirements.

1. **Resolved: reject negative bonuses when solo words are active.** Existing
   negative `--word-bonus` and `--pair-bonus` values remain legal without
   `--solo-words`. With solo words, require both to be non-negative. This
   keeps an assignment a reward, makes the per-member optimistic score
   natural, and avoids the surprising maximum-cardinality rule in the Codex
   draft, under which a zero-value edge could displace a positive pair edge or
   a negative edge could be forced into the score.

2. **Resolved for now: reject duplicate solo words.** Reject a duplicate even
   when it arrives through a repeated option. This treats the list as unique
   partners, not a capacity-encoding multiset, and prevents
   `--solo-words sea,sea` from silently bypassing the once-per-answer rule.
   Repeated option occurrences otherwise append, up to the shared limit of
   16. Extra capacity can be designed explicitly later if a real use case
   appears.

3. **Resolved: parse direct CLI values strictly.** Accept only nonempty
   lowercase `a-z0-9` fields; reject uppercase, punctuation, spaces, and empty
   fields. `--pairs` file cleanup is intentionally lossy because it ingests a
   file; silently rewriting a direct score option is less predictable.

4. **Resolved: use aggregate phrase presence, and name it accurately.** The
   index probe consumes the pair's trailing space and accepts the aggregate
   node used by phase 1 and `query-index --score`. It does not require a
   positive exact residual from `exact_entry_count()`. Help and comments must
   say "aggregate index phrase" rather than "complete entry" so a phrase
   prefix supported only by longer entries is not misrepresented.

## Scoring contract

Let:

```text
W = --word-bonus * log(DFS_WORD_BONUS_BASE)
P = --pair-bonus * log(DFS_PAIR_BONUS_BASE)
```

Both are non-negative while solo words are active. Preserve the current base
score of each selected entry:

```text
log(count)
+ W for an existing multi-word entry
+ P when that multi-word entry is in --pairs
```

For a selected single-word segment and a solo word, an ordinary index edge has
weight `W`; a `--pairs` edge has weight `W + P`. Collapse the two phrase orders
to one edge. Choose the maximum-total-weight bipartite matching, with capacity
one on each selected segment occurrence and each unique solo word. Unmatched
vertices are allowed.

This implies:

- one segment earns at most one `W` and one `P`;
- one solo word is never spent twice;
- segment order cannot change the score;
- a zero-weight edge may be matched or not without changing the score; and
- scarcity is resolved in favor of the greatest total bonus, not greedily and
  not by maximizing assignment count ahead of score.

The score must round-trip through `query-index --score` when the same
`--solo-words`, `--pairs`, and bonus arguments are supplied.

## Search invariant: optimistic queue, exact retention

The once-per-answer constraint cannot be represented by a static member score
or by the current phase-2 cache key. Do not add a used-solo mask to that key;
with 16 words it could multiply the projected state space by 65,536.

For one single-word member profile, define its local upper bonus as:

```text
W + P  if it has any --pairs edge
W      else if it has any aggregate-index edge
0      otherwise
```

The search side ignores competition and lets every selected segment take its
local upper bonus. This value is used consistently for:

- class-member ordering;
- member zero's phase-2 score;
- projected and length score bounds;
- phase-3 spelling deltas; and
- the phase-3 pending queue.

The sum of independent local bonuses is at least the exact matching bonus, so
all those values remain admissible upper bounds. Member zero remains the
highest-upper-bound member of its class, and spelling descendants remain
non-improving in pending-queue order.

At concrete spelling expansion, collect its profiles and calculate:

```text
correction = exact_matching_bonus - sum(local_upper_bonus)
```

The correction is never positive. Retain the spelling at:

```text
exact_log_score = pending_upper_log_score + correction
```

Do **not** recompute base score plus exact bonus in a different addition order.
Using the pending value plus a conservatively rounded non-positive correction
guarantees in machine arithmetic that the retained exact score is no greater
than the value tested at every phase-3 cutoff. This closes the floating-point
pruning issue in `findings/solo-words-codex-review.md` without adding a fuzzy
comparison to every cutoff. The helper should assert the mathematical
invariant and round a negative long-double correction toward negative infinity
when converting it to `double`; clamp only a positive rounding artifact to
zero.

The pending queue and every floor comparison continue to use the upper score.
The top-N heap, dedup table, published floor, printed scores, and `--segments`
aggregation use the corrected exact score.

## Shared solo-word context

Add `source/dfs-solo-words.h` and `source/dfs-solo-words.cpp` to
`dfs_class_list_lib`. The context owns the normalized solo list, caches the
solo-side index positions, resolves candidate profiles during extraction, and
performs exact assignment after extraction.

### Pair profiles

With at most 16 solo words, one candidate profile is:

```cpp
struct DfsSoloMasks {
  uint16_t word_mask;
  uint16_t pair_mask;
};
```

Bit `i` denotes an edge to solo word `i`; `pair_mask` is always a subset of
`word_mask`. Resolve profiles only for single-word emissions and only when at
least one effective solo bonus is nonzero.

Use `IndexReader` continuation positions rather than rebuilding both phrases:

- Resolve each solo once with `aggregate_entry_position()`.
- For `"candidate solo"`, continue from the trailing-space position already
  available at the candidate's phase-1 emission.
- For `"solo candidate"`, continue from the saved solo position.
- Probe `DfsPairSet` once per candidate/solo pair; because the loader stores
  both orders, either key order is sufficient. A hit sets both masks even when
  neither index direction exists.

The context is populated before parallel phase 2/3 begins and frozen before it
is shared with workers. Store exact profiles out of line in a compact sorted
table keyed by the stable candidate text and length. Phase 3 looks up only
selected single-word members whose score flags say an edge exists. This avoids
both a per-record profile ID limit and a node allocation per profile.

### Exact matcher

Keep traversal separate from a pure graph routine. Avoid both greedy matching
and floating-point path costs.

For each reachable matching cardinality `K`, compute the maximum possible
number `H[K]` of `--pairs` edges using integer `0/1` edge costs and a
successive-augmenting-path flow. Residual edges can be negative, so use
Bellman-Ford on this tiny graph or another explicitly negative-edge-safe
shortest-path algorithm. There are at most 16 augmentations.

Then choose the best of:

```text
K * W + H[K] * P, for K = 0..maximum cardinality
```

This exactly maximizes the two available edge weights without comparing
floating-point values inside residual-path selection. A disjoint-mask fast
path returns zero correction without building the graph. It covers every
spelling with at most one eligible segment and every spelling whose possible
partners do not overlap.

### Zero-cost fast paths

- When `--solo-words` is absent, pass no context and execute the current path.
- When both effective bonuses are zero, do not resolve or probe profiles; the
  option is score-inert.
- When `W == 0` and `P > 0`, only `--pairs` edges can affect score, so skip
  aggregate-index probes.
- When `P == 0`, pair and ordinary edges have the same weight; exact matching
  needs only their union.

These gates matter because `DfsExtractor::emit()` is a phase-1 hot path.

## Packed-member representation

Keep `DfsPackedMember` at 16 bytes and `IntermediateMember` at 24 bytes.
Replace `known_pair` with `uint16_t score_flags` and define bits for:

- the member itself being a known multi-word pair;
- a single-word member having any solo word edge; and
- a single-word member having any solo pair edge.

Expose the flags through `DfsMemberView`. Equal member text deterministically
has equal flags, so `same_member()` need not compare them; document and assert
that invariant where profiles are registered.

Do not put the two 16-bit masks into `DfsPackedMember`. The Claude draft's
16-to-24-byte growth would make members span cache lines irregularly, reduce
cache density from four to two or three records per line, and add 50% to the
packed member arena on every search even when the option is absent.

Keep `DfsScoreModel::segment_log_score()` as the exact static score of the
entry itself. Add explicit helpers for a solo profile's local upper bonus and a
member's base-plus-solo upper score. Do not masquerade a solo edge as
`multi_word`/`known_pair`; the solo bonus is conditional on whole-answer
assignment.

Rename `best_member_log_scores` and phase-3 `log_score` fields/locals to
`*_upper_*` where they cease to be exact. Keep the rename limited to the score
path; no search-cache shape or recurrence changes are required.

## CLI behavior

In `source/dfs-cli-args.h/.cpp`:

- add `DFS_OPT_SOLO_WORDS` and a required-argument common long option;
- store parsed words in `DfsCommonArgs`;
- append comma-separated fields across repeated occurrences;
- reject empty/malformed fields, duplicates, and a 17th word with diagnostics
  naming `--solo-words`;
- mark the option compatible with `query-index --score`; and
- after parsing either CLI, reject a negative word or pair bonus when the list
  is nonempty.

Both help texts must explain external/non-consuming context, both pair orders,
the index-or-`--pairs` edge rule, once-per-answer use, and the 16-word limit.
They must also state that bonuses must be non-negative with this option and
that the aggregate index phrase test matches normal phase-1 scoring.

Construct the context only after the pair file and index are available. The
existing rule that zeroes `pair_bonus` when no pair file is supplied remains
unchanged.

## `dfs-anagrams` integration

In `source/dfs-class-list.{h,cpp}`:

- accept an optional mutable solo context during extraction;
- pass the single-word terminator's continuation position into `emit()`;
- register single-word profiles and copy only their category flags into the
  packed member; and
- order members by static segment score plus local solo upper bonus.

In the phase-2 score path:

- use member zero's base-plus-solo upper score in all representative, length,
  projected, and certificate bounds;
- preserve the current rounding envelopes and cache keys; and
- make comments/names say upper bound rather than exact representative score.

In `source/dfs-output.{h,cpp}`:

- borrow the frozen solo context in `DfsTopN`;
- queue and generate descendants by upper score;
- collect exact profiles while constructing a concrete spelling;
- apply the non-positive correction once before `offer()`; and
- keep every early cutoff on the upper score while publishing only exact heap
  floors.

## `query-index` integration

### `--score`

For each validated single-word entry, resolve its profile. Accumulate the same
static-plus-local-upper score used by DFS, then apply the same whole-sequence
correction once. Multi-word entries retain only their current bonuses. This
keeps the score formula and floating-point grouping as close to DFS as the two
interfaces allow.

### Ordinary output

Each row is a one-segment solution, so its exact solo bonus is its local upper
bonus. Since `pair_edge => word_edge` and bonuses are non-negative, the output
still needs only three constant-bonus groups:

- no effective bonus;
- word bonus only (multi-word non-pairs and ordinary solo edges); and
- word plus pair bonus (known multi-word pairs and solo pair edges).

Retain the current bounded partial-sort plus three-way merge. Do not replace it
with an `O(E log E)` all-member score sort. The raw-count fast path remains
active whenever both effective bonuses are zero, including score-inert
`--solo-words` input.

`--csv` prints phrases only, so solo profiles cannot change its rows.

## Phase 1 — shared profile and assignment helper [x]

Files:

- `source/dfs-solo-words.h` (new)
- `source/dfs-solo-words.cpp` (new)
- `source/test-dfs-solo-words.cpp` (new)
- `source/meson.build`

Tasks:

- [x] Implement solo resolution, directional aggregate probes, pair-file
      edges, profile freezing/lookup, local upper bonuses, the disjoint fast
      path, the integer matching frontier, and conservative correction.
- [x] Keep the graph routine independent of `IndexReader` so its invariants are
      tested without a large fixture.
- [x] Cover both index directions, a pairs-only edge, one-use scarcity, a
      greedy-fails reroute, a case where maximum score uses fewer edges than
      maximum cardinality, zero bonuses, and `correction <= 0` including a
      one-ulp conversion boundary.
- [x] Run the focused test, `/review`, and `git diff --check`.

Completion gate: the helper returns admissible local upper bonuses and an
exact, order-independent, capacity-constrained correction without any CLI
behavior or packed-record change.

## Phase 2 — expose and integrate the option [x]

Primary files:

- `source/dfs-cli-args.h`, `source/dfs-cli-args.cpp`
- `source/dfs-score.h`, `source/dfs-score.cpp`
- `source/dfs-class-list.h`, `source/dfs-class-list.cpp`
- `source/dfs-search.cpp` and score-bound naming call sites
- `source/dfs-output.h`, `source/dfs-output.cpp`
- `source/dfs-anagrams.cpp`, `source/query-index.cpp`
- `source/test-dfs-cli.sh`, `source/test-dfs-output.cpp`
- `source/test-query-index.sh`

Tasks:

- [x] Add parsing, validation, diagnostics, and help to both CLIs.
- [x] Populate score flags while preserving the 16-byte packed and 24-byte
      intermediate static assertions.
- [x] Apply local upper scores consistently to class ordering and every search
      bound.
- [x] Apply exact correction only after concrete spelling expansion.
- [x] Apply the same logic to `query-index --score` and the efficient ordinary
      three-group merge.
- [x] Add smoke cases for index-leading, index-trailing, and pairs-only edges;
      pair bonuses; one-use scarcity; an assignment reroute; a bounded top-N
      winner; DFS/query score round-trip; ordinary bounded output equaling the
      unlimited prefix; aggregate-prefix semantics; and byte-identical output
      with omitted or score-inert solo words.
- [x] Add only focused parser failures: empty field, malformed word, duplicate,
      17th word, missing argument, and negative bonus with solo words.
- [x] Run focused tests, then `/review`, affected reruns, and
      `git diff --check`.

Completion gate: both CLIs share one documented scoring contract, all retained
scores enforce one-use capacity exactly, all pruning compares admissible upper
bounds, and the no-option path retains its current representation and output.

## Build, test, and performance gates

Use the repository setup:

```bash
source ~/code/nutrimatic/.env/bin/activate
conan build .
source build/dep-info/conanbuild.sh
meson test -C build dfs-solo-words dfs-output dfs-cli query-index-cli
```

Keep the new tests to the smoke cases above. Run the complete Meson suite only
after the focused tests pass.

Performance is a completion criterion, not an optional spot check:

1. Preserve and assert `sizeof(DfsPackedMember) == 16` and
   `sizeof(IntermediateMember) == 24`.
2. Before every timing run, check the host process table for both
   `query-index` and `dfs-anagrams`; do not kill another session.
3. Load `IDX` and `S6` exactly as `AGENTS.md` specifies. Use a bounded S6 bag,
   not the full 90-letter benchmark.
4. Capture alternating baseline/new runs of phase-1-dominated `query-index`
   with output redirected, recording median wall time and peak RSS. The
   no-option and score-inert-option paths must show no repeatable regression
   beyond normal run noise; investigate any repeatable regression above 2%.
5. Measure active runs with 1, 4, and 16 solo words. Record phase-1 wall/RSS and
   the number of resolved profiles/edges so observed scaling has an
   explanation.
6. Measure a bounded real-index DFS run that expands enough spellings to
   exercise exact matching. Record phase-2 nodes, solutions, spellings
   expanded, wall time, and peak RSS with and without solo words. Correctness
   permits optimistic reuse of capacity, but a large expansion multiplier is
   a release blocker until the bound looseness is understood.

Write the commands, uncontended-process checks, counters, and results to a
findings file during implementation. Do not choose a more elaborate packed
layout or capacity-aware cache state preemptively; use those measurements to
identify the actual hot cost.

## Known risks and follow-ups

- **Bound looseness:** many selected segments may share one solo partner, so
  phase 2 can overestimate that bonus many times. Correctness is unaffected,
  but phase-2 and phase-3 pruning can degrade. The required DFS benchmark is
  specifically intended to reveal this.
- **Profile lookup locality:** a sorted out-of-line table preserves the hot
  member layout but adds lookups for eligible concrete spellings. If profiling
  shows this dominates, use a conditional compact profile-ID or stable-pointer
  lookup for active solo runs; do not enlarge every member record.
- **Pair-file authority:** a pairs-only edge can reward a combination absent
  from the corpus index. This is deliberate because `--pairs` is an explicit
  user assertion, but it should be visible in help and tests.
- **Aggregate presence:** an apparent two-word pair may exist only as the
  prefix of a longer indexed phrase. This matches current aggregate scoring but
  may surprise users who interpret "entry" as an exact residual.
- **16-word ceiling:** this is the settled bound that enables compact masks.
  Raising it later requires a different profile representation, not a silent
  truncation or a wider packed member.

## Commit protocol

For each phase, mark it `[-]`, implement only that phase, run its focused
verification and performance work, invoke `/review`, resolve every correctness
finding, rerun affected checks, and run `git diff --check`. Mark the phase `[x]`
in the same diff as its implementation and commit only that phase's files.

Inspect `git status --short` before every commit and preserve unrelated or
concurrent worktree changes.
