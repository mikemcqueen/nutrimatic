# Review: plans/short-word-pairs.md

Reviewed against `source/dfs-class-list.{h,cpp}`, `source/dfs-cli-args.{h,cpp}`,
`source/dfs-anagrams.cpp`, `source/query-index.cpp`, `source/dfs-solo-words.cpp`,
`source/dfs-search.cpp`, `source/dfs-all-runner.cpp`, and `source/test-dfs-cli.sh`.

## Status

All findings below were reviewed with the author and resolved into
`plans/short-word-pairs.md` on 2026-09-10. S5 was withdrawn on inspection of
the call sites. This file is the record of what was found and why; the plan is
the authority on what gets built.

## Verdict

The core argument is sound. Phase two divides remaining letters by
`min_word_length` in exactly two places -- `derived_max_depth()` in
`dfs-search.cpp` and the exact-`-g` remainder prune in `dfs-all-runner.cpp` --
so requiring every pair entry to hold at least `-m` non-space characters really
does preserve both the depth bound and the pruning. Nothing in phase two reads
`word_count`.

The two-gate diagnosis of why `new york` is unreachable today is correct: the
space edge is withheld by `word_len >= min_len`, and the continuation is
withheld by both `words + 1 < max_extract_words` and `letters_left >= min_len`.

The packed-spelling bound restated as `letters + effective_max_extract_words - 1`
is correct and still peaks at 255 under `-m 1`.

## Correctness gaps

### C1. The two-word cap could truncate ordinary traversal

Step 2 says to permit exceptional traversal only from an exact first-word prefix
and to never continue past the second word. Read together, that invites gating
on prefix membership. But an exception prefix can be an ordinary long word: with
`york,new` loaded under `-m 4`, `york` is an exception prefix, yet
`york city hall` must still extract normally under `-x 0`.

The cap belongs on how the boundary was crossed and on whether a short word is
involved, not on membership in `exception_prefixes`.

**Resolved.** The plan now defines *exceptional continuation* as a boundary
crossing the ordinary gates would have refused, and caps only that. Step 2 says
prefix membership alone must not cap an entry whose first word crossed under
the ordinary gates.

### C2. The capacity widening breaks plain `--words-only`

Step 2 derives the effective capacity as the normal bound, then the two-word
expansion when prefixes exist, then `-x`. The normal bound is already 1 when
`include_phrases` is false (`DfsExtractor`'s member initializer). Applying
`max(derived, 2)` on top of it re-enables phrase extraction in the one mode
Step 3 promises to leave alone.

The expansion must be conditioned on `include_phrases`, and Step 2 must say so
rather than leaving Step 3's prose to contradict it.

**Resolved.** Both the formula and the Step 2 bullet now condition the two-word
expansion on `include_phrases`.

### C3. One boolean is not enough state

The plan sets `requires_pair_match` when the first word is short. But `york,new`
has a long first word and is reached by ordinary continuation whenever the bag
is at least twice `-m`. Nothing in the described state tells the walker to run
the exact-key check that exposes the space after a short second word.

The rule that carries the weight is positional: at a second-word node whose
length is below `-m`, expose the space only if the accumulated text is an
oriented pair key. That rule stands on its own; the boolean is a separate
concern.

**Resolved.** The second-word space rule is now stated positionally and
independently of `requires_pair_match`, with the twelve-letter `york,new` case
as the worked example. The boolean's scope is narrowed to first-word emission
suppression and whole-entry admission.

### C4. `-x 1` is not gated in the walker

The validation list requires `-x 1` to prevent the exception, but the
exceptional-continuation rule omits any word-count check. The ordinary path has
one. It needs adding, and the exceptional space exposure should be gated on it
too, so a capped run does not widen the character set for a space it cannot use.

**Resolved.** The exceptional bullet now requires remaining capacity, and the
plan states that the word-count cap is the one gate an exception never
overrides and that it also governs space visibility.

## Blind spots

### B1. Existing solo-word pair edges regress silently

`DfsSoloWords::resolve_from_position()` probes only `candidate + " " + solo`.
Today `new,york` plus `--solo-words new` gives candidate `york` a pair edge,
because the set is symmetric. Under oriented insertion only `new york` is
stored, so the edge disappears and the user must rewrite the line as `york,new`.

The plan describes the new behavior but never labels it a break in existing
behavior.

**Resolved.** The direction section now labels this a behavior change and names
the rewrite users need. The Preserved-behavior bullet names the exception.

### B2. Raising `-m` turns a working pair file fatal

Validation is a hard error measured against the effective minimum. A file that
loads today at `-m 4` fails outright at `-m 5` if any entry totals four
characters. There is no skip-and-warn path, and the same file stays valid in
`--score`, which never validates. One file is therefore fatal in one mode and
fine in another.

**Resolved.** The hard error stands. The Rule section now states that a pair
file is specific to the minimum it was written for, and Command scope states
that validating in score mode is not evidence a file will load for extraction.

### B3. The out-of-scope perf argument does not follow

The cost of prefix gating is the second-word subtree opened under every makeable
exception prefix. Capping entries at two words does not shrink that subtree, it
only stops a third one. And `query-index` never sets `max_extract_words`, so it
runs at 0 (no limit); the justification cites only `dfs-anagrams`' `-x 2`.

The real bound is that the added work is one second-word walk per makeable short
prefix, which is usually small next to what ordinary traversal already does.

This matters because a real pair file can be prefix-heavy:
`~/code/words/cluelist.72.single.pairs` has 24,034 of 90,100 rows with a field
under four characters.

**Resolved.** The justification is replaced with the walk-count argument. The
`-x` non-sequitur and `query-index`'s uncapped default are called out.

### B4. Step 4 budgets no index fixture work

`source/make-dfs-test-index.cpp` has no long-then-short phrase, no phrase that
merely contains a listed pair, and no second entry sharing a first word. Several
validation bullets cannot be written against it. Adding entries perturbs the
`fghij` and `abcd` expectations that `test-dfs-cli.sh` already asserts.

The short-first-word direction *is* testable today: `f,gh` under `-m 2` with bag
`fgh` exercises the whole exceptional path.

**Resolved.** Step 4 now names what the fixture covers, what it does not, and
the disjoint-letter-range constraint that keeps the existing exact-output
assertions green.

## Smaller notes

### S1. Diagnostic fires when the exception is inert

The diagnostic switches on "an exceptional prefix was loaded", so it changes
wording under `-x 1` even though the exception is then inert.

**Resolved.** Both the invariant section and Step 3 now gate on the exception
being able to fire, not merely on a prefix having been loaded.

### S2. The error text reports the effective `-m`

`finalize_min_word_length()` may lower `min_word_len` below what the user typed
or defaulted to. The proposed message reports the lowered value.

**Resolved.** The Rule section now states that `MINIMUM` is the finalized value
and why that is the right one to report.

### S3. `--dict` precedence over an asserted pair is unargued

Letting `--dict` veto an explicitly asserted pair is stated under preserved
behavior but never justified. A user writing `new,york` plausibly expects it to
win.

**Resolved.** `--dict` stays authoritative. The Preserved-behavior bullet now
carries the reason and warns that a dictionary miss on a short first word makes
the exception silently inert.

### S4. The key-count diagnostic becomes `-m`-dependent

"Preserve the reported row/key counts" reads as stronger than what holds. The
existing `ab,cd`/`cd,ab` test survives only because both words satisfy `-m 2`.

**Resolved.** Step 1 now separates the diagnostic's format from its value and
states that a changed key count is the intended signal.

### S5. Constructor parameter placement

Adding the prefix set "adjacent to the existing pair-set input" pushes
`DfsClassList`'s constructor to eleven positional parameters and shifts two call
sites. Appending is cheaper unless grouping is worth the churn.

**Withdrawn.** Adjacent placement is correct. Appending after `exclude_pairs`
would force `query-index` to pass an explicit `NULL` for a parameter it does
not use. Both call sites need editing either way, and a mistake is a compile
error because `DfsSoloWords*` will not bind to `DfsPairSet const*`.

## Verified claim the plan asserted without proof

The widened capacity is inert for ordinary traversal. A bag smaller than twice
`-m` always fails the `letters_left >= min_len` gate after a first word of at
least `-m`, so no ordinary two-word entry becomes newly reachable. This is why
the widening is safe to apply globally rather than only under an exception
prefix.

**Resolved.** The proof is now written into the word-count bound section, along
with a note that the expansion's only other effects, on the per-depth choice
vector and the packed-spelling bound, are conservative.
