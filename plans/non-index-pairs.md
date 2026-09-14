# Plan: make external pairs available to `dfs-anagrams`

## Status

Design only. No production implementation has been authorized or started.

## Outcome

`dfs-anagrams` can use a positive external pair even when that two-word entry
does not exist in the selected Nutrimatic index. This applies to every positive
pair source:

- legacy `--pairs`;
- `--seed-pairs`;
- `--yes-pairs`; and
- `--best-pairs`, including workflow-inferred files.

An otherwise eligible missing pair becomes a synthetic two-word index entry
with corpus count `1` and its normal pair-bonus tier. Existing indexed entries
keep their index counts.

`query-index` remains index-only. Neither its ordinary output nor `--score`
may report or score an entry absent from the index.

## Motivation

Positive pair files currently affect only entries reached while walking the
index trie. They can grant a short-word extraction exception and can change an
indexed entry's score, but they cannot create an entry. Consequently, an
explicitly promoted pair such as `hobbit,home` has no effect when the index has
no `hobbit home` or `home hobbit` entry.

This contradicts the purpose of the external evidence. Seed, classified-YES,
and target BEST inputs assert that a pair is useful. The index remains the
source of corpus counts for observed phrases, but failure to observe the
phrase must not make that external assertion unusable in `dfs-anagrams`.

The count is not an implementation blocker. Pair scoring already applies a
large source-specific bonus. A fallback count of `1` is conservative,
deterministic, and does not claim an unobserved corpus occurrence greater than
the minimum representable positive count.

## Concrete S3 evidence

The current S3 inputs were inspected with its sentence bag, workflow
dictionary, and global NO exclusions:

- 322 positive pair identities are eligible for the target;
- 321 identities have at least one indexed orientation;
- 643 oriented entries are eligible;
- 454 orientations are indexed and 189 are absent; and
- `hobbit home` and `home hobbit` are the only identity for which both
  orientations are absent.

The eligible external orientations make only small contributions to anagram
classes for this target: the largest external contribution is six members,
well below the existing 255-member class limit.

After subtracting the letters in `hobbit home`, the remaining bag has valid
two-segment completions. One is `newton flew,pour`. Therefore admitting the
missing pair can produce valid three-segment S3 answers; it is not merely an
isolated segment that can never complete the target.

## Required semantics

### External pair identity and direction

Use the normalized keys already produced by the extraction pair loaders.
Do not introduce another normalization policy:

- a pair whose two words both meet `-m` has both orientations;
- a pair containing a short word retains only the written orientation;
- duplicate or reversed weighted inputs retain the strongest tier; and
- standalone entries accepted by positive pair files are not synthesized.

Thus `hobbit,home` under `-m 4` can synthesize both `hobbit home` and
`home hobbit` when those orientations are absent. A directional short-word
entry such as `new,york` under `-m 4` can synthesize only `new york`.

### Eligibility

A missing oriented pair is synthetic only when it would otherwise be a valid
`dfs-anagrams` phase-one entry:

1. Phrase extraction is active and the effective `-x` permits two words.
2. The normalized pair has already passed the extraction loader's total-length
   validation for `-m`.
3. Its combined letter multiset fits within the current input bag.
4. When a dictionary is active, both component words occur in it.
5. It is absent from every effective exclusion source.

These conditions preserve existing behavior:

- `-x 1` continues to forbid every two-word entry.
- `--dict` continues to validate phrase components independently.
- Global and target-local NO, plus explicit `--exclude-pairs`, continue to win
  over every positive source.
- An unspellable positive pair remains irrelevant to the current target.

### Index presence and count

For each eligible oriented key, use `IndexReader::aggregate_entry_count()` to
test index presence. This is the same aggregate trailing-space count used by
phase-one extraction and `query-index --score`; do not use the exact residual
count.

- If the lookup succeeds, do nothing. The normal trie walk emits the entry
  with its real aggregate index count.
- If it fails, emit one synthetic member with count `1`, two words, the
  normalized spelling, and the source's existing score flags.

Do not replace an indexed count with `1`, and do not emit both a normal and a
synthetic copy of the same oriented spelling.

### Scoring and search

A synthetic member is otherwise an ordinary `DfsPackedMember`. Its score is:

```text
count 1
* the normal multi-word bonus
* the legacy, seed, YES, or BEST pair bonus
```

Once inserted into its letter-signature class, all existing machinery must
handle it without a synthetic-only scoring path:

- member ordering within the class;
- phase-two score bounds and exact-segment search;
- phase-three spelling expansion;
- displayed result scores; and
- `--segments` reporting.

With `-g 3`, a synthetic two-word pair is one segment, exactly like an indexed
two-word phrase.

### Query-index boundary

Do not change `query-index.cpp`, its CLI contract, or its output:

- ordinary `query-index` extraction shows only index-backed entries;
- `query-index --score` continues to fail when any supplied entry is absent
  from the index; and
- `query-index --csv` never includes synthetic entries.

The shared class-list implementation must therefore default to index-only
behavior. Only `dfs-anagrams` may opt into synthetic external pairs.

## Implementation design

### 1. Add an explicit class-list policy

Add a named policy to `source/dfs-class-list.h`, for example:

```cpp
enum DfsExternalPairPolicy {
  DFS_EXTERNAL_PAIRS_INDEX_ONLY,
  DFS_EXTERNAL_PAIRS_SYNTHESIZE_MISSING,
};
```

Add it as the final `DfsClassList` constructor argument with an index-only
default. `dfs-anagrams` passes `DFS_EXTERNAL_PAIRS_SYNTHESIZE_MISSING`.
Existing `query-index` construction remains unchanged and therefore uses the
index-only default.

A named policy is preferable to an unexplained boolean at the end of the
already long constructor call.

### 2. Screen positive keys inside `DfsExtractor`

Keep the work inside `DfsClassList`/`DfsExtractor`, where the index reader,
input bag, dictionary, extraction capacity, exclusions, and pair maps are all
available. The generic file loaders should continue to parse and normalize;
they should not acquire index or target-specific responsibilities.

After the ordinary trie walk has restored its traversal state, iterate the
legacy pair set or weighted pair map when synthesis is enabled. The CLI already
rejects combining legacy and weighted positive inputs, but the helper should
still avoid duplicate keys defensively.

Screen bag, dictionary, `-x`, and exclusion eligibility before probing the
index. This avoids hundreds of thousands of unnecessary trie lookups for
global or sentence-wide pairs that cannot fit the current target.

### 3. Append missing entries through the normal member path

Refactor the existing phase-one emission code only as needed to share its
arena allocation, signature interning, class-size accounting, text storage,
and score-flag construction.

For a synthetic phrase:

- calculate its mixed-radix signature directly from its letters;
- copy its normalized space-delimited spelling into the text arena;
- store count `1`, word count `2`, and the appropriate pair tier flags; and
- intern it into the same signature table as trie-extracted entries.

Synthetic phrases do not need an index continuation and must not participate
in single-word solo-profile registration.

The existing sort and dedup pass then groups and orders them with normal
members. Retain the current explicit failure if any anagram class exceeds its
member-count representation; do not silently discard external spellings.

### 4. Add an observable diagnostic

Expose the number of synthesized oriented entries from `DfsClassList` and add
one concise `dfs-anagrams` stderr diagnostic after phase one:

```text
phase 1 external pairs: N synthetic entries
```

Print it when synthesis is enabled, including when `N` is zero. Count oriented
entries because those are the actual phase-one members being added.

Do not add this diagnostic to `query-index`.

### 5. Update dfs-anagrams help

Document under the positive pair options that `dfs-anagrams` admits an
otherwise eligible listed pair missing from the index with fallback corpus
count `1`. State that dictionary, bag, minimum-length, extraction-word, and
exclusion rules still apply.

No `query-index` help text changes are in scope.

## Minimal validation

Keep coverage focused in the existing `dfs-cli` fixture:

1. Build a tiny index containing both component words but not their aggregate
   phrase.
2. Confirm the phrase is absent without a positive pair source.
3. Supply it through legacy `--pairs` and confirm both eligible orientations
   can appear with count `1` and the legacy bonus.
4. Supply the same absent pair through weighted sources and confirm the
   strongest duplicate tier controls its score.
5. Confirm an exclusion suppresses the synthetic pair.
6. Confirm `-x 1`, an insufficient bag, and a dictionary miss do not synthesize
   it. Combine these checks where practical rather than creating a broad test
   matrix.
7. Confirm an indexed listed pair retains its real count and is not duplicated.

Compile both callers because the shared class-list interface changes, but do
not edit `query-index` or its tests. Run its existing focused test unchanged to
confirm the index-only default remains intact.

Suggested validation:

```bash
source ./setup.sh
source .env/bin/activate
conan build .
source build/dep-info/conanbuild.sh
meson compile -C build dfs-anagrams query-index
meson test -C build dfs-cli query-index-cli --print-errorlogs
git diff --check
```

For a final S3 smoke check, run a bounded `dfs-anagrams` invocation using the
same workflow, target, and `best.pairs`, and verify that stderr reports the
expected synthetic-entry count and that retained output can contain
`hobbit home`. This is a functional check, not a timing benchmark.

Before any timing comparison, follow `AGENTS.md` and check the host process
table for both `query-index` and `dfs-anagrams`.

Review the complete diff with `/review` before any commit, as required by
`AGENTS.md`.

## Expected files

Implementation should normally be limited to:

- `source/dfs-class-list.h`;
- `source/dfs-class-list.cpp`;
- `source/dfs-anagrams.cpp`; and
- `source/test-dfs-cli.sh`.

Do not modify `source/query-index.cpp` or `source/test-query-index.sh`.

## Explicitly out of scope

- Synthesizing standalone positive-list entries.
- Synthesizing arbitrary phrases not named by a positive pair source.
- Changing pair normalization or short-word directionality.
- Overriding `-m`, `-x`, dictionary, bag, or exclusion rules.
- Estimating an absent phrase's count from its component word counts.
- Changing the fixed seed, YES, or BEST bonus tiers.
- Changing `query-index` behavior, source, help, or tests.
- Rebuilding or changing the workflow index.
