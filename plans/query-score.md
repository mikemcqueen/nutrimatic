# Plan: add `query-index --score`

## Goal

Add a scoring mode:

```text
query-index INPUT.index 'entry one,entry two,...' --score
```

In this mode, the second positional argument is an ordered, comma-separated
sequence of exact index entries. An entry may itself contain spaces. Validate
every entry against the index, then print the score produced by the same score
model used by the DFS anagram path.

Keep the scoring formula in one shared module. `query-index` should look up
counts and submit score terms; it should not reimplement restart or word-bonus
arithmetic.

## Proposed score definition

Treat each comma-separated item as one corpus segment:

- Its contribution is its exact index count.
- A multi-word index entry remains one segment, so its internal spaces do not
  incur restart penalties.
- Every comma boundary starts another segment and incurs exactly one restart.
- Repeated entries are valid and contribute repeatedly.

For entries `e[0] ... e[k-1]`, the default score is:

```text
product(count(e[i])) * (RESTART / corpus_total)^(k - 1)
```

With the existing `--word-bonus N`, add the shared multi-word bonus once for
each explicitly selected multi-word segment. Compute in natural-log space:

```text
sum(log(segment_score(e[i])))
    + (k - 1) * (log(RESTART) - log(corpus_total))
```

and convert to a linear score only for display. This matches
`DfsAnagramSearch`'s score arithmetic and avoids intermediate product
overflow/underflow.

## Implementation

### 1. Create one shared DFS score model

Add `source/dfs-score.h` and `source/dfs-score.cpp`.

Move the authoritative DFS scoring pieces there:

- the production `RESTART` constant;
- restart-rate construction from `restart` and `corpus_total`;
- the multi-word bonus calculation;
- a segment's log score from its index count and phrase status;
- appending a segment, including the restart except for the first segment;
- conversion from log score to the displayed linear score.

A small value object such as `DfsScoreModel` should be constructed from
`restart`, `corpus_total`, and `word_bonus`. Its API should make the two cases
explicit, for example:

```cpp
double first_segment_log_score(int64_t count, bool multi_word) const;
double append_segment_log_score(
    double accumulated, int64_t count, bool multi_word) const;
```

The exact names are implementation details. The invariant is that neither
`query-index` nor `DfsAnagramSearch` spells out the count/bonus/restart formula.

Refactor the DFS-family callers to use the model:

- `source/dfs-search.cpp` for representative class scores and restart
  transitions;
- `source/query-index.cpp` for ordinary single-entry ranking and the new
  sequence mode;
- `source/dfs-cli-args.{h,cpp}` no longer owns `RESTART` or
  `multi_word_bonus()`;
- `source/meson.build` builds the new source into `dfs-class-list_lib`.

This scope deliberately covers the DFS scoring used by `dfs-anagrams` and
`query-index`. The older `SearchDriver` in `find-anagrams` uses a float,
base-2, queue-integrated score representation and has no word-bonus concept;
folding it into this refactor is a separate, higher-risk change unless
explicitly requested.

### 2. Add exact index-entry lookup

Add a small exact-lookup helper, preferably on `IndexReader`, that:

1. begins at `reader.root()` with `reader.count()`;
2. follows one requested character at a time using a singleton
   `IndexReader::CharSet`;
3. follows a final `' '` because index entries are terminated by a space;
4. returns the terminating edge's count, or "not found".

The helper must distinguish an exact entry from a mere prefix. For example,
finding the path for `ab` is insufficient unless the `ab ` terminator exists.

Putting traversal in `IndexReader` keeps the encoded-trie details out of
`query-index`; if no other caller is expected, a file-local helper in
`query-index.cpp` is an acceptable smaller alternative.

### 3. Parse `--score` as a separate CLI mode

Add a no-argument `--score` long option and a mode flag to `Args`.

Branch before the normal letter-bag cleanup. In score mode:

- retain the second positional argument as the raw sequence rather than
  passing it to `clean_letters()`;
- split on commas;
- trim whitespace adjacent to each comma;
- reject an empty sequence or empty item;
- retain internal spaces because they identify multi-word index entries;
- reject malformed entry spelling consistently with the vocabulary accepted
  by `query-index` (lowercase `a-z`, digits, and single internal spaces);
- perform exact lookup for every item and fail the whole command before
  printing anything if any item is absent;
- derive phrase status/word count from the validated entry;
- feed the resulting count/phrase terms to the shared score model in sequence.

Do not build `DfsClassList` or run phase 1/phase 2 in this mode. Exact lookup is
proportional to the total input text length and requires no letter bag, class
list, cache, or worker threads.

Track whether mode-specific options were explicitly supplied. Reject
irrelevant search/filter options in score mode instead of accepting settings
that have no effect:

- `-u`, `--dict`, `-m`, `-n`, `-w`/`--words-only`;
- `--require-completable`;
- `-C`, `-T`, `-S`, `-d`, `-D`, and `-F`.

Keep `--word-bonus` meaningful and apply it from each selected entry's own
single-word/multi-word status.

### 4. Errors and output

Use a clear validation error naming the failed item, for example:

```text
error: index has no entry "missing phrase"
```

Treat malformed sequences and absent entries as invalid query input and return
status 2. Index open/read failures remain status 1.

Proposed successful output, matching `dfs-anagrams`' score formatting:

```text
2.147e-05 entry one,entry two
```

Use `%#.4g`. Preserve the user's sequence order in displayed text; scoring
separate segments is mathematically order-independent, but the input is still
a sequence.

### 5. Minimal smoke tests

Extend `source/test-query-index.sh` and reuse `make-dfs-test-index`:

1. One exact entry: `ab` scores its count (`10`) and pays no restart.
2. Two exact entries: `ab,cd` matches
   `10 * 7 * RESTART / corpus_total`.
3. One multi-word entry: `ab cd` scores its single index count (`70`) and pays
   no internal restart.
4. Contrast `ab cd` with `ab,cd` to prove phrase grouping and reset placement.
5. Three entries prove there are exactly two resets.
6. A repeated entry proves duplicates are accepted and scored twice.
7. A missing entry, an empty item (`ab,,cd`), and a prefix without an exact
   terminator each fail with status 2 and no stdout.
8. `ab, cd` is equivalent to `ab,cd`; malformed internal spacing is not
   silently normalized.
9. Verify `--word-bonus` applies exactly once to a multi-word segment and never
   to a single-word segment.
10. One incompatible option such as `--score -n 1` fails clearly, covering the
    mode-option guard without exhaustively testing every option.
11. Existing non-score `query-index` smoke tests remain unchanged.

Use the synthetic index for all arithmetic tests so the test does not depend
on `$IDX`. Compare floating output with `awk` and a tolerance rather than
encoding platform-specific last digits.

## Acceptance criteria

- Every requested segment must be an exact entry in the supplied index.
- A multi-word entry uses its own index count and pays no reset internally.
- A sequence of `k` entries pays exactly `k - 1` reset penalties.
- The new mode and `dfs-anagrams` obtain restart/count/bonus arithmetic from
  the same shared score model.
- The implementation performs no phase-1 extraction or phase-2 search.
- Existing `query-index` behavior without `--score` is unchanged.
- Smoke tests cover exact lookup, phrase-versus-comma semantics, reset count,
  missing entries, and relevant bonus behavior.

## Settled decisions

### 1. Successful output format

Print `%#.4g <original sequence>`, consistent with `dfs-anagrams` and
self-describing in interactive use.

### 2. Meaning of `--word-bonus`

There is an existing mismatch:

- ordinary `query-index` applies the bonus to each multi-word member;
- `dfs-anagrams` applies it to a class according to whether that class's
  highest-count representative is multi-word, then adjusts alternate spellings
  by count only.

For an explicit entry sequence, score each selected entry according to whether
that entry is multi-word. This matches ordinary `query-index`. It may not
exactly reproduce `dfs-anagrams` for an anagram class containing both single-
and multi-word spellings; changing that existing DFS class policy is out of
scope.

### 3. How broad "one scoring spot" should be

Centralize the DFS score model shared by `dfs-anagrams` and `query-index`,
leaving the legacy `SearchDriver` representation alone. Folding
`find-anagrams` into the same representation is out of scope because it would
require preserving its base-2 float priority behavior and queue invariants.

### 4. Input normalization

Trim spaces around commas but otherwise require exact, lowercase entry
spelling. Do not lowercase or collapse internal whitespace; silent
normalization can make the displayed sequence differ from the entry that was
actually scored.

### 5. Options in score mode

Reject explicitly supplied options that do not affect score mode rather than
silently ignoring them. `--word-bonus` is the only existing query/filter
option retained by score mode.

## Unknown-unknown audit

- The index stores an invisible trailing space terminator. Exact lookup must
  consume it or prefixes can be falsely accepted.
- A raw index can technically contain characters outside the restricted DFS
  vocabulary. This plan keeps `query-index`'s lowercase/digit/space domain;
  supporting arbitrary index bytes would require an escaping/delimiter policy,
  especially for commas.
- Linear scores can legitimately underflow to zero or overflow to infinity for
  long sequences or extreme bonuses even when the log score is finite.
  Log-space computation protects ranking/arithmetic, but the desired display
  behavior for those extremes should be decided if they matter.
- The formula for separately restarted segments is commutative. Calling the
  input a sequence preserves user intent and output order, but reordering
  comma-separated segments does not change their score.
- Existing `dfs-anagrams` output scoring is natural-log `double`; legacy
  `find-anagrams` reconstructs scores from a base-2 `float` queue priority and
  can differ in the last displayed digit. The acceptance oracle should be the
  DFS model, not `SearchDriver`.
- Comma becomes reserved syntax in score mode. An exact index entry containing
  a comma cannot be expressed without adding quoting/escaping, which this plan
  intentionally does not add.
