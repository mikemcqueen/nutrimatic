# Plan: show per-segment DFS bonuses

## Status

Design only. No production implementation has been authorized or started.

## Outcome

`dfs-anagrams --show-bonus` adds one comma-separated bonus column between the
displayed score and the anagram. The column has one marker for each emitted
segment, in the same order as the comma-separated segment list:

```text
1234 WS,WY,WB seed pair,yes pair,best pair
```

The ordinary output contract remains unchanged when `--show-bonus` is absent:

```text
1234 seed pair,yes pair,best pair
```

This is an output-annotation feature only. It does not change extraction,
scoring, ordering, pruning, deduplication, or the number of retained results.

## Marker semantics

Use these single-character markers:

- `W`: the segment received the `--word-bonus` term;
- `P`: the segment received the legacy `--pairs` / `--pair-bonus` term;
- `S`: the segment received the fixed SEED pair tier;
- `Y`: the segment received the fixed YES pair tier; and
- `B`: the segment received the fixed BEST pair tier.

Within one segment, print `W` first and then its pair-source marker. A segment
can receive at most one of `P`, `S`, `Y`, or `B`:

- legacy `--pairs` cannot be combined with the fixed-tier sources; and
- duplicate or reversed SEED, YES, and BEST inputs retain only the strongest
  tier, with `B > Y > S`.

Print `-` when no score bonus applies to a segment. This preserves a strict
one-to-one positional mapping even when some segments have no bonus:

```text
1234 WS,-,WB seed pair,ordinary segment,best pair
```

A marker describes an actual nonzero score term, not mere eligibility or file
membership. Therefore:

- omit `W` when `--word-bonus 0`;
- omit `P` when `--pair-bonus 0`; and
- retain `W` or `P` for a negative nonzero value when that value is legal,
  because the corresponding score term was still applied even though it acts
  as a penalty.

The fixed SEED, YES, and BEST terms are nonzero constants, so a selected fixed
tier always prints its marker.

## What counts as the word bonus

For an ordinary extracted entry, `W` applies when the entry contains more than
one word. It is not intrinsically limited to two words. The normal
`dfs-anagrams` default of `-x 2` makes two-word entries the usual case, but an
invocation allowing wider entries can print `W` for a three-or-more-word
segment as well.

Positive pair files may also contain standalone entries. Such a single-word
segment can print `P`, `S`, `Y`, or `B` without `W` when its exact entry is
selected and the corresponding pair term is nonzero.

## Solo-word semantics

`--solo-words` needs separate treatment. A selected single-word index entry can
receive the multi-word bonus by pairing with one external solo word, and an
asserted pair edge can also supply `P`, `S`, `Y`, or `B`. For example, a
single-word segment whose selected external partner came from the YES source
prints `WY` even though the stored index entry itself contains no space.

The marker must describe the exact solo matching retained for that spelling:

- print `W` only when the segment was assigned an external partner and the
  word-bonus term is nonzero;
- obtain the pair tier from that selected partner's
  `DfsSoloMasks::pair_kinds` entry; and
- do not use `dfs_member_solo_pair_bonus_kind(score_flags)` as the final
  answer, because the member flags hold the strongest possible solo edge for
  an upper bound, which need not be the edge selected by the global matching.

`--hide-solo-words` changes only whether the parenthesized partner is shown in
the anagram column. It does not hide the bonus marker: the score still contains
the selected solo edge's terms.

## CLI contract

Add the long option only:

```text
--show-bonus
```

Document it in the detailed help as adding one marker per segment between the
score and anagram. Add `[--show-bonus]` to the usage synopsis; because the
option has no short form, its long spelling is the only spelling to show there.

Reject `--show-bonus` with `--segments`:

```text
error: --show-bonus cannot be combined with --segments
```

`--segments` emits an aggregate `best-score count text` table rather than one
row per retained spelling, so a per-answer aligned marker column does not have
the same meaning there. `--weighted` already requires `--segments` and is
therefore incompatible through the same rule.

Do not add this option to `query-index`. Its `--score` mode continues to emit
the existing two-column score and sequence format.

## Output compatibility boundary

The annotated form is deliberately opt-in and display-only. Existing readers
such as `first-segments`, `filter-segments`, `top-segments`, `segment-stats`,
and `measure-coherence` currently treat everything after the score as the
segment list. They would interpret the marker column as part of the first
segment rather than ignore it.

Do not change those consumers in this implementation. Workflow-generated DFS
commands must also remain unchanged, so their persisted output retains the
ordinary `score segment[,segment ...]` contract. If annotated output later
needs to become pipeline input, add an explicit shared parsing contract in a
separate change instead of making every existing reader guess whether its
first token is a marker list.

## Implementation design

### 1. Parse the dfs-anagrams-only option

In `source/dfs-anagrams.cpp`:

- add `bool show_bonus` to `Args`, defaulting to false;
- add a private long-option code and a `show-bonus` row to `long_options`;
- set the flag in `parse_args()`;
- reject its combination with `--segments` after all arguments have been
  parsed; and
- add the detailed help text and incompatibility statement.

Keep the option out of `DFS_COMMON_LONG_OPTIONS`: the annotation applies only
to `dfs-anagrams`, not to `query-index` or its score output.

### 2. Retain exact per-segment bonus metadata only on demand

Add a compact per-segment bonus representation in `source/dfs-output.h`. One
byte is sufficient: reserve one bit for `W` and encode at most one
`DfsPairBonusKind` in the remaining bits. Keep the representation internal;
the stable user contract is the rendered `W`, `P`, `S`, `Y`, and `B` text.

Add `segment_bonus_flags` vectors aligned with `segment_lengths` to both
`DfsSpelling` and `RetainedSpelling`. They may be empty when annotation was not
requested, following the existing optional `solo_word_indexes` pattern.

Give `DfsTopN` an optional `retain_segment_bonuses` constructor argument,
defaulting to false for existing callers and tests. `dfs-anagrams` passes
`args.show_bonus`. Avoid allocating or populating the new vectors on the
default path so ordinary large top-N searches pay no new per-result storage
cost.

Every `DfsTopN::offer()` path must move the metadata together with the spelling:

- replacement of an already-retained word-set key;
- insertion before the bounded heap is full;
- heap-root eviction and node reuse; and
- `take_sorted_results()` drainage.

The bonus metadata must not participate in score ordering, word-set identity,
or tie-breaking.

### 3. Record ordinary-entry bonuses during expansion

While `DfsTopN::emit()` expands the selected class members, initialize one
bonus byte per segment when retention was requested.

For each `DfsMemberView`:

- set `W` when `word_count > 1` and
  `score_model->multi_word_log_bonus() != 0.0`;
- obtain the exact direct pair source with
  `dfs_member_pair_bonus_kind(view.score_flags)`; and
- encode that source only when
  `score_model->pair_log_bonus(kind) != 0.0`.

This reads the same member metadata the score model already uses. Do not probe
the original pair sets or reload source files during output.

### 4. Correct the selected solo edges

After `dfs_solo_score_correction()` returns `profile_matches`, update the bonus
byte for each matched profile segment:

- add `W` when the word-bonus term is nonzero; and
- read the selected source from
  `profiles[i].pair_kinds[profile_matches[i]]`, adding its marker only when its
  score term is nonzero.

This must happen from the concrete `profile_matches` assignment before the
spelling is offered to the retained map. An unmatched profile adds no solo
bonus markers.

### 5. Render the aligned marker column

Add a small formatter in `source/dfs-output.cpp`, declared in
`source/dfs-output.h`, that:

- requires `segment_bonus_flags.size() == segment_lengths.size()`;
- renders one token per segment separated by commas;
- renders characters in `W`-then-pair-source order; and
- renders a zero byte as `-`.

Map all `DfsPairBonusKind` values explicitly. Treat `DFS_PAIR_BONUS_NONE` as no
pair marker; unreachable or corrupt values should assert rather than silently
produce misleading output.

In the final ordinary-output loop in `source/dfs-anagrams.cpp`, print:

```text
SCORE BONUS-LIST ANAGRAM
```

only when `args.show_bonus` is true. Keep the existing `printf` call intact on
the default branch so formatting, precision, commas, solo annotations, and
byte-level output remain unchanged.

## Minimal validation

Keep validation focused on the output contract in the existing small DFS
fixtures.

### CLI smoke coverage

Extend `source/test-dfs-cli.sh` with compact cases that establish:

1. Without `--show-bonus`, output remains exactly the existing two-column
   format.
2. A multi-word entry and an ordinary single-word entry render `W` and `-`
   respectively.
3. One fixed-tier invocation covers `S`, `Y`, and `B`, including an overlap
   that confirms only the strongest tier is displayed.
4. A separate legacy invocation renders `P`, because legacy and fixed-tier
   inputs cannot be combined.
5. A multi-segment answer renders exactly one comma-separated marker per
   comma-separated segment and preserves segment order.
6. `--word-bonus 0` and `--pair-bonus 0` suppress `W` and `P` respectively.
7. One `--solo-words` case confirms that the marker follows the actually
   selected partner and source tier, including when `--hide-solo-words` hides
   the annotation.
8. `--show-bonus --segments` fails with status 2 and the specified diagnostic.

Prefer combining categories in a few invocations rather than adding a broad
matrix. Use exact line comparisons for the marker column; score closeness is
already covered by existing scoring tests and this feature must not recompute
the score.

### Focused build and tests

```bash
source ./setup.sh
source .env/bin/activate
conan build .
source build/dep-info/conanbuild.sh
meson compile -C build dfs-anagrams test-dfs-output
meson test -C build dfs-output dfs-cli --print-errorlogs
git diff --check
```

No timing measurement is required: the default path does not retain bonus
vectors and the annotated path is intended for presentation. If timing is
added later, first follow `AGENTS.md` and check the host process table for both
`query-index` and `dfs-anagrams` outside the sandbox PID namespace.

Review the complete implementation diff with `/review` before committing, as
required by `AGENTS.md`.

## Expected files

Implementation should normally be limited to:

- `source/dfs-output.h`;
- `source/dfs-output.cpp`;
- `source/dfs-anagrams.cpp`; and
- `source/test-dfs-cli.sh`.

`source/test-dfs-output.cpp` may be changed only if focused coverage is needed
for moving the new metadata through retained-map replacement or heap eviction.

The worktree currently contains concurrent changes in several of these files.
Implementation must inspect and preserve them, and any eventual commit must
stage only the approved feature files and hunks.

## Explicitly out of scope

- Changing any bonus magnitude or scoring formula.
- Stacking SEED, YES, and BEST bonuses instead of selecting the strongest.
- Adding provenance paths or pair-file names to result rows.
- Adding a table header.
- Annotating `--segments` output.
- Adding the option to `query-index`.
- Teaching segment-analysis tools to consume annotated rows.
- Changing workflow-generated DFS commands or persisted workflow artifacts.
- Changing ordinary output when `--show-bonus` is absent.
