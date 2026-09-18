# `dfs-anagrams --no-repeat` and `--disable-repeats`

## Context

`dfs-anagrams` can return a result that uses the same index entry twice — the
`"ab ab"` case asserted at test-dfs-search.cpp:44. Nothing forbids it: the
entry-point tie-break at dfs-all-runner.cpp:176 uses `std::max`, so the class
just chosen is still a candidate at the next level.

A blanket ban throws away results that are fine. What a solver actually wants
is to name the word that keeps coming back. `--no-repeat` does that, and
`--disable-repeats` keeps the blunt instrument available.

Neither is on by default. A run that passes neither behaves exactly as today.

## The two options

`--no-repeat=VALUE`, repeatable. The value's shape selects what it constrains:

- **WORD** — no space. That word may appear at most once in a result, counted
  wherever it falls: as a single-word segment, inside a multi-word segment, or
  twice inside one multi-word segment. `--no-repeat=dog` rejects
  `"hot dog" + "dog house"`.
- **PAIR** — at least one space. That whole segment may appear at most once.
  Matching is whole-segment equality, so `--no-repeat="hot dog"` rejects
  `"hot dog" + "hot dog"` and leaves `"hot dog" + "dog house"` alone. Word
  order is literal; naming both orders takes two values.

A comma anywhere in a value is an error. `--no-repeat` never takes a list.

WORD and PAIR values are **independent counters**. Passing `--no-repeat=dog`
and `--no-repeat="hot dog"` applies both tests on their own terms; naming a
pair does not constrain the words inside it.

`--disable-repeats` is separate and takes no value: it is the WORD test
widened to every word there is, so no word may occur twice in a result. It
rejects `"hot dog"` beside `"dog house"` without being told which word to
watch, and, since it counts an occurrence wherever it falls, it rejects a
self-repeating entry like `"step by step"` on its own. Only whole words count,
so `"dog"` beside `"god"` or `"dogma"` is not a repeat. It composes with
`--no-repeat`; both tests run.

## Where it is enforced

In `DfsTopN::emit()` (dfs-output.cpp:399), before `dfs_build_spelling()`, via
the private `DfsTopN::admits()` (dfs-output.cpp:347).
**The traversal is not touched at all**, so a run without these options pays
nothing.

Phase 2 chooses anagram *classes*; the member that makes a class a segment is
not chosen until phase 3, and a class can hold both a one-word and a
multi-word spelling (`onset` / `on set`) — so no class-level test could decide
any of this correctly.

At the top of the expansion loop, `class_indexes` and
`current.member_indexes` are both in hand, and `class_list->member(ci, mi)`
yields the entry's `text`, `text_length` and `word_count`. All three tests read
from there; none needs the assembled spelling.

A rejected candidate is skipped, not pruned — still generate its successors.
`(0,0)` on a repeated class may be a repeat while its descendant `(0,1)` is
legal. Successor scores come from `spelling_upper_log_score()`
(dfs-output.cpp:159), which needs no built spelling, so a rejection costs only
the scan.

### PAIR values are a run-length test, not a set

Two invariants make that test cheap:

- `walk()` passes the chosen class index as the next `entry_point`
  (dfs-all-runner.cpp:122) and computes `start = std::max(begin, entry_point)`
  (dfs-all-runner.cpp:176); `walk_certified()` receives that same `start` and
  only moves forward. Class indexes are therefore **nondecreasing** along a
  path, so repeated classes form a contiguous run.
- The expansion canonicalization in `emit()` (the `canonical` test,
  dfs-output.cpp:441-470) already keeps only **nondecreasing member indexes**
  across a repeated class.

So the same segment can only land in **adjacent** positions holding **equal**
member indexes. A PAIR value is whole-segment equality, so the test is one
pass comparing each position with the one before it and, where the
`(class, member)` pair repeats, comparing that member's text against the named
pairs — no hash set, no per-node state.

### The word tests scan the whole result

A word can recur in two *different* segments, which the adjacency invariant
says nothing about. Both word-level tests therefore walk every position's
member text word by word.

- A named WORD is counted across every position. The loop is per named word on
  the outside and per position on the inside, so the running count is a local
  that dies with the word. The transposed form — one pass over positions
  carrying a counter per named word — is the same work but needs storage live
  across the scan, and `emit()` runs concurrently on several search threads,
  so that could not live on the borrowed policy.
- `--disable-repeats` has no named list to count against. It collects the
  result's words into a fixed stack array of `(text, length)` views and
  rejects the first one it has already seen. A result's words are disjoint
  parts of the bag and each holds at least one letter, so their number is
  bounded by `DFS_MAX_BAG_LETTERS` (dfs-class-list.h:32) and the quadratic
  scan inside that buffer is over the words of one result, not the corpus.

The named set is user-supplied and small, results are a handful of segments,
and each block is skipped when its option was not given. No precomputed
per-member masks, and nothing stored on the class list.

## Interface

dfs-anagrams.cpp, private option codes after `OPT_PTM = 261`:

```cpp
static int const OPT_NO_REPEAT = 262;
static int const OPT_DISABLE_REPEATS = 263;
  { "no-repeat", OPT_NO_REPEAT, OPTPARSE_REQUIRED },
  { "disable-repeats", OPT_DISABLE_REPEATS, OPTPARSE_NONE },
```

Usage gains `[--no-repeat WORD|PAIR]... [--disable-repeats]`.

Help text:

```
  --no-repeat WORD|PAIR limits one entry to a single use per result; may be
    repeated
    a value with no space is a word, and is counted wherever it falls,
    including inside a multi-word segment
    a value with a space is a whole segment, matched in the order written;
    naming the other order takes a second value
  --disable-repeats is the same test widened to every word there is: no word
    may occur twice in a result, so "hot dog" beside "dog house" is rejected
    it is a blunt instrument; the two occurrences need not be in different
    entries, so a self-repeating entry like "step by step" is rejected on its
    own
    only whole words count, so "dog" beside "god" or "dogma" is not a repeat
```

Values are normalized as the dictionary loader normalizes a line, except that
word boundaries are kept: lowercased, characters outside a-z/0-9 dropped,
internal runs of space collapsed to one. Errors, each naming the value, follow
this file's existing `error: ` prefix:

- `error: --no-repeat does not take a comma-separated list: %s`
- `error: --no-repeat value is empty after normalization: %s`

Repeated identical values collapse rather than erroring, matching how the pair
loaders treat a duplicate key.

`DfsTopN` gains one parameter carrying both, defaulted so every existing
construction is unchanged:

```cpp
  DfsTopN(DfsClassList const* classes, DfsScoreModel const* model,
          size_t limit, DfsSoloWords const* solo_words = NULL,
          bool retain_segment_bonuses = false,
          DfsRepeatPolicy const* repeats = NULL);
```

`DfsRepeatPolicy` holds the normalized word list, the normalized pair list and
the `--disable-repeats` bool, plus an `admits_everything()` test. It is a plain
value held by `Args` and borrowed by `DfsTopN`; the constructor stores NULL
when the policy is NULL or admits everything, so a single pointer test skips
all three tests.

### Name collision to avoid

`--solo-words` in this tool already means something unrelated: up to 16
external partner words that consume no letters (dfs-anagrams.cpp:134-141,
`DfsSoloWords`). Only `top-segments --solo-words` means "single-word segment".
Nothing here touches `DfsSoloWords`, and no option or value uses the word
`solo`.

## Known cost

`DfsTopN` publishes a score floor from its heap, and phase 2 prunes against it
(`should_prune()`, dfs-all-runner.cpp:37-59). Rejected candidates never enter
the heap, so an aggressive ban list fills it more slowly, holds the floor
lower, and weakens phase-2 pruning. The search is unchanged; it just gets less
help from the floor.

## Files

- `source/dfs-anagrams.cpp` — option codes, table rows, parse and normalize,
  usage and help, hold the policy in `Args` and pass it to `DfsTopN` at :498.
- `source/dfs-output.h` / `.cpp` — `DfsRepeatPolicy`, the constructor
  parameter, and `admits()` running the three tests in `emit()` ahead of
  `dfs_build_spelling()`.
- `source/test-dfs-cli.sh` — the smoke cases, which need real index entries
  and so belong with the CLI fixture rather than test-dfs-search.cpp.
- `findings/no-repeat.md` — the two monotonicity invariants, which are what
  make the PAIR adjacency test sufficient and are not visible at the emit
  site, and why the word tests cannot use that pass.
- `docs/detailed-workflow.md` — the two options where the generator's options
  are described.

`query-index` is unaffected: `DfsTopN` is constructed only by `dfs-anagrams`
and the tests.

## Verification

```sh
source ~/code/nutrimatic/.env/bin/activate
conan build .
source build/dep-info/conanbuild.sh
```

1. `pgrep -x query-index dfs-anagrams` on the host process table before any
   timed run.
2. The CLI fixture's `abab` run reaches `ab,ab`. It is present by default,
   absent under `--disable-repeats`, and absent under `--no-repeat=ab`.
3. A WORD value counts an occurrence inside a multi-word entry: under
   `--no-repeat=ab`, `abcdab` tops out at `ab cd,ba` rather than a result
   using `ab` twice.
4. A PAIR value rejects only the whole-segment repeat: `abcdabcd` under
   `--no-repeat='ab cd'` loses the doubled segment, and under
   `--no-repeat='cd ab'` keeps it, since the other word order names a
   different entry.
5. `--disable-repeats` rejects two entries sharing a word (`abcdab`), and
   rejects a self-repeating entry on its own — the index holds no such entry,
   so `--pairs` synthesizes `ab,ab` for that case and for the matching
   `--no-repeat=ab` case.
6. Two spellings of one class (`klmn` / `kl mn`, which share no word) still
   co-occur under `--disable-repeats`; this is the case a class-level test
   would wrongly reject.
7. A comma in a value errors, and so does a value that normalizes to nothing.
8. Run the existing suite. The `"ab ab"` assertions at test-dfs-search.cpp:44,
   :129 read a `CollectSolutions` sink, which sees class paths and is
   unaffected. The `DfsTopN` cases at :172-205 pass no policy and compare
   pruning modes against each other, so they are untouched. Three tests skip
   without `$IDX`.
9. Smoke only, per repo convention: one case per value shape, the
   self-repeating entry, the `klmn` non-repeat, and the two errors.

Timing: the traversal is not modified, so the exact-search benchmark with
`--require-completable` should be unchanged. Run it to confirm that.
