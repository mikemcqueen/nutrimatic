# Plan: allow short-word exceptions for known pairs

## Outcome

During ordinary extraction, `dfs-anagrams --pairs` and `query-index --pairs`
can admit an exact one- or two-word index entry containing a word shorter than
`-m`, without weakening the minimum-length invariant used by phase two.
Short-word pairs are directional, ordinary pairs remain symmetric, and
`query-index --score` retains its current minimum-independent symmetric pair
matching.

The implementation described below is complete.

## Scope

### Included

- An opt-in, minimum-aware pair-loading policy for ordinary extraction.
- Exact oriented pair admission in the shared `DfsClassList` extractor.
- The corresponding extraction-capacity and packed-spelling bounds.
- Consistent ordinary extraction in `dfs-anagrams` and `query-index`.
- Directional `--solo-words` pair edges when a listed pair contains a short
  word.
- CLI diagnostics and detailed `--pairs` help for the new behavior.
- Focused smoke regressions for the affected loader, extractor, and CLI paths.

### Excluded

- Any change to `query-index --score` pair semantics.
- Any change to `query-index --near`, which does not accept `--pairs`.
- Any change to `--exclude-pairs` or other strict rejection-list callers.
- General prefix-trie optimization for exceptional pairs; the exact-prefix
  gate described below is sufficient for this implementation.
- More than two fields in the accepted `--pairs` syntax.

## Current behavior

`dfs-anagrams -m N` applies the minimum strictly to every literal
space-delimited word in a normalized index entry. During phase-one extraction,
the trie walker exposes a space edge only after the current word has reached
`N` characters. Consequently, an entry containing any shorter word is not
extracted.

For example, with `-m 4`, the indexed phrase `new york` is unavailable because
the extractor refuses the space after `new`. It never reaches `york` and
therefore never gets an opportunity to recognize or score the complete phrase.

## Rule

Use `--pairs` to grant exact-entry exceptions to `-m` during ordinary
extraction in both `dfs-anagrams` and `query-index`:

> An index entry is extractable when it contains at most `-x N` words and
> either every word is at least `-m N` characters long, or the complete
> normalized entry appears in the oriented `--pairs` set.

Every normalized `--pairs` entry must also contain at least `-m N` non-space
characters in total. Validate this after cleaning the one or two input fields;
the comma and joining space do not count. An entry that fails the constraint
should make the command fail rather than be silently ignored.

With `-m 4`:

- `be,an` is valid: the two words contain four characters in total.
- `be,a` is invalid: the two words contain only three characters in total.
- A standalone pair-file word must itself contain at least four characters.

This total-length constraint is part of pair-file validation, not just the
condition for receiving an extraction exception.

Report a failure with the existing pair-list path and line-number context:

```text
error: pair list "PATH" line LINE: normalized entry has COUNT non-space characters, fewer than -m MINIMUM
```

`MINIMUM` is the finalized minimum, which `finalize_min_word_length()` may have
lowered to the bag length when `-m` was not given explicitly. That is the
minimum extraction will apply, so it is the right value to validate and report,
but it can name a number the user never typed. Load pairs after the minimum is
finalized, and let the message stand on the effective value rather than the
requested one.

This remains a pair-file loading failure and therefore uses the command's
existing exit status for an invalid pair list.

The constraint makes a pair file specific to the minimum it was written for. A
file that loads at `-m 4` fails at `-m 5` if any single entry totals four
characters, and the failure aborts the run rather than dropping the row. This
is deliberate: silently ignoring an entry the user wrote is the worse outcome,
and pair files are already named per minimum. It does mean a large shared file
must be revalidated when the minimum rises.

Normal entries remain eligible. `--pairs` grants narrow exceptions; it does
not become an allowlist for the whole search.

## Direction of short-word pairs

Pair orientation depends on whether the normalized pair needs a short-word
exception:

- If both words satisfy `-m`, preserve the existing behavior and insert both
  orientations into the pair set.
- If either word is shorter than `-m`, insert only the orientation written in
  the pair file.

Therefore, with `-m 4`, `new,york` admits and scores `new york` but does not
admit or score `york new`. A separately written `york,new` line would admit
only `york new`.

This directional behavior also naturally applies to asserted `--solo-words`
pair edges: a pair-file entry grants only the candidate/solo ordering actually
present in the oriented in-memory pair set. The candidate must still be an
independently extractable single-word entry and therefore must satisfy `-m`.
The asserted pair does not make a short candidate extractable: with `-m 4`,
`york,new` can give candidate `york` the external solo partner `new`, while
`new,york` cannot produce `new (york)`. Solo words consume no letters from the
bag, so admitting the latter would also violate the phase-two minimum-entry
invariant.

This is a behavior change, not a preserved one. A pair line whose words both
satisfy `-m` keeps both solo orderings, but a line containing a short word
previously granted a solo pair edge in either order and now grants only the
written one. A user relying on `new,york` to give candidate `york` the solo
partner `new` must rewrite the line as `york,new`. `DfsSoloWords` probes only
the candidate-then-solo key, so nothing diagnoses the loss.

## Command scope

The extraction exception applies in both:

- `dfs-anagrams --pairs`; and
- ordinary `query-index --pairs` extraction.

Both commands construct `DfsClassList`, so putting the behavior in the shared
extractor is simpler and keeps the two extraction paths consistent.

Preserve `query-index --words-only` behavior. Plain `--words-only` does not
extract phrases, so a pair-file exception must not make a phrase available in
that mode. With `--words-only --require-completable`, the command already
extracts phrases as internal completion classes and filters them only from
displayed output; short-pair exceptions may participate in that internal
completion search but must remain absent from words-only output.

`query-index --score` is different. It scores an explicitly supplied sequence,
does not perform minimum-length extraction, and rejects `-m`. It should retain
the existing pair-file validation and symmetric pair matching rather than
inventing an implicit minimum-length threshold for score mode. Near mode
already rejects `--pairs` and does not load the file.

Make this a deliberate pair-loading policy boundary. Minimum-aware total-length
validation, directional insertion, and construction of `exception_prefixes`
belong only to ordinary extraction. `query-index --score` must use the existing
minimum-independent, symmetric policy. Select the policy after the command mode
is known rather than loading one representation before the score/extraction
branch.

Because validation belongs only to the extraction policy, a file that aborts
`dfs-anagrams` under a given `-m` still loads cleanly under
`query-index --score`, which has no minimum to check against. Validating in
score mode is therefore not evidence that a file will load for extraction.

Strict `--exclude-pairs` inputs are not changed by this feature. They remain
pair-only, symmetric, and independent of `-m`. Other strict rejection-list
callers of the shared pair-file loader must retain that behavior as well; the
extraction policy must be opt-in rather than a change to the loader's defaults.

Update both commands' `--pairs` help to state that, during ordinary extraction,
an entry with either word shorter than `-m` is matched only in the written
order, other pairs are matched in either order, and every loaded entry must
contain at least `-m` normalized non-space characters in total. The
`query-index` help must additionally state that `--score` retains symmetric
matching and does not apply the extraction minimum.

## Extraction design

The exception cannot be implemented solely as a final check in `emit()`. The
current walker prunes a short word at its terminating space and its extraction
word bound assumes every word consumes at least `-m` letters.

Pair loading for extraction should produce two related sets:

1. The oriented complete-entry pair set used for admission and scoring.
2. An `exception_prefixes` set containing the first word of every explicitly
   ordered pair in which either component is shorter than `-m`.

The second set deliberately includes the first word even when that first word
is not itself short. For example, `york,new` needs `york` as an exception prefix
because an exact seven-letter bag under `-m 4` has only three letters left after
`york`; ordinary continuation would otherwise stop before reaching `new`.
Calling this set `exception_prefixes` or `short_pair_prefixes` is therefore more
accurate than `short_word_prefixes`.

At the first word boundary, the walker should:

- proceed normally when the first word satisfies `-m` and enough letters
  remain for another ordinary word; or
- proceed exceptionally when the first word is in `exception_prefixes` and the
  effective extraction capacity still permits another word, even if the first
  word is short or fewer than `-m` letters remain.

The word-count cap is the one gate an exception never overrides. It also
governs visibility: when the capacity permits no second word, do not expose the
short first word's terminating space at all, since the walker would neither
emit it nor follow it and would only widen the allowed character set the
`reader->children()` call scans. This is what makes `-x 1` prevent the
exception.

Treat boundary visibility, entry emission, and continuation as separate
decisions. In particular, exposing the terminating space of a short first word
must permit traversal only: it must not emit that first word as a standalone
entry. A first-word entry remains emittable only when it independently
satisfies `-m`; membership in `exception_prefixes` grants no standalone
exception.

The recursive state needs one boolean such as `requires_pair_match`, set when
the first word is short. It governs first-word emission suppression and the
whole-entry admission check; second-word space visibility is governed by the
positional rule below. The existing `word_len` continues to describe the
current word, so no minimum-word-length-so-far value is needed.

At the second word boundary, admit the complete entry when:

```text
not requires_pair_match and second_word_length >= min_word_length
or pairs.contains(complete_entry)
```

Equivalently, exact pair membership is mandatory when either the first or
second word is short.

The walker must make that decision early enough to expose the second word's
terminating space. When the second word is short, request the space edge only
if the text accumulated so far is an exact oriented pair key; a check performed
only after the current `reader->children()` call cannot recover a space edge
that was omitted from its allowed character set.

This rule is positional, not a consequence of `requires_pair_match`. At any
second-word node whose current word is shorter than `-m`, expose the
terminating space when the accumulated text is an exact oriented pair key,
regardless of how the first-word boundary was crossed. A long first word in a
bag large enough to satisfy the ordinary gates reaches its short partner this
way and no other: `york,new` under `-m 4` with a twelve-letter bag crosses the
first boundary ordinarily, so no exceptional continuation and no
`requires_pair_match` is involved. The lookup is safe to skip entirely when
`exception_prefixes` is empty, because an empty set means no loaded pair has a
short component and no such key can exist.

Do not use prefix membership alone to require a final pair match. A prefix can
be shared. If `york` is in `exception_prefixes` because `york,new` is listed,
the unrelated entry `york city` must still be admitted normally because both
of its words satisfy `-m 4`.

An *exceptional continuation* is a first-word boundary crossing that the
ordinary gates would have refused, because the first word is shorter than `-m`
or because fewer than `-m` letters remain. Only that crossing is capped at one
further word, and only an entry containing a word shorter than `-m` must equal
a complete oriented pair key. A first word that satisfies `-m` and leaves `-m`
letters behind continues under the ordinary rules whether or not it is an
exception prefix, so an unrelated entry below a listed first word keeps its
full word budget. Accepted `--pairs` syntax contains at most two words, so a
longer entry merely containing a listed pair remains ineligible under the
exception.

## Extraction word-count bound and `-x`

`DfsExtractor` currently derives a maximum entry word count from:

```text
letters / min_word_length
```

and then lets `-x` tighten that bound. For the seven letters in `new york` and
`-m 4`, the derived bound is one word, so merely opening the space after `new`
would still not allow traversal into `york`.

Keep the normal derived bound unchanged when no valid short-word exception is
loaded. When `exception_prefixes` is nonempty and phrases are being extracted,
the derived extraction capacity must additionally permit a two-word entry:

```text
derived_words = max(letters / min_word_length, 2)
```

The normal derived bound is already 1 when phrase extraction is off, so the
expansion must be conditioned on that flag rather than applied to the quotient
alone. Plain `query-index --words-only` therefore keeps its one-word capacity
and cannot see an exceptional phrase.

An explicit positive `-x` then tightens that result as it does today. Thus
`-x 1` still prevents the exception, while `-x 2` permits it. `-x 0` retains
its documented meaning of no additional user-requested limit.

The widening cannot admit an ordinary two-word entry. It changes the derived
bound only when `letters / min_word_length` is below 2, which means the bag
holds fewer than `2 * min_word_length` letters. Ordinary continuation past the
first word needs all three of the walker's gates: the terminating space appears
only once the first word has consumed at least the minimum, the word count must
leave room, and the remaining-letter count must still be at least the minimum.
After a first word of `w >= min_word_length` letters, what remains is
`letters - w`, at most `letters - min_word_length`, which is below the minimum
whenever the bag is smaller than twice it. The third gate therefore fails for
every ordinary first word, and the new capacity is reachable only through an
exceptional continuation. This is why the expansion is safe to apply to the
derived bound globally rather than only below a listed prefix.

The expansion's only other effects are conservative. It enlarges the per-depth
choice vector and raises the packed-spelling bound, and both merely reserve
more room.

This widening also changes the local proof for packed spelling size. The
existing bound `letters + letters / min_word_length - 1` omits the possible
second word when that quotient is one. Express the bound using the effective
extraction capacity after the exception expansion and `-x` tightening:

```text
letters + effective_max_extract_words - 1
```

Equivalently, before `-x` tightens it, the exceptional term uses
`max(letters / min_word_length, 2)`. The 128-letter global limit remains safe:
the worst case is still `-m 1`, producing at most 255 stored bytes. Update the
corresponding `DfsClassList` constructor comments when implementing so they no
longer say the bag and minimum alone always imply the phrase capacity.

The wider capacity does not justify exploring arbitrary short words. The trie
walker takes the exceptional continuation only for an exact first-word hit in
`exception_prefixes`, keeping the additional phase-one work local to pair-file
entries that actually need it.

Strictly speaking, prefix gating can open every makeable second-word
continuation below a listed first word before exact pair membership rejects
unrelated entries at their terminating boundary. Further constraining that
walk with full exceptional-entry prefixes is out of scope. The added work is
one second-word walk per exception prefix the bag can make, and ordinary
traversal already performs one such walk per makeable first word that satisfies
`-m`, which is normally the larger number. The exception dominates only when
the bag is smaller than twice `-m`, where the derived capacity was 1 and no
ordinary second word existed, and those bags are cheap in absolute terms. Note
that `-x` does not help here: it bounds word count, not subtree breadth, and
`query-index` leaves it at 0 rather than the `-x 2` `dfs-anagrams` defaults to.
This can be revisited if phase-one node counts show a material regression for
large bags or prefix-heavy pair files.

## Phase-two invariant

Phase two uses `-m` as a lower bound on the letters consumed by every selected
index entry. That bounds search depth and supports pruning for an exact `-g`
segment count.

Requiring every normalized pair entry to contain at least `-m` non-space
characters preserves this invariant:

- an ordinary entry consumes at least `-m` letters because its first word does;
- an exceptional entry consumes at least `-m` letters because pair-file
  validation requires that total.

Phase-two depth, score-bound construction, and exact-`-g` remainder pruning
therefore do not need a weaker minimum or a separate actual-minimum-entry
calculation.

The startup diagnostic that derives an "at most N words" statement from
`letters / -m` becomes inaccurate: `be an` has two words in four letters under
`-m 4`. When an exception was loaded and `-x` still permits a second word, so
an exceptional entry can actually be extracted, replace the claims about
literal words with the phase-two invariant and unit actually being bounded:

```text
LETTERS letters "BAG", entries of MINIMUM+ letters, at most COUNT segments
```

For example, seven letters under `-m 4` report entries of four or more letters
and at most one segment. Keep any existing `-g` suffix that reports an exact
segment count. `-g` remains a count of index entries, not of their component
words.

## Preserved behavior

The exception alters only minimum-length admission and short-pair orientation:

- `--exclude-pairs` continues to win when the same complete entry is present
  in both inputs.
- `--dict` continues to validate every component word independently, and it
  wins over an asserted pair. The dictionary is a global vocabulary constraint,
  so a pair-file bypass would make its guarantee conditional on an unrelated
  input. A dictionary missing a short first word therefore makes the exception
  inert: the walker skips the whole space branch, so it neither emits the pair
  nor traverses into the second word, and nothing diagnoses it.
- An admitted known pair continues to receive its existing pair scoring flag.
- Pair matching remains exact whole-entry equality.
- `-g` continues to count index entries; `new york` is one segment.
- `-x` continues to be an unconditional maximum on words within one entry.
- Pair entries whose words all satisfy `-m` remain symmetric, including
  their solo-word pair edges. Entries containing a short word become
  directional in both roles.
- Results unrelated to `--pairs` continue to obey the ordinary `-m` rule.

## Implementation status

Use these status values:

```text
[ ] pending
[-] in progress
[x] complete
```

| Step | Deliverable | Status |
|---:|---|:---:|
| 1 | Minimum-aware extraction pair loader | [x] |
| 2 | Shared extractor exception path | [x] |
| 3 | Both CLI integrations, diagnostics, and help | [x] |
| 4 | Focused validation and review | [x] |

The four steps form one behavior change and should land together. A partially
landed loader or extractor would either reject existing inputs in the wrong
mode or expose traversal without the matching admission rule. The proposed
commit subject is:

```text
Allow known short-word pairs through extraction
```

## Implementation sequence

### Step 1 — Add an extraction-specific pair-loading policy

Change `source/dfs-cli-args.h` and `source/dfs-cli-args.cpp`.

- Retain `load_pair_file()` as the minimum-independent, symmetric default used
  by score mode, exclusions, and rejection lists. Do not add extraction
  semantics to its default arguments.
- Add an explicit extraction entry point taking the finalized minimum word
  length and producing both:
  - the oriented complete-entry `DfsPairSet`; and
  - a `DfsPairSet` of `exception_prefixes`.
- Share the existing parsing and cleanup implementation internally. Do not
  parse the file twice.
- Stage all normalized rows before modifying either output set, preserving the
  current all-or-nothing behavior on malformed input or read failure.
- For one-field rows, insert the cleaned word once after validating its total
  length. It cannot be an exception prefix because a valid standalone word is
  already at least `-m` characters long.
- For two-field rows, apply the direction and prefix rules above. Continue to
  collapse duplicate keys through `DfsPairSet`.
- Preserve existing pair-list path and line-number diagnostics, hyphen
  handling, and the exit-status contract, and keep the row/key count line in
  its current format. Add the exact total-length diagnostic specified above.
- The reported key count is expected to change. A two-field row contributes two
  keys only when both words satisfy `-m`, so the same file reports fewer keys
  at a higher minimum. This is the intended signal that orientation was
  applied, not a regression. The existing `ab,cd`/`cd,ab` assertion is
  unaffected because both words satisfy its `-m 2`.
- Update the loader and `DfsPairSet` comments that currently promise every
  pair is stored in both orientations.

### Step 2 — Teach the shared extractor about exceptional prefixes

Change `source/dfs-class-list.h` and `source/dfs-class-list.cpp`.

- Add a borrowed optional `exception_prefixes` input to `DfsClassList` and
  `DfsExtractor`, adjacent to the existing pair-set input. A null or empty set
  must preserve the current extraction path and derived capacity.
- Compute the effective extraction word capacity from the normal derived
  bound, then the exceptional two-word expansion when prefixes exist and
  `include_phrases` is true, then the explicit `-x` tightening, in that order.
- Size `choices` and enforce the packed-text bound from that same effective
  capacity. Update the constructor and 128-letter representation comments so
  their proof uses the effective capacity.
- Extend `walk()` with the `requires_pair_match` state described above. Keep
  space-edge visibility, entry emission, and further continuation as separate
  decisions.
- Permit an exceptional continuation only from an exact first-word prefix.
  Require exact oriented whole-entry membership before emitting an entry
  containing a short word, and never continue past the second word of an
  exceptional continuation. Membership in `exception_prefixes` alone must not
  cap an entry whose first word crossed its boundary under the ordinary gates.
- Keep dictionary checks at each word boundary, exclusion checks before
  emission, and known-pair score flags on the admitted complete entry.
- Preserve the existing hot path when no exceptional prefixes were loaded.
  Exceptional traversal may use exact hash lookups but must not scan all pair
  keys.

### Step 3 — Integrate the two command modes deliberately

Change `source/dfs-anagrams.cpp` and `source/query-index.cpp`.

- `dfs-anagrams` always loads `--pairs` with the extraction-specific policy,
  using the finalized `args.common.min_word_len`, then passes both resulting
  sets to `DfsClassList`.
- In `query-index`, keep the near-mode return before any pair load. After the
  mode is known:
  - score mode uses the existing symmetric `load_pair_file()` path; and
  - ordinary extraction uses the minimum-aware loader and passes its prefix
    set to `DfsClassList`.
- Continue passing the oriented complete-entry set to `DfsSoloWords`; its
  existing candidate-then-solo probe then implements the directional rule
  without a second orientation policy.
- Preserve plain `--words-only` by leaving phrase extraction disabled.
  `--words-only --require-completable` may use exceptional phrases internally,
  after which the existing member filter removes them from displayed output.
- Change the `dfs-anagrams` startup diagnostic only when an exceptional prefix
  was loaded and the requested `-x` still permits two words, using the
  entry/segment wording specified above. Under `-x 1` the exception cannot fire
  and the existing per-word wording remains accurate, so leave it. Preserve the
  exact-`-g` suffix and the current wording when no exception exists.
- Update detailed `--pairs` help in both commands with the extraction and score
  mode distinctions. Keep the synopsis unchanged.

### Step 4 — Validate and review the complete change

All test-related work—coverage evaluation, test updates, new cases, and test
execution—must use the latest Terra model as required by `AGENTS.md`. Keep the
resulting suite to focused smoke coverage, combining cases in shared fixtures
where practical.

The shared fixture in `source/make-dfs-test-index.cpp` covers the
short-first-word direction already: `f,gh` under `-m 2` with the bag `fgh`
needs the exception on both gates. The long-then-short orientation, an
unrelated entry sharing an exception prefix, and a longer phrase containing a
listed pair have no fixture support and need new entries. Add them in a letter
range no existing case uses, following the fixture's disjoint-group convention,
so the exact-output assertions for `abcd` and `fghij` in `test-dfs-cli.sh` are
not perturbed.

Run the repository setup from its root before building:

```bash
source ./setup.sh
source .env/bin/activate
conan build .
source build/dep-info/conanbuild.sh
```

Compile the affected targets:

```bash
meson compile -C build dfs-anagrams query-index
```

Run only the focused tests selected by the Terra test activity. Before
committing:

1. Run `/review` over the complete implementation diff.
2. Resolve every correctness finding and rerun affected checks.
3. Run `git diff --check`.
4. Inspect `git status --short` and stage only this plan's implementation,
   tests, and completed status updates.
5. Preserve all unrelated dirty and untracked files.

## Validation requirements

An implementation should cover at least:

- `new york` is absent under `-m 4` without `new,york` and present with it;
- `new,york` does not admit the reverse `york new` under `-m 4`;
- an explicit `york,new` admits the long-then-short orientation;
- a pair whose two words both satisfy `-m` remains symmetric;
- `be,an` is accepted and `be,a` fails validation under `-m 4`;
- a standalone pair-file word shorter than `-m` fails validation;
- validation measures normalized field characters and excludes the separator;
- `-x 1` prevents a two-word exception;
- an unrelated short entry and a longer phrase containing the listed pair
  remain rejected;
- listing `new,york` does not emit the short first word `new` as a standalone
  entry or make it available to phase two;
- an ordinary long-word entry sharing an exception prefix remains admitted;
- an explicitly excluded exception remains excluded;
- dictionary filtering still rejects a missing component;
- pair scoring and directional solo-word pair edges use the oriented set;
- ordinary `query-index` implements the same extraction exception;
- `query-index --score` retains its existing symmetric, non-extracting
  behavior; and
- searches with short-word exceptions retain correct phase-two and exact-`-g`
  results without weakening the existing minimum-entry bound.
