# Plan: order class members by score

## Outcome

`--word-bonus N` stops cancelling inside a class and stops leaking a per-class
constant across classes. Class members are sorted by segment score rather than
raw count, which makes member 0 the class's best member by construction; the
phase-2 bound, the phase-3 delta, and the expansion's monotonicity invariant
then all rest on the same ordering.

`findings/bonus-issues.md` is the analysis this plan implements. Its notation is
used here without restating the derivation: $B$ is the bonus magnitude,
$s(m) = \log \operatorname{count}(m) + B \cdot [\,m \text{ is multi-word}\,]$,
and $D(c) = \operatorname{best}(c) - \log c_0$ is the error term this plan
drives to zero.

## Scope

### Included

- A bonus magnitude on `DfsClassList`, used by `member_order()` to sort members
  score-descending.
- `spelling_log_score()`'s delta computed in score space against member 0.
- Removal of the `bonus_reorders` class-best scan in `DfsAnagramSearch`.
- Comment updates where member order is documented as count order.

### Excluded

- `--pair-bonus` and the third term in `DfsScoreModel`. This plan is its
  prerequisite, not part of it.
- The pair-membership flag on `DfsPackedMember` / `IntermediateMember` and the
  set probe in `DfsExtractor::emit()`.
- query-index's two-group partition/merge (`source/query-index.cpp:378-404`).
  It is correct while exactly one bonus exists, and this plan does not change
  the number of bonuses.
- `plans/pair-bonus.md`, which lands `--pairs FILE` and touches none of this.

## Decisions

1. **Score order, not a repaired delta.**
   Computing $s(m_j) - \operatorname{best}(c)$ in `spelling_log_score()` while
   leaving members count-sorted is correct arithmetic that breaks the
   expansion: a rarer multi-word member at index 3 can outscore the single-word
   member at index 1 by $B$, so a descendant improves on its parent and both
   early breaks (`source/dfs-output.cpp:143-146`,
   `source/dfs-output.cpp:161-166`) plus the `next.log_score > published` filter
   (`source/dfs-output.cpp:202`) start discarding spellings that belong in the
   output. Sorting by score restores that invariant instead of merely preserving
   it.

2. **`DfsClassList` takes the bonus magnitude, not a `DfsScoreModel`.**
   A trailing `double multi_word_log_bonus = 0.0` parameter. Ordering needs only
   the bonus term — the segment boundary score is constant across the members of
   a class and cannot reorder them — so this keeps phase 1 free of a dependency
   on `dfs-score.h`, and the default leaves all nine existing construction sites
   unchanged.

3. **`DfsTopN` takes a `DfsScoreModel const*`.**
   Phase 3 needs `segment_log_score()` itself, not just the magnitude, and
   already sits downstream of the model.

4. **The zero-bonus path stays bit-identical.**
   At $B = 0$ the score comparator reduces to count descending: log is injective
   over this corpus's count range in double, which is the same argument
   `count_order()` already documents at `source/query-index.cpp:171-175`. Ties
   keep the existing text and word-count tie-breaks, so member order under the
   default is unchanged and no existing test output moves.

## Status and commit protocol

Use these status values in the phase headings:

```text
[ ] pending
[-] in progress
[x] complete
```

For the implementation phase:

1. Change the phase status to `[-]`.
2. Implement only that phase's scope.
3. Run the phase's build and verification.
4. Invoke `/code-review` and review the complete diff before committing.
5. Resolve every correctness finding and rerun affected checks.
6. Run `git diff --check`.
7. Mark the phase `[x]`, including its verification checkboxes, in the same
   diff.
8. Commit the implementation, tests, and status update together using the
   proposed subject.

The worktree may contain unrelated or concurrent changes. Before every commit,
inspect `git status --short`, stage only the phase's files, and preserve all
unrelated edits.

## Phase status

| Phase | Deliverable | Status | Proposed commit |
|---:|---|:---:|---|
| 1 | Score-ordered members and score-space deltas | [x] | `Order class members by score` |

The three sites land together. Any partial state is a different wrong answer
rather than a smaller one: score-ordered members with a count-space delta
mismeasures against a new member 0, and a score-space delta with count-ordered
members breaks the expansion per decision 1.

## Common setup

```bash
source ~/code/nutrimatic/.env/bin/activate
conan build .
```

`conan build .` does not run the tests. Run them explicitly:

```bash
meson test -C build
```

Corpus-backed checks use the configured index:

```bash
export IDX=~/code/nutrimatic/idx/wiki-merged.5.index
```

The `bonus_reorders` removal deletes a per-class scan, so phase-2 timing can
only improve. No timing gate applies, but the comparison run below wants a quiet
box: check the host process table for competing `dfs-anagrams` and
`query-index` processes first.

## Phase 1 — score-ordered members and score-space deltas [x]

### Purpose

Drive $D(c)$ to zero at its source, then let the two consumers that were
compensating for it stop compensating.

### Files

- `source/dfs-class-list.h`
- `source/dfs-class-list.cpp`
- `source/dfs-output.h`
- `source/dfs-output.cpp`
- `source/dfs-search.cpp`
- `source/dfs-anagrams.cpp`
- `source/query-index.cpp`
- `source/make-dfs-test-index.cpp`
- `source/test-dfs-cli.sh`
- `source/test-query-index.sh` (the new fixture entries move the synthetic
  corpus total from 136 to 1141, which every score constant there divides by)

### Tasks

- [x] Add `double multi_word_log_bonus = 0.0` as the last `DfsClassList`
      constructor parameter (`source/dfs-class-list.h:134-137`), store it, and
      document that it selects the member ordering.
- [x] Rewrite `member_order()` (`source/dfs-class-list.cpp:345-350`) to compare
      `log(double(count)) + (word_count > 1 ? bonus : 0.0)` descending, then
      fall through to the existing count, text, and word-count tie-breaks
      unchanged. The comparator needs the bonus, so it stops being a free
      function usable as a bare `std::sort` predicate
      (`source/dfs-class-list.cpp:462`) — pass it as a lambda or a small functor
      holding the magnitude.
- [x] Leave `same_member()` (`source/dfs-class-list.cpp:352-356`) alone. It
      compares fields, not order, and the `std::unique` it feeds is unreachable
      regardless.
- [x] Update `DfsClassRecord::members`' "highest count first"
      (`source/dfs-class-list.h:54`) to say highest score first, and note that
      the two coincide at bonus 0.
- [x] Pass `score_model.multi_word_log_bonus()` at the two production
      construction sites (`source/dfs-anagrams.cpp:287`,
      `source/query-index.cpp:310`). Both already build the model; if
      construction order puts the class list first, move the model above it.
- [x] In `DfsAnagramSearch`'s constructor (`source/dfs-search.cpp:206-225`),
      delete `bonus_reorders` and the inner scan. `best_member_log_scores` is
      `first_segment_log_score()` of member 0. Replace the three-sentence
      comment above it with one stating that member 0 is the class's best score
      by construction.
- [x] Give `DfsTopN` a `DfsScoreModel const*` (`source/dfs-output.h`,
      constructor at `source/dfs-output.cpp:83`), asserted non-NULL, and thread
      it to `spelling_log_score()`.
- [x] Rewrite `spelling_log_score()`'s accumulation
      (`source/dfs-output.cpp:73-79`) as

  ```cpp
  score += model.segment_log_score(view_j.count, view_j.word_count > 1) -
           model.segment_log_score(view_0.count, view_0.word_count > 1);
  ```

      keeping the `member_indexes[i] == 0` early `continue`, which is now exact
      rather than incidentally exact.
- [x] Update the `DfsTopN` call sites for the new constructor argument.
- [x] `source/dfs-all-runner.cpp:66-76` needs no change: it accumulates
      whatever `best_member_log_scores` holds, and that value's definition is
      unchanged — only its computation gets cheaper.
- [x] Add a within-class fixture to `source/make-dfs-test-index.cpp`, after the
      `gh ij ` entry so the writer stays sorted:

  ```cpp
  // One class holding both spellings of the same letters, so --word-bonus
  // has a single-word member to promote the phrase over. k-n are otherwise
  // unused, keeping this disjoint from every other bag in this fixture.
  writer.next("kl mn ", 0, 5);
  writer.next("klmn ", 0, 1000);
  ```

- [x] Add the promotion case to `source/test-dfs-cli.sh`: `klmn -m 2 -n 5` at
      `--word-bonus 0` puts `klmn` first, and at `--word-bonus 1` puts `kl mn`
      first with score `5.000e+06` and `klmn` second at `1000.`. Assert both
      the order and the two scores — the order alone would pass on a fix that
      promoted the phrase while still misreporting the single word.

### Verification

- [x] `conan build .` succeeds.
- [x] `meson test -C build` passes unchanged. No C++ test constructs a
      `DfsClassList` with a bonus, so every one of them exercises decision 4's
      zero-bonus path.
- [x] Default-path identity, on the real index:

  ```bash
  ./build/dfs-anagrams "$IDX" "${S6:0:16}" -n 20 > /tmp/before.txt
  ```

      captured before the change and diffed byte-for-byte after it.
- [x] The new `klmn` case passes. Today's output on that fixture is

  ```text
  --word-bonus 0        --word-bonus 1          --word-bonus 3
  1000. klmn            5.000e+06 klmn          5.000e+18 klmn
  5.000 kl mn           2.500e+04 kl mn         2.500e+16 kl mn
  ```

      showing both halves of the defect in one place: the single word carries
      the phrase's $D(c)$, the phrase is scored as though the bonus were never
      applied, and no magnitude changes the order. After the fix,
      `--word-bonus 1` reads `5.000e+06 kl mn` then `1000. klmn`, and the
      `--word-bonus 0` column is unchanged.
- [x] Cross-class inflation is gone: `klmn`'s score at `--word-bonus 1` and at
      `--word-bonus 3` both equal its score at `--word-bonus 0`. A single-word
      spelling stops moving with a bonus it never earned, which is exactly
      $D(c) = 0$.
- [x] Corpus spot check, same shape at scale: `eehinrt -m 3` is a real class
      holding `therein` (5511) and `the rein` (167). A `--word-bonus` large
      enough to cover the 33x count gap promotes the phrase after the fix and
      cannot before it.
- [x] `git grep -n bonus_reorders` returns nothing.

### Completion gate

Zero-bonus output is byte-identical, a nonzero bonus reorders within a class,
and single-word scores no longer move with the bonus. Commit as:

```text
Order class members by score
```

## Follow-up

`--pair-bonus` becomes additive once this lands. What it still needs on its own:

- A third term in `DfsScoreModel` and a pair-membership flag in the `reserved`
  fields (`source/dfs-class-list.h:41-47`,
  `source/dfs-class-list.cpp:29-36`), probed from `DfsExtractor::emit()`.
- `DfsClassList`'s ordering input generalized from one magnitude to whatever
  makes a member's score, since the pair term is a property of the text rather
  than of `word_count`.
- query-index's two-group partition/merge split into three groups
  (`source/query-index.cpp:378-404`). The `-n` cutoff is applied per group
  before the merge, so a missing third group drops rows rather than merely
  reordering them.
- query-index's `word_bonus == 0.0` count fast path
  (`source/query-index.cpp:378`) widened to "no bonus of any kind".
- The `--score` path's `find(' ')` multi-word test
  (`source/query-index.cpp:256-260`) given the same pair probe, so it agrees
  with what dfs-anagrams computes for the same sequence.
