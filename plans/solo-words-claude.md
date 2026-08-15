# Superseded

This proposal is retained as design history. The canonical implementation plan
is `plans/solo-words.md`, which incorporates its mask/correction invariants
without growing every packed member by 50%.

# Plan: `--solo-words` as candidate pair partners

## Context

`--word-bonus` and `--pair-bonus` can only reward a segment for what is *inside*
it: a multi-word index entry earns the word bonus, and a `--pairs`-listed entry
earns the pair bonus on top. A single word earns neither, no matter how
obviously it belongs next to something the solver already has in mind.

`--solo-words WORD[,WORD...]` supplies that missing context. Each solo word is a
*candidate pair partner* that costs no letters. A single-word segment that forms
a real two-word pair with one of them — in either order — is promoted as if it
were the phrase it implies. This is the same assertion `--word-bonus` already
makes (`source/dfs-score.h:20-27`: pairs are the objective, and the preference is
asserted, never derived), extended to words whose partner is outside the bag.

Intended outcome: with `--solo-words sea --word-bonus 1`, the single word
`horse` scores like the phrase `sea horse` while `hoarse` keeps its raw count,
and no answer double-counts `sea` across two of its segments.

## Scope

### Included

- `--solo-words WORD[,WORD...]`, a shared option on `dfs-anagrams` and
  `query-index`, at most `DFS_MAX_SOLO_WORDS` (16) words.
- Per-member solo masks computed during phase-1 extraction.
- Solo bonuses in member ordering, the phase-2 bound, phase-3 spelling deltas,
  `query-index`'s default listing, and `query-index --score`.
- Exact one-use-per-answer accounting for the whole-answer score, by max-weight
  bipartite matching between an answer's single-word segments and the solo
  words.

### Excluded

- Solo bonuses for multi-word segments. A segment with an interior space
  already earns the word bonus on its own; the user's rule is for single-word
  segments only, and widening it would double-count.
- Any change to what phase 1 extracts. Solo words never consume letters, never
  appear in output text, and never gate extraction.
- `--csv`, which prints only multi-word entries (`source/query-index.cpp:380-382`),
  so solo bonuses cannot show there. Left alone, documented.

## Decisions

1. **A pair is "formed" if the index has it or `--pairs` has it.**
   For a single-word segment `W` and solo word `S`, tier 1 (the word bonus)
   fires when `"S W"` or `"W S"` is an index entry **or** is in the loaded
   `DfsPairSet`. Tier 2 (the pair bonus, additive) fires only on `--pairs`
   membership. So tier 2 implies tier 1, exactly as `known_pair` already implies
   multi-word — which is what keeps `query-index`'s partition at three groups
   rather than four (see decision 6).

   Consequence worth stating in the usage text: `--word-bonus` defaults to 0, so
   `--solo-words` alone changes nothing. That matches `--pairs`, which is inert
   until a bonus is set.

2. **Both orders, one probe each.**
   The index probe must try both orders, since the trie is not symmetric. The
   `--pairs` probe needs only one, because `load_pair_file()` already inserts
   both orders (`source/dfs-cli-args.cpp:229-233`).

3. **16 solo words, and `DfsPackedMember` grows to 24 bytes.**
   Two `uint16_t` masks — `solo_word_mask` (tier 1) and `solo_pair_mask`
   (tier 2) — become their own fields rather than being packed into the spare
   bits of `known_pair`. `IntermediateMember` absorbs them in existing tail
   padding at no cost; `DfsPackedMember` goes 16 -> 24 bytes, so the phase-1
   member arena grows 50% for every search whether or not `--solo-words` is
   given, and the `static_assert` at `source/dfs-class-list.cpp:408` changes.

   If that ever bites, the lever is a parallel mask array allocated only when
   `--solo-words` is given, reordered alongside `retain_members()`'s `memmove`.
   Do not do this preemptively.

4. **Non-negative bonuses are required with `--solo-words`.**
   A segment's solo bonus is the best tier its masks allow. That is an upper
   bound on what any assignment can give it only while `--word-bonus >= 0` and
   `--pair-bonus >= 0`; with a negative bonus the "best tier" would be to take
   nothing, the per-group-constant-bonus assumption behind
   `query-index`'s merge would break, and the phase-2 bound would stop bounding.
   Reject negatives as a usage error when `--solo-words` is present, and leave
   the existing options unrestricted otherwise.

5. **Optimistic during search, exact at retention.**
   The once-per-answer rule makes a segment's bonus depend on the rest of the
   answer, which no per-member score can express. So every search-side score —
   `MemberOrder`, `best_member_log_scores`, `spelling_log_score()`'s delta,
   the pending-expansion queue — uses the *optimistic* bonus, in which every
   segment assumes it gets its best solo word. That is an upper bound on the
   exact score, so:

   - member 0 stays the class's best member, keeping the phase-2 bound
     admissible and phase-3 descendants non-improving;
   - a candidate pruned because `optimistic <= floor` would also have failed on
     its exact score, since `exact <= optimistic`.

   `DfsTopN::emit()` then computes the exact score once per expanded spelling
   and stores *that*, so the heap, the dedup table, the published floor, and
   every printed number are exact. Only the pruning comparisons stay optimistic.

   The reverse assignment — exact scores in the pending queue — is wrong: a
   child's conflict correction can be smaller than its parent's, so exact scores
   are not monotone down the expansion and the early `break`s at
   `source/dfs-output.cpp:153,172-174,210` would cut off better spellings.

6. **Effective flags, not new score terms.**
   Solo bonuses reuse `multi_word_log_bonus_` and `pair_log_bonus_`. No new
   magnitude, no new `DFS_*_BASE` constant, no new `DfsAnagramSearch`
   constructor parameter. `DfsScoreModel::segment_log_score()` gains two
   defaulted mask parameters, so all eight existing call sites keep compiling
   and the ones that pass masks pick the bonus up automatically.

7. **Exact assignment, not greedy.**
   Conflicts are resolved by max-weight bipartite matching, so the answer's
   score does not depend on segment order — which is what lets a `dfs-anagrams`
   result line round-trip through `query-index --score` and agree. A left-to-
   right greedy is order-dependent and provably suboptimal (two segments, one
   sharing a high-weight partner the other needs).

   A disjoint-mask fast path (OR-accumulate the masks; no overlap means no
   conflict) short-circuits the overwhelming majority of spellings, including
   every spelling with at most one eligible segment.

8. **Parsing lives with the other loaders, probing gets its own module.**
   `--solo-words` cleanup is `--pairs`'s cleanup, so it belongs in
   `dfs-cli-args.cpp` next to `clean_word()` (`source/dfs-cli-args.cpp:22-33`).
   Index probing and matching are new machinery with their own test, so they get
   `source/dfs-solo.{h,cpp}`.

## Status and commit protocol

```text
[ ] pending
[-] in progress
[x] complete
```

For each phase: set `[-]`, implement only that phase, run its verification,
invoke `/code-review` and review the complete diff, resolve every correctness
finding and rerun affected checks, run `git diff --check`, mark `[x]` in the
same diff, then commit implementation, tests, and status together.

The worktree may hold unrelated changes. Inspect `git status --short` before
every commit, stage only the phase's files, and preserve unrelated edits.

## Phase status

| Phase | Deliverable | Status | Proposed commit |
|---:|---|:---:|---|
| 1 | Solo-word probing and matching module | [ ] | `Add solo-word pair probing and assignment` |
| 2 | `--solo-words` in every scoring path | [ ] | `Score single words against --solo-words partners` |

## Common setup

```bash
source ~/code/nutrimatic/.env/bin/activate
conan build .
meson test -C build
export IDX=~/code/nutrimatic/idx/wiki-merged.5.index
source ./setup.sh   # ${S6:0:N}
```

Phase 2 adds work to phase 1's hottest path and changes ranking. Before any
timing comparison, check the host process table (outside any sandbox PID
namespace) for other `query-index` / `dfs-anagrams` runs.

## Phase 1 — probing and matching module [ ]

### Purpose

Land the mask probe and the assignment solver with their own unit test, before
anything calls them. No CLI surface, no behavior change.

### Files

- `source/dfs-solo.h` (new)
- `source/dfs-solo.cpp` (new)
- `source/test-dfs-solo.cpp` (new)
- `source/meson.build`

### Interface

```cpp
inline constexpr size_t DFS_MAX_SOLO_WORDS = 16;

// The solo words, plus each one's index position just past "word ", so a
// leading probe is one continuation walk instead of a walk from the root. A
// word absent from the index simply has no leading position and forms no
// leading pair; that is not an error.
struct DfsSoloWords {
  std::vector<std::string> words;
  std::vector<IndexReader::EntryPosition> leading;
  std::vector<bool> leading_valid;
  size_t size() const { return words.size(); }
};

void dfs_resolve_solo_words(
    IndexReader const& reader, std::vector<std::string> const& words,
    DfsSoloWords* out);

// Bit i of each mask is solo word i. pair_mask is always a subset of
// word_mask: --pairs membership is one of the two ways tier 1 is earned.
struct DfsSoloMasks { uint16_t word_mask; uint16_t pair_mask; };

// Masks for one single-word segment. `after_word` is the entry position just
// past "word " when the caller already holds it -- the extractor does, from
// the space transition that produced the entry -- and NULL when it does not,
// in which case this resolves it. `scratch` is a caller-owned buffer the
// --pairs probe builds its key in, so a hot caller allocates nothing.
DfsSoloMasks dfs_solo_masks(
    IndexReader const& reader, DfsSoloWords const& solo,
    DfsPairSet const* pairs, std::string_view word,
    IndexReader::EntryPosition const* after_word, std::string* scratch);

// The best total solo bonus for one answer, using each solo word at most once:
// the value of a maximum-weight bipartite matching between `segments` and the
// solo words, where an edge weighs pair_log_bonus + word_log_bonus when the
// pair mask has the bit and word_log_bonus when only the word mask does.
// Both bonuses must be non-negative (decision 4).
double dfs_solo_exact_bonus(
    DfsSoloMasks const* segments, size_t count,
    double word_log_bonus, double pair_log_bonus);

// What the search already added: the sum over segments of each one's best
// available tier, ignoring conflicts. Always >= dfs_solo_exact_bonus().
double dfs_solo_optimistic_bonus(
    DfsSoloMasks const* segments, size_t count,
    double word_log_bonus, double pair_log_bonus);

// exact - optimistic, i.e. the (never positive) whole-answer correction.
// Returns 0 without solving when no two segments share a solo word.
double dfs_solo_correction(
    DfsSoloMasks const* segments, size_t count,
    double word_log_bonus, double pair_log_bonus);
```

### Tasks

- [ ] `dfs_resolve_solo_words()`: one `aggregate_entry_position(word)` per solo
      word (`source/index.h:105-108`), recording validity rather than failing.
      `source/coherence-measure.cpp:849-875` is the existing precedent for this
      exact probe pattern.
- [ ] `dfs_solo_masks()`:
    - trailing `"W S"` — `continuation_entry_position(*after_word, S, &out)`,
      skipped entirely when `after_word->continuation == IndexReader::Node(-1)`,
      the sentinel `walk()` already tests at `source/dfs-class-list.cpp:324`;
    - leading `"S W"` — `continuation_entry_position(solo.leading[i], W, &out)`,
      skipped when `!leading_valid[i]`;
    - `--pairs` — build `W + ' ' + S` in `*scratch` and probe once; both orders
      are already in the set (decision 2). A hit sets the bit in *both* masks.
- [ ] `dfs_solo_correction()`: OR-accumulate `word_mask`s; return 0 on no
      overlap. Otherwise solve.
- [ ] The solver: max-weight rectangular assignment over a `k x (m + k)` matrix
      where `k` is the number of segments with a nonzero `word_mask`, `m` is the
      solo count, and the `k` padding columns weigh 0 so any segment may go
      unassigned. Shortest-augmenting-path with potentials (the standard
      Hungarian/JV form), `O(k^2 (m + k))` — at most `32 * 32 * 48` and only on
      the conflict path.
- [ ] Register `dfs-solo.cpp` in `dfs_class_list_lib`
      (`source/meson.build:87-99`) and add a `dfs-solo` test entry alongside
      `dfs-class-list` / `dfs-output`.

### Verification

- [ ] `conan build .` succeeds.
- [ ] `meson test -C build dfs-solo --print-errorlogs` passes, covering:
    - a single segment with a nonempty mask gets its best tier;
    - disjoint masks sum with no correction;
    - the greedy-fails case — segments `{S0,S1}` and `{S0}` must both be paid,
      which a left-to-right greedy cannot do;
    - a tier-2 edge is passed over when using it elsewhere scores higher;
    - `optimistic >= exact` and `correction <= 0` on every case;
    - both bonuses 0 gives 0.
- [ ] `meson test -C build` otherwise unchanged.

### Completion gate

The module builds and its unit test passes; nothing else in the tree references
it. Commit as `Add solo-word pair probing and assignment`.

## Phase 2 — `--solo-words` in every scoring path [ ]

### Purpose

Land the option, the per-member masks, the ordering, and both CLIs' scoring
together. Partial landing is not an option: masks that the score sees and the
member ordering does not would stop member 0 from being the class best, breaking
the phase-2 bound and phase-3 monotonicity at once — the same constraint
`plans/pair-bonus.md` records for `--pair-bonus`.

### Files

- `source/dfs-cli-args.h`, `source/dfs-cli-args.cpp`
- `source/dfs-score.h`, `source/dfs-score.cpp`
- `source/dfs-class-list.h`, `source/dfs-class-list.cpp`
- `source/dfs-search.cpp`
- `source/dfs-output.cpp`
- `source/dfs-anagrams.cpp`, `source/query-index.cpp`
- `source/test-dfs-cli.sh`, `source/test-query-index.sh`

### Tasks — option surface

- [ ] `DFS_OPT_SOLO_WORDS = 304` and a
      `{ "solo-words", DFS_OPT_SOLO_WORDS, OPTPARSE_REQUIRED }` row in
      `DFS_COMMON_LONG_OPTIONS` (`source/dfs-cli-args.h:22-40`).
- [ ] `std::vector<std::string> solo_words` in `DfsCommonArgs`
      (`source/dfs-cli-args.h:44-57`).
- [ ] A file-local `parse_solo_words()` in `dfs-cli-args.cpp` reusing
      `clean_word()` (`source/dfs-cli-args.cpp:22-33`), so `--solo-words` and
      `--pairs` normalize identically. Errors, each fatal with exit 2:
    - `error: --solo-words: empty word`
    - `error: --solo-words: duplicate word "W"`
    - `error: --solo-words: at most 16 words`
- [ ] The `dfs_parse_common_option()` case sets `info.name = "--solo-words"` and
      `info.score_incompatible = false` — it is a term of the score `--score`
      computes, like `--pairs` and `--word-bonus`
      (`source/dfs-cli-args.cpp:270-310`).
- [ ] After the option loop in both CLIs, reject `--word-bonus < 0` or
      `--pair-bonus < 0` when `solo_words` is non-empty (decision 4):
      `error: --solo-words requires a non-negative --word-bonus/--pair-bonus`.
- [ ] `dfs_diagnostic("solo words: %s\n", ...)` with the cleaned words joined by
      `", "`, so cleanup and dedup are observable and testable.
- [ ] Usage text in both synopses and option lists (`source/dfs-anagrams.cpp:59,75`,
      `source/query-index.cpp:41,57`):

  ```text
  --solo-words WORD[,WORD...] treats each WORD as a candidate pair partner
    that costs no letters: a single-word entry forming a two-word index entry
    or --pairs entry with an unused WORD, in either order, earns --word-bonus,
    and earns --pair-bonus too when that two-word form is in --pairs; each
    WORD counts once per answer, and at most 16 are accepted
  ```

### Tasks — score model

- [ ] `DfsScoreModel::solo_log_bonus(uint16_t word_mask, uint16_t pair_mask)`:
      `pair_mask` nonzero gives `multi_word_log_bonus_ + pair_log_bonus_`,
      else `word_mask` nonzero gives `multi_word_log_bonus_`, else 0.
- [ ] Two defaulted mask parameters on `segment_log_score()`,
      `first_segment_log_score()`, and `append_segment_log_score()`
      (`source/dfs-score.h:33-39`), adding `solo_log_bonus()` to the result.
      Existing call sites compile untouched.
- [ ] Extend the class comment (`source/dfs-score.h:16-27`) to say a single-word
      segment can earn both bonuses through an unused solo partner, and that the
      per-segment value is the optimistic one.

### Tasks — phase 1

- [ ] `solo_word_mask` / `solo_pair_mask` on `IntermediateMember`
      (`source/dfs-class-list.cpp:31-38`, into existing tail padding),
      `DfsPackedMember` (`source/dfs-class-list.h:44-50`, 16 -> 24 bytes), and
      `DfsMemberView` (`source/dfs-class-list.h:89-95`). Update the size comments
      and the `static_assert` at `source/dfs-class-list.cpp:408`, and the copies
      at `source/dfs-class-list.cpp:479` and `:598-604`.
- [ ] `DfsClassList` takes a trailing `DfsSoloWords const* solo = NULL`
      (`source/dfs-class-list.h:141-146`) and forwards it to the extractor.
- [ ] `DfsExtractor::emit()` gains the trailing-space node so a trailing probe
      costs `|S|` steps instead of `|W| + |S|`: pass `choice.next` from the one
      call site (`source/dfs-class-list.cpp:321`) and build
      `EntryPosition{choice.next, choice.count}`. Gate the whole thing on
      `solo != NULL && word_count == 1`, mirroring how the pair probe is gated
      on `word_count > 1` (`source/dfs-class-list.cpp:266-273`) — `emit()` is
      phase 1's hottest path, and the option must cost nothing when unused.
      Use an extractor-owned scratch string for the `--pairs` key; restore
      `text` exactly as `walk()` left it.
- [ ] `MemberOrder::score()` (`source/dfs-class-list.cpp:356-374`) passes the
      masks. This is the load-bearing one: member 0 must stay the class best
      under the complete model.

### Tasks — phases 2 and 3

- [ ] `best_member_log_scores` passes member 0's masks
      (`source/dfs-search.cpp:206-213`). No `DfsAnagramSearch` signature change:
      the bonus magnitudes are unchanged (decision 6).
- [ ] `spelling_log_score()` passes masks on both sides of the delta
      (`source/dfs-output.cpp:80-83`), keeping the pending queue optimistic.
- [ ] `DfsTopN::emit()` (`source/dfs-output.cpp:155-176`): collect each chosen
      member's `DfsSoloMasks` in the loop that already builds the text, then set
      `spelling.log_score = current.log_score + dfs_solo_correction(...)`. Leave
      every `current.log_score` comparison — the queue order, the two floor
      breaks — alone, per decision 5. Add a comment saying exactly that, because
      the two scores now differ and the difference is deliberate.

### Tasks — CLIs

- [ ] `dfs-anagrams.cpp`: build a `DfsSoloWords` from `args.common.solo_words`
      after `IndexReader reader(fp)` and before the `DfsClassList`
      (`source/dfs-anagrams.cpp:294-302`), and pass it to the class list.
- [ ] `query-index.cpp` default mode: effective predicates next to `is_phrase` /
      `is_pair` (`source/query-index.cpp:184-190`) —
      `eff_pair(m) = m.known_pair || m.solo_pair_mask` and
      `eff_word(m) = m.word_count > 1 || m.solo_word_mask`. Substitute them in
      the partition (`:414-415`), `ScoreOrder` (`:196-207`), and `print_row`
      (`:398-404`). Still three groups, not four: `eff_pair` implies `eff_word`
      because `pair_mask` is a subset of `word_mask` (decision 1). The
      `word_bonus == 0.0 && pair_bonus == 0.0` fast paths (`:396-397`, `:407`)
      need no change — solo bonuses are those same two magnitudes.
      No matching is needed here: every printed row is one segment.
- [ ] `query-index --score` (`print_sequence_score()`,
      `source/query-index.cpp:243-285`): for each space-free entry call
      `dfs_solo_masks()` with `after_word = NULL`, chain the segments with those
      masks, then add `dfs_solo_correction()` once for the whole sequence.
      `--score` also needs the index open before scoring, which it already has.
- [ ] Both CLIs already zero `pair_bonus` when `--pairs` is absent
      (`source/dfs-anagrams.cpp:262-263`, `source/query-index.cpp:303`). That
      stays correct: tier 2 requires `--pairs` membership.

### Tasks — tests

The synthetic fixture needs no new entries. `ab`(10) / `cd`(7) / `dc`(2) /
`ab cd`(70) supply the index-backed case, and a `--pairs` file supplies
arbitrary extra edges (`source/make-dfs-test-index.cpp:19-23`).

- [ ] `source/test-query-index.sh`, against the synthetic index:
    - `--solo-words ab --word-bonus 1` on bag `cd`: `cd` prints `7e6` (the
      index has `ab cd`), `dc` prints `2` (neither order exists);
    - identity tripwire: `--solo-words ab` with no `--word-bonus` leaves stdout
      byte-identical to the run without it;
    - `--solo-words ab --pairs` listing `ab,cd` with `--word-bonus 1
      --pair-bonus 1`: `cd` earns both bonuses;
    - `--pairs` listing `ab,dc` alone gives `dc` both bonuses even though the
      index has no `ab dc`, and the index alone gives `cd` only the word bonus
      (decision 1, both arms);
    - `--score cd` and `--score cd,dc` agree with the listing and with each
      other under the same options;
    - `--solo-words` with an empty field, a 17-word list, a duplicate, and no
      argument each exit 2 with the right message;
    - `--solo-words` with `--word-bonus -1` exits 2.
- [ ] `source/test-dfs-cli.sh`, extending the `--word-bonus` / `--pairs` block
      (`source/test-dfs-cli.sh:199-260`):
    - one-use-per-answer: bag `cdcd -m 2 --solo-words ab --word-bonus 1` must
      score the two-segment answer `cd,cd` with **one** word bonus, not two —
      this is the assertion the whole exact-scoring path exists for;
    - the assignment case: `--solo-words ab,ba` with a `--pairs` file holding
      `ab,cd`, `ba,cd`, and `ab,dc` on bag `cdcd`, where `cd` can take either
      solo word and `dc` only `ab`, so a left-to-right greedy pays one bonus and
      the correct assignment pays two;
    - every result line round-trips through `query-index --score` with the same
      options, via the existing `assert_close` harness. The two paths compute
      the same value by different groupings, so the 0.06% tolerance is doing
      real work here, not just absorbing print rounding.
- [ ] Existing `--word-bonus` / `--pairs` assertions must pass unchanged: with
      no `--solo-words`, every mask is zero and every score is what it was.

### Verification

- [ ] `conan build .` succeeds (`werror=true`, so the struct-size and
      signature changes must be warning-clean).
- [ ] `meson test -C build` passes, including `dfs-cli-differential` and
      `dfs-search-14`.
- [ ] `./build/dfs-anagrams --help` and `./build/query-index --help` read
      correctly around `--solo-words`.
- [ ] Corpus check, with the host process table confirmed quiet first:

  ```bash
  ./build/query-index "$IDX" "${S6:0:14}" -w -n 20 --word-bonus 1
  ./build/query-index "$IDX" "${S6:0:14}" -w -n 20 --word-bonus 1 --solo-words <partner>
  ```

  The second must promote words that form real phrases with the partner and
  leave every other row's score identical.
- [ ] `./build/dfs-anagrams "$IDX" "${S6:0:16}" -n 10 --word-bonus 1 --solo-words a,b`
      produces no answer whose score implies a solo word used twice; spot-check
      by pasting a result's entry list into `query-index --score` with the same
      options and confirming agreement.
- [ ] Phase-1 timing with and without `--solo-words` on the same bag, to confirm
      the probe stays off the no-option path and is affordable on the option
      path. Compare `phase 1 complete` diagnostics.
- [ ] `git grep -n 'solo' source/` shows no leftover scaffolding.

### Completion gate

Both CLIs accept `--solo-words`, single-word segments earn the bonuses their
partners imply, no answer spends a solo word twice, and every pre-existing score
is unchanged when the option is absent. Commit as
`Score single words against --solo-words partners`.
