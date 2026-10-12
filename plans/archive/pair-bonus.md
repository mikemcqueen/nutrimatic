# Plan: repurpose `--pairs` as a pair-list file

## Outcome

`--pairs` stops being shorthand for `--max-extract-words 2` and becomes
`--pairs FILE`, a shared option on `dfs-anagrams` and `query-index` that loads a
list of word pairs into a lookup set.

The first two phases land the option, the file format, the loader, and the
lookup seam. Phase 3 applies `--pair-bonus` throughout extraction, class
ordering, phase-2 bounds, phase-3 spelling scores, and both `query-index`
scoring modes. `findings/bonus-issues.md` is the supporting analysis.

The scoring corrections `--pair-bonus` depended on have since landed — see
[Already landed](#already-landed). They change nothing in this plan's phases;
they change what the [Follow-up](#follow-up) after it has to do.

Anyone wanting the old two-word extraction cap passes `-x 2` explicitly.

## Scope

### Included

- `--pairs` takes a required file argument on both CLIs.
- A pair-file format and loader: one `word,word` line per pair, both word
  orders inserted, malformed lines fatal.
- A `DfsPairSet` type whose lookup accepts non-NUL-terminated text without
  allocating, so phase 1 can probe it from `emit()` later.
- Removal of the `-x 2` shorthand, its `--pairs`/`-x` reconciliation, and the
  now-empty `dfs_finalize_common_args()`.
- A diagnostic reporting what was loaded, so the loader is observable and
  testable in this commit.
- Test updates for the tests that assert the old shorthand.

### Deferred to phase 3

The first two phases deliberately leave the following work for phase 3, where
it lands together because partial implementation would break score ordering:

- `--pair-bonus`, and the third term in `DfsScoreModel`.
- The pair-membership flag on `DfsPackedMember` / `IntermediateMember`, and the
  set probe in `DfsExtractor::emit()`.
- Generalizing `DfsClassList`'s ordering input from one bonus magnitude to
  whatever makes a member's score.
- query-index's two-group partition/merge (`source/query-index.cpp:379-405`),
  which needs a third group once a second bonus exists, and its
  `word_bonus == 0.0` count fast path (`source/query-index.cpp:379`), which
  has to widen to "no bonus of any kind". Three groups and not four because
  pair membership implies multi-word (decision 3): single word, multi-word
  non-pair, multi-word pair. Substring matching would break that.

`findings/bonus-issues.md` is the reference for all of them. This plan's job is
to leave a clean seam, not to touch them.

### Already landed

`plans/bonus-fix.md` shipped as `Order class members by score`, which closes
two items this plan previously listed as excluded and changes the shape of the
remaining ones:

- Class members are sorted by score, not count. `DfsClassList`'s constructor
  takes a trailing `double multi_word_log_bonus` (`source/dfs-class-list.h:137`)
  and orders members by `log(count)` plus that bonus, so member 0 is the class's
  best member under the bonus in effect.
- `spelling_log_score()` computes its phase-3 delta in score space via
  `DfsScoreModel::segment_log_score()`, and `DfsTopN` holds a
  `DfsScoreModel const*` for it (`source/dfs-output.h:66`).
- The `bonus_reorders` per-class scan in `DfsAnagramSearch`'s constructor is
  gone; the phase-2 bound reads member 0 directly.

The consequence for `--pair-bonus` is a constraint, not just an opportunity:
member order and the score model must agree. A pair term that the score sees
and the ordering does not would leave member 0 no longer the class best, which
breaks the phase-2 bound and the expansion's monotonicity invariant at once.

## Decisions

1. **`--pairs` is score-compatible.**
   A pair list is a scoring input, never an extraction filter, so
   `dfs_parse_common_option()` sets `score_incompatible = false` for it, as it
   already does for `-P` and `--word-bonus`
   (`source/dfs-cli-args.cpp:224-237`).

   In this commit that means `query-index --score seq --pairs FILE` loads and
   reports the file and does not change the printed score. That gap is real and
   closes when `--pair-bonus` lands. It is preferred over marking the option
   incompatible now and flipping it one commit later.

2. **`std::unordered_set<std::string>`, owning its keys.**
   Same shape as `DfsDictionary` (`source/dfs-class-list.h:30`), and owning
   storage keeps the loader trivial: no arena, no exact `reserve()` pass, no
   lifetime coupling, and rehashing is invisible because nothing holds
   iterators or pointers into the set.

   The probe never needs heterogeneous lookup, so C++17 is not a constraint —
   both future probe sites already hold a `std::string`. See
   [Follow-up](#follow-up).

   The memory is the thing to watch, not the lookup. Nodes are individually
   allocated, keys over 15 characters exceed libstdc++'s SSO and heap-allocate
   again, and storing both word orders doubles all of it. That is noise for a
   ten-thousand-pair file and a few hundred MB for a million-pair one. If it
   ever bites, the lever is storing only the canonical order
   (`min(a,b) + " " + max(a,b)`) and canonicalizing at probe time — half the
   keys, one `memchr` and a compare per probe. Do not do this preemptively.

3. **Both orders, one key each.**
   `a,b` inserts `"a b"` and `"b a"`. `a,a` inserts `"a a"` once. Matching is
   whole-entry: the extractor's entry text is compared against a key in full,
   with no scan for pairs inside longer entries. Since every key holds exactly
   one space, only two-word entries can ever match, but no code needs to special
   case that.

4. **Malformed lines are fatal.**
   A line that does not yield exactly two fields is an error naming the file and
   the 1-based line number, and the CLI exits 1. Silent skipping would let a
   mangled list quietly do less than intended.

5. **Cleanup rules match `--dict`.**
   Lowercase; drop characters outside `a-z0-9`; skip lines containing `-`
   entirely. This is `load_dictionary()`'s behavior
   (`source/dfs-cli-args.cpp:144-173`), applied per comma-separated field. A
   skipped `-` line is not a malformed line. A field that cleans to empty is
   malformed.

6. **`dfs_finalize_common_args()` is deleted, not emptied.**
   Its only job is the `--pairs`/`-x` reconciliation
   (`source/dfs-cli-args.cpp:246-256`). With the shorthand gone it has no body.
   Remove it, its declaration (`source/dfs-cli-args.h:80`), `DFS_PAIRS_LIMIT`
   (`source/dfs-cli-args.h:19`), `pairs_given` (`source/dfs-cli-args.h:55`), and
   both call sites (`source/dfs-anagrams.cpp:200`,
   `source/query-index.cpp:134`).

7. **The loader reports pairs and keys.**
   `pair list: N pairs, K keys` on the diagnostic stream. `K` is the set size
   after both-order insertion and deduplication, so `K < 2N` is direct evidence
   that reversal and dedup both happened. Without a consumer in this commit,
   this line is what makes the loader testable end to end.

## Status and commit protocol

Use these status values in the phase headings:

```text
[ ] pending
[-] in progress
[x] complete
```

For each implementation phase:

1. Change the phase status to `[-]`.
2. Implement only that phase's scope.
3. Run the phase's build and smoke verification.
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
| 1 | Pair-set type and loader | [x] | `Add a pair-list file loader` |
| 2 | `--pairs FILE` on both CLIs | [x] | `Repurpose --pairs as a pair-list file` |
| 3 | `--pair-bonus` in every scoring path | [x] | `Apply pair bonuses to DFS scores` |

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

Phases 1 and 2 do not change search behavior. Phase 3 changes ranking but adds
no timing-sensitive mechanism, so no timing gate applies.

## Phase 1 — pair-set type and loader [x]

### Purpose

Land the format, the container, and the parser with their own tests, before any
CLI surface changes. Nothing calls the loader yet.

### Files

- `source/dfs-cli-args.h`
- `source/dfs-cli-args.cpp`
- `source/test-dfs-cli.sh` (new cases only; the shorthand cases move in Phase 2)

### Tasks

- [x] Add to `source/dfs-cli-args.h`:

  ```cpp
  // A pair list: every loaded pair as both "left right" and "right left".
  // Owns its keys; probes are same-type find() against a caller's std::string.
  typedef std::unordered_set<std::string> DfsPairSet;

  // Loads a newline-delimited list of "word,word" pairs, applying
  // load_dictionary()'s cleanup to each field, and inserts both word orders.
  // Lines containing '-' are skipped. Prints an error and returns false if the
  // file can't be opened or read, or if any surviving line does not hold
  // exactly two nonempty fields.
  bool load_pair_file(
      char const* path, DfsPairSet* pairs, size_t* pair_count);
  ```

- [x] Factor the per-field cleanup out of `load_dictionary()`
      (`source/dfs-cli-args.cpp:144-173`) into a file-local helper both loaders
      call. Do not change `load_dictionary()`'s behavior.
- [x] Implement `load_pair_file()`. Track a 1-based line number across every
      line read, including skipped ones, so diagnostics point at the real line.
- [x] Error text, one line each, all to `stderr`:
    - `error: can't open pair list "PATH"`
    - `error: can't read pair list "PATH"`
    - `error: pair list "PATH" line N: expected two comma-separated words`
- [x] Parse every line first, then `reserve(2 * pair_count)` and insert, so a
      large list does not rehash repeatedly during load. This needs no second
      pass over the file.

### Verification

- [x] `conan build .` succeeds.
- [x] `meson test -C build` passes unchanged.
- [x] A scratch check, not committed, confirming reversal and dedup: a file of
      `a,b` and `c,c` loads to three keys — `a b`, `b a`, `c c`.

### Completion gate

The loader builds, its diagnostics read correctly, and no existing test changed
behavior. Commit as:

```text
Add a pair-list file loader
```

## Phase 2 — `--pairs FILE` on both CLIs [x]

### Purpose

Repurpose the option, retire the shorthand and its reconciliation, and load the
file from both CLIs with an observable diagnostic.

### Files

- `source/dfs-cli-args.h`
- `source/dfs-cli-args.cpp`
- `source/dfs-anagrams.cpp`
- `source/query-index.cpp`
- `source/test-dfs-cli.sh`
- `source/test-query-index.sh`

### Tasks

- [x] Change the `pairs` row in `DFS_COMMON_LONG_OPTIONS`
      (`source/dfs-cli-args.h:36`) to `OPTPARSE_REQUIRED`.
- [x] Replace `bool pairs_given` in `DfsCommonArgs` with
      `char const* pair_file = NULL`, alongside `dictionary_file`
      (`source/dfs-cli-args.h:46`).
- [x] In `dfs_parse_common_option()`, set `out->pair_file = options->optarg`,
      `info.name = "--pairs"`, and `info.score_incompatible = false` per
      decision 1.
- [x] Delete `dfs_finalize_common_args()`, its declaration, `DFS_PAIRS_LIMIT`,
      and both call sites per decision 6.
- [x] In each CLI's `main()`, next to the existing `--dict` load
      (`source/dfs-anagrams.cpp:249-254`, `source/query-index.cpp:294-299`):
      declare a `DfsPairSet`, load it when `pair_file != NULL`, `return 1` on
      failure, and emit
      `dfs_diagnostic("pair list: %zu pairs, %zu keys\n", ...)`.
- [x] Load the pair file in `query-index`'s `--score` path too, before
      `print_sequence_score()`, so the option behaves identically in both modes.
- [x] Update both usage strings: `[--pairs FILE]` in the synopsis
      (`source/dfs-anagrams.cpp:59`, `source/query-index.cpp:41`), and replace
      the `--pairs is shorthand for --max-extract-words 2` line
      (`source/dfs-anagrams.cpp:75`, `source/query-index.cpp:57`) with:

  ```text
  --pairs FILE loads word pairs, one "word,word" line each, matched in
    either order; it has no effect until --pair-bonus
  ```

  Drop the trailing clause when `--pair-bonus` lands.
- [x] Delete the shorthand equivalence test (`source/test-dfs-cli.sh:183-192`)
      and the conflict-diagnostic test (`source/test-dfs-cli.sh:278-281`).
- [x] Delete the shorthand equivalence test
      (`source/test-query-index.sh:221-230`). The `-x 2` coverage immediately
      above it is unaffected and stays.
- [x] Add to `source/test-dfs-cli.sh`, against the existing synthetic index:
    - a two-line pair file, one line reversed relative to the other, loads and
      reports `2 pairs, 2 keys` — the reversed line's two insertions are the
      two the first line already made, so four insertions collapse to two;
    - a `word,word,word` line exits 1 with the line number in the message;
    - a missing path exits 1;
    - a `-` line is skipped and not counted;
    - `--pairs` with no argument is a usage error.
- [x] Add one `source/test-query-index.sh` case: `--pairs` on a valid file emits
      the diagnostic and leaves stdout byte-identical to the same run without
      it. This is the assertion that guards the seam — it must keep passing
      until `--pair-bonus` deliberately breaks it.

### Verification

- [x] `conan build .` succeeds.
- [x] `meson test -C build` passes.
- [x] `./build/dfs-anagrams --help 2>&1 | grep -A1 -- '--pairs'` reads correctly.
- [x] `./build/query-index "$IDX" abcdefgh -n 5` and the same run with
      `--pairs FILE` produce identical stdout.
- [x] `git grep -n 'DFS_PAIRS_LIMIT\|pairs_given\|dfs_finalize_common_args'`
      returns nothing.

### Completion gate

Both CLIs accept `--pairs FILE`, the shorthand is gone from source, usage, and
tests, and the pair set has no effect on any output. Commit as:

```text
Repurpose --pairs as a pair-list file
```

## Phase 3 — `--pair-bonus` in every scoring path [x]

The scoring corrections this phase needs have already landed (see
[Already landed](#already-landed)). The implementation uses these seams:

- `DfsCommonArgs::pair_file` and a loaded `DfsPairSet` in both `main()`s.
- `reserved` fields in both member structs (`source/dfs-class-list.h:41-47`,
  `source/dfs-class-list.cpp:30-37`) with room for a flag at no size cost.
- `DfsClassList`'s `multi_word_log_bonus` parameter, which is the ordering
  input to generalize. A pair bonus is a property of a member's text rather
  than of its `word_count`, so the parameter widens to whatever scores a
  member — the pair-membership flag plus both magnitudes — and `MemberOrder`
  (`source/dfs-class-list.cpp:346`) scores against that. Both production sites
  already build the model before the class list
  (`source/dfs-anagrams.cpp:287`, `source/query-index.cpp:308`), so there is no
  construction-order work left to do.
- `DfsTopN`'s `DfsScoreModel const*`. Once the model carries a pair term and a
  member's flag reaches `segment_log_score()`, phase 3's delta picks the term up
  with no change to `spelling_log_score()` itself, and phase 2's per-class bound
  follows from member 0 with no change to `DfsAnagramSearch`.
- Two probe sites that each already hold a `std::string`, so neither needs
  heterogeneous lookup and neither allocates.

  `DfsExtractor::emit()` (`source/dfs-class-list.cpp:253`) reads the entry out
  of its own `std::string text` member, which carries the trailing space
  `walk()` pushed at `source/dfs-class-list.cpp:311` — hence the
  `text.size() - 1`. Borrow the string rather than copying out of it:

  ```cpp
  text.pop_back();
  bool const known_pair = pairs != NULL && pairs->count(text) != 0;
  text.push_back(' ');
  ```

  Neither call touches capacity, so `text` is restored exactly as `walk()` left
  it and nothing reallocates. Gate the probe on `word_count > 1`: single words
  cannot match a two-word key, and they are the overwhelming majority of
  emissions on phase 1's hottest path.

  query-index's `--score` path probes `entries[i]` directly
  (`source/query-index.cpp:256-260`), which is already a `std::string`.

  A probe from `DfsPackedMember::text` would be the case that needs a
  transparent hasher and C++20, since packed text is not NUL terminated
  (`source/dfs-class-list.h:38-47`). No planned work does that.
- The query-index identical-stdout test, which is the tripwire that says the
  bonus started doing something.

### Completed work

- [x] Add shared `--pair-bonus N` parsing and a third additive term to
      `DfsScoreModel`.
- [x] Mark exact whole-entry pair membership during extraction without growing
      either packed member record.
- [x] Order class members with the complete score model so member 0 remains the
      phase-2 bound and phase-3 descendants remain non-improving.
- [x] Include pair membership in phase-3 spelling deltas.
- [x] Apply the bonus in `query-index --score` and ordinary extraction output.
- [x] Replace query-index's two-run merge with pair, other-phrase, and word
      runs, retaining up to `-n` candidates from each before merging.
- [x] Keep `--pairs FILE --pair-bonus 0` byte-identical to a run without a
      pair list.

### Verification

- [x] `conan build .` succeeds.
- [x] Focused `dfs-cli` and `query-index-cli` Meson tests pass.
- [x] A listed rare phrase is promoted over a higher-count single-word member
      of the same class without changing that single word's score.
- [x] `query-index --score` applies word and pair bonuses additively and leaves
      unlisted phrases unbonused.
