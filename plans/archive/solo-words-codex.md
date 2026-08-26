# Superseded

This proposal is retained as design history. The canonical implementation plan
is `plans/solo-words.md`, which preserves its 16-byte packed-member design and
closes the correctness and semantic issues in
`findings/solo-words-codex-review.md`.

# Plan: add `--solo-words` candidate-pair scoring

## Outcome

Add the shared option
`--solo-words WORD[,WORD,...]` to `dfs-anagrams` and `query-index`.
The supplied words are external context: they do not consume letters, become
segments, or appear in output. Instead, each selected single-word index entry
may pair with one supplied solo word when either ordered two-word phrase exists
in the index. Such a match earns the existing word bonus, and additionally
earns the existing pair bonus when that word pair is in `--pairs`.

Within one complete anagram or `query-index --score` sequence, a supplied solo
word occurrence can be assigned to at most one single-word segment occurrence.
The exact score therefore depends on the whole solution, unlike the current
word and pair bonuses, which are static properties of individual index entries.

## Scoring contract

Let `W = --word-bonus * log(DFS_WORD_BONUS_BASE)` and
`P = --pair-bonus * log(DFS_PAIR_BONUS_BASE)`. Preserve the existing base score
of every selected member:

```text
log(count)
+ W when the member is already a multi-word entry
+ P when that multi-word entry is in --pairs
```

For every selected single-word member occurrence and solo-word occurrence, add
an assignment edge when either `"solo member"` or `"member solo"` exists as a
complete index entry. Its weight is:

```text
W + P when the two words are in --pairs
W     otherwise
```

First maximize the number of assignments, then maximize their total edge
weight, with capacity one on each selected segment occurrence and each supplied
solo-word occurrence. Add that matching's weight to the solution's base score.
The maximum-cardinality rule implements "if it forms a pair, it earns the
bonus" even for the already-supported zero and negative bonus values; score is
the tie-break between assignments when scarcity prevents every eligible
segment from being paired.

Consequences to preserve explicitly:

- A single-word segment earns at most one word bonus and one pair bonus, even
  if it can pair in both orders or with several supplied words.
- One supplied occurrence is usable once. Repeating a word in the option, for
  example `--solo-words red,red`, intentionally supplies capacity two.
- Repeating the option appends more occurrences, just like one longer
  comma-separated argument.
- `--pairs` is order-insensitive because `load_pair_file()` already stores both
  orders. A pair present in both index orders still creates one assignment edge
  and one set of bonuses.
- Only `word_count == 1` members participate. Existing phrase word/pair bonuses
  remain unchanged and do not consume solo words.
- Pair existence is based on the aggregate trailing-space trie entry, matching
  phase 1 and `query-index --score`; the pair's corpus count is irrelevant.
- `--dict`, the minimum word length, and `--max-extract-words` continue to
  control extracted anagram members only. They do not filter the external solo
  words or the two-word context probe.

## Architectural constraint: exact output, optimistic search

Phase 2 walks anagram classes, not concrete spellings. It cannot know which
single-word member each class will expand to, and adding used-solo-word state to
the score-cache key would multiply the projected state space by the solo-word
capacity combinations.

Keep phase 2 independent of that state. For each single-word member, compute a
local upper bound equal to the best positive solo edge it could take, ignoring
whether another segment consumes the same solo word. Sort class members by:

```text
existing segment score + local solo upper bound
```

The best member of each class and every projected/length score table then carry
an admissible, possibly loose upper bound. Phase 2 may reuse one solo word in
that bound any number of times; phase 3 must not. This is safe because exact
matching can never exceed the sum of the independent local maxima.

Rename representative-score variables and comments on this path to say
`upper` or `upper_bound`. Once solo words are active, the phase-2 value is no
longer necessarily the score of member-zero spelling.

Phase 3 must also stop assuming that the pending expansion candidate's value is
an exact score. A lower-ranked member can free a scarce solo word and improve
the exact matching even though its upper bound cannot improve. Therefore:

- order the spelling-expansion priority queue by the monotone upper bound;
- compute the exact base score and exact solo matching before offering a
  spelling to the top-N heap;
- compare the published result floor only with upper bounds; and
- stop expanding only when the best pending upper bound is at or below the
  exact result floor.

This retains all current pruning while preventing a capacity conflict from
hiding a valid top-N spelling.

## Shared solo-word matcher

Add `source/dfs-solo-words.h` and `source/dfs-solo-words.cpp` to
`dfs_class_list_lib`. Give the helper three jobs:

1. Own the normalized solo-word multiset as unique text plus occurrence
   capacity.
2. Resolve and cache, for each encountered standalone candidate word, which
   unique solo words form an index pair and whether that edge is in `--pairs`.
3. Return both a cheap per-member upper-bound category and the exact
   maximum-cardinality, maximum-weight matching bonus for a vector of selected
   member occurrences.

Use `IndexReader`'s existing continuation API instead of constructing every
pair string and looking it up from the root:

- Resolve every unique solo word once with `aggregate_entry_position()`.
  Failure is not an error; that word may still occur second in `"candidate
  solo"`.
- At a single-word extraction terminator, retain its already-known
  `EntryPosition`. Test `"candidate solo"` by continuing from it.
- Test `"solo candidate"` by continuing from the solo word's saved position.
- Collapse the two directions into one edge.

`DfsExtractor::emit()` currently receives only count and word count. Thread the
trailing-space choice's continuation and aggregate count into it so the hot
phase-1 path does not retraverse the candidate word. Multi-word emissions skip
all solo work.

Cache profiles as owned candidate text plus edges `(solo_id, known_pair)`. The
packed member only needs two bits saying whether it has any ordinary solo edge
and any known-pair solo edge; exact phase-3 lookup can use a frozen, sorted
profile vector with heterogeneous `string_view` binary search. This avoids
growing `DfsPackedMember`, imposing a 16-bit profile-ID limit, or allocating a
temporary NUL-terminated string during concurrent expansion.

Compact duplicate supplied words into one target with a capacity count. The
exact matcher should use a solution-sized maximum-cardinality, maximum-cost
augmenting-path flow: segment nodes have capacity one, unique solo nodes have
their parsed capacity, and edge costs are the log bonuses above. Augment until
no source-to-sink path remains, including zero- or negative-cost paths, and use
residual edges so each flow cardinality has the best possible score. This
handles rerouting cases that a greedy first-match algorithm gets wrong while
keeping work proportional to the usually small number of segments and supplied
words in one result.

The matcher is populated before parallel phase 2/3 begins and is read-only
after it is frozen, so `DfsTopN::emit()` remains thread-safe without another
lock.

## Packed-member and score-model changes

Replace `DfsPackedMember::known_pair` and the corresponding intermediate field
with a `uint16_t score_flags`, retaining the 16-byte record size. Define bits
for:

- the member itself being a known multi-word pair;
- at least one ordinary solo edge; and
- at least one known-pair solo edge.

Expose the same information through `DfsMemberView`. `same_member()` need not
compare the flags because equal member text deterministically produces equal
flags, but document that invariant.

Keep `DfsScoreModel::segment_log_score()` as the exact score for the member
itself. Add helpers for:

- an ordinary or known-pair solo edge's log bonus;
- the best nonnegative local solo upper bound represented by the two flags;
  and
- a packed member's base-plus-upper score for class ordering and search bounds.

Do not fold solo matching into the `multi_word` or `known_pair` arguments. That
would make the same member appear to have a static bonus and would reintroduce
the once-per-solution bug in phase 3.

## CLI behavior

In `source/dfs-cli-args.h/.cpp`:

- Add a shared long-option code and a
  `{ "solo-words", ..., OPTPARSE_REQUIRED }` row to
  `DFS_COMMON_LONG_OPTIONS`.
- Store parsed occurrences in `DfsCommonArgs`.
- Parse comma-separated, nonempty lowercase `a-z0-9` words. Reject empty
  fields, spaces, punctuation, and uppercase with an error naming
  `--solo-words` and the bad field. This matches the strict CLI/index spelling
  convention rather than the lossy cleanup intended for dictionary files.
- Mark the option score-compatible so it works in `query-index --score`.
- Append values across repeated occurrences of the option.

Update both usage strings to show `[--solo-words WORD[,WORD,...]]` and explain
that these are non-consuming context words, single-word segments receive the
word bonus when either pair order exists, and each supplied occurrence is used
at most once per full solution.

Construct the matcher only after `--pairs` is loaded and `IndexReader` exists.
When no solo words are supplied, pass `NULL`/take the current fast path so
default class extraction, record flags, output ordering, and score arithmetic
remain byte-identical.

## `dfs-anagrams` integration

In `source/dfs-class-list.{h,cpp}`:

- Accept an optional mutable matcher during extraction.
- Register only single-word emissions and copy the returned profile-category
  bits into `score_flags`.
- Include the local solo upper bound in `MemberOrder`, preserving the existing
  count/text tie-breaks.
- Keep member zero as the highest upper-bound member of its class.

In `source/dfs-search.{h,cpp}`, `source/dfs-search-data.h`, the runners, and the
projected-bound code:

- Feed the member-zero upper-bound score into `best_member_log_scores` and all
  score-bound builders.
- Rename that vector and representative-score locals to make their upper-bound
  role explicit.
- Keep the recurrence and cache keys otherwise unchanged; solo-word capacities
  are deliberately absent from phase 2.
- Preserve the rounding padding used by current admissible bounds.

In `source/dfs-output.{h,cpp}`:

- Borrow the frozen matcher in `DfsTopN`.
- Change `ExpansionCandidate` to hold an upper bound rather than an assumed
  exact score.
- Compute a tuple's upper bound from the phase-2 representative upper plus the
  per-member upper-score deltas. Because members are sorted by that same value,
  descendants remain non-improving in queue order.
- Compute the concrete tuple's existing segment score independently, collect
  profiles for its single-word members, and add the matcher's exact assignment
  bonus.
- Offer and retain that exact score. Use only the upper bound for early floor
  checks and pending-queue termination.
- Leave word-set deduplication, segment lengths, output formatting, and
  `--segments` aggregation unchanged; they will naturally consume the exact
  retained score.

## `query-index` integration

### `--score`

While validating the supplied exact entry sequence, retain each entry's
`EntryPosition` along with its aggregate count. Register only entries without
an interior space, freeze their profiles, compute the current exact base score,
then add one exact matching bonus across the entire sequence. This gives
`query-index --score` the same scarcity rule as a `dfs-anagrams` result and
keeps its round-trip role intact when the same `--solo-words`/`--pairs` options
are supplied.

### Ordinary extraction output

Treat each displayed row as a one-segment solution. A single-word row with any
edge receives its highest-weight eligible edge, even when that configured
bonus is zero or negative, since no other segment competes for its solo word.
Multi-word rows retain only their existing word/pair bonuses.

The current query output uses three count-sorted groups because each group has
a constant bonus. Generalize the category function and k-way merge to cover:

- plain single words;
- single words with ordinary-only, known-pair-only, or both solo-edge kinds;
- unlisted multi-word entries; and
- known-pair multi-word entries.

Each category is still count ordered. Retain at most `-n` members from each and
merge their heads by exact one-row score, preserving the current linear scan
plus bounded partial sorts rather than replacing it with an all-entry score
sort. The raw-count fast path remains valid only when both effective bonus
magnitudes are zero; supplying solo words alone at default zero bonuses must
not change output formatting.

`--csv` filters out single-word members, so solo words cannot affect its rows;
keep the shared extraction/matcher plumbing but preserve its phrase-only
output.

## Implementation phases

Status values are `[ ]` pending, `[-]` in progress, and `[x]` complete.

| Phase | Deliverable | Status | Proposed commit |
|---:|---|:---:|---|
| 1 | Shared solo-word resolver and exact assignment scorer | [ ] | `Add solo-word pair matching` |
| 2 | Shared CLI option and complete scoring integration | [ ] | `Score single words with solo-word pairs` |

### Phase 1 — shared matcher [ ]

Files:

- `source/dfs-solo-words.h` (new)
- `source/dfs-solo-words.cpp` (new)
- `source/meson.build`
- a focused matcher test, if the graph interface can be tested without
  exposing implementation-only APIs

Tasks:

- [ ] Implement unique-word capacities, directional index resolution,
  direction collapse, pair-list annotation, profile freezing, local upper
  bounds, and exact maximum-cardinality, maximum-weight assignment.
- [ ] Keep index traversal separate from the pure assignment routine so the
  scarcity/rerouting case can be tested with a tiny synthetic graph.
- [ ] Verify duplicate capacities, a one-solo/two-segment conflict, a graph
  requiring an augmenting-path reroute, and an extra pair bonus. Keep this as
  one compact test rather than broad coverage.
- [ ] Build and run the focused test.
- [ ] Run `/review`, address correctness findings, and run
  `git diff --check` before committing only this phase's files.

Completion gate: the helper can resolve both pair directions, return an
admissible local upper bound, and compute the exact capacity-constrained bonus
without any CLI-visible behavior.

### Phase 2 — expose and integrate the option [ ]

Files:

- `source/dfs-cli-args.h`
- `source/dfs-cli-args.cpp`
- `source/dfs-score.h`
- `source/dfs-score.cpp`
- `source/dfs-class-list.h`
- `source/dfs-class-list.cpp`
- `source/dfs-search.h`
- `source/dfs-search-data.h`
- `source/dfs-search.cpp`
- `source/dfs-all-runner.h`
- `source/dfs-all-runner.cpp`
- `source/dfs-search-projected.cpp`
- `source/dfs-solution-sink.h`
- `source/dfs-output.h`
- `source/dfs-output.cpp`
- `source/dfs-anagrams.cpp`
- `source/query-index.cpp`
- `source/make-dfs-test-index.cpp`
- `source/test-dfs-cli.sh`
- `source/test-dfs-output.cpp`
- `source/test-query-index.sh`
- `source/meson.build` if phase 1 did not add a test target

Tasks:

- [ ] Add shared parsing, help text, and matcher construction to both CLIs.
- [ ] Populate packed flags during extraction without changing packed record
  sizes.
- [ ] Convert class ordering and every phase-2 score table to the optimistic
  solo-aware upper score, with naming/comments that distinguish it from exact
  scores.
- [ ] Convert phase-3 expansion to upper-bound queueing plus exact per-spelling
  matching.
- [ ] Apply exact matching to `query-index --score` and the one-row equivalent
  to ordinary query output, including the generalized category merge.
- [ ] Add a small disjoint fixture whose pair graph demonstrates both leading
  and trailing index phrases and scarcity. If extending
  `make-dfs-test-index.cpp` changes `reader.count()`, update the few explicit
  synthetic corpus-total expectations in `test-query-index.sh` together.
- [ ] Add smoke assertions that:
  - a single candidate is promoted by a leading or trailing solo pair;
  - a pair also in `--pairs` receives both bonuses;
  - two solution segments cannot reuse one supplied occurrence;
  - duplicating that solo word permits two uses;
  - a bounded `-n` run still retains the exact winner, exercising the
    optimistic phase-2 and phase-3 cutoff path;
  - `query-index --score` reproduces the `dfs-anagrams` score when passed the
    same solo/pair options; and
  - omitted `--solo-words` output remains byte-identical.
- [ ] Add parser failures for an empty list field and a missing option
  argument; avoid a larger validation matrix.
- [ ] Build and run the two focused CLI tests.
- [ ] Run `/review`, resolve every correctness finding, rerun affected checks,
  and run `git diff --check` before committing only this phase's files.

Completion gate: both CLIs document and accept the common option, exact full
solutions enforce each supplied occurrence once, top-N pruning remains
admissible, query single-row and sequence scoring agree with DFS scoring, and
the no-option path is unchanged.

## Build and verification

Use the repository setup:

```bash
source ~/code/nutrimatic/.env/bin/activate
conan build .
source build/dep-info/conanbuild.sh
meson test -C build dfs-cli query-index-cli
```

Keep tests to the smoke cases above. No timing gate is required. If a real-index
spot check is useful, use:

```bash
export IDX=~/code/nutrimatic/idx/wiki-merged.5.index
```

Before any timing comparison, inspect the host process table for both
`dfs-anagrams` and `query-index` as required by `AGENTS.md`.

Before each commit, inspect `git status --short`, preserve unrelated concurrent
changes, stage only the phase's files, and complete `/review` first.
