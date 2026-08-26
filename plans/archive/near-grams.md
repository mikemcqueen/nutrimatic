# Plan: add `query-index --near`

## Status

This is an implementation plan. No source changes have been made.

Status values are `[ ]` pending, `[-]` in progress, and `[x]` complete.

| Phase | Deliverable | Status | Proposed commit |
|---:|---|:---:|---|
| 1 | Literal near-gram mode and bounded trie search | [ ] | `Add near-gram index queries` |
| 2 | CLI smoke coverage and production-index checks | [ ] | same commit |

## Outcome

Add a third `query-index` mode:

```text
query-index INPUT.index INPUT --near WORD [-n TOP]
```

`INPUT` and `WORD` are each one or more literal lowercase `a-z0-9` words.
The mode reports aggregate index phrases that start with one argument, end
with the other, and contain at least one complete intervening word.

For example:

```text
query-index "$IDX" alpha --near omega
```

can report:

```text
COUNT alpha middle omega
COUNT omega one two alpha
```

but not the two-word phrase `alpha omega`. More generally, if the arguments
contain `a` and `b` words, every result contains more than `a + b` words. The
printed phrase spans exactly from the first supplied endpoint to the second;
it does not add unrelated words before the first endpoint or after the second.
This interpretation makes the extra words the distance between the terms and
avoids a full-index search for arbitrary outer context.

Search a direction only when its first endpoint is an aggregate index entry:

- if `INPUT` is present, search `INPUT ... WORD`;
- if `WORD` is present, search `WORD ... INPUT`;
- if both are present, search both directions; and
- if neither is present, reject the query with a diagnostic naming both
  arguments.

An endpoint is "present" when `IndexReader::aggregate_entry_position()` can
consume it and its implicit trailing space. Do not use `exact_entry_count()`:
an aggregate phrase supported only by longer retained phrases is a valid
anchor everywhere else in `query-index` and phase 1.

## Why the existing index can answer it

The index is a forward character trie whose spaces are real edges. Once an
anchor and its trailing space have been resolved, every descendant path is a
longer phrase beginning with that anchor. At each later word boundary,
`continuation_entry_position()` can probe the other endpoint directly. The
returned `aggregate_count` is exactly the count of the complete candidate
phrase, including occurrences that continue with still more corpus text.

The two directions are asymmetric but complementary. A query does not need a
reverse index: `INPUT ... WORD` is below the `INPUT` node, while
`WORD ... INPUT` is below the `WORD` node. This is also why at least one supplied
term must be an anchor. No `DfsClassList`, FST, letter-bag search, or phase-2
completion search participates in this mode.

Production indexes are built from a 40-character history window in
`source/make-index.cpp`. Near-gram results are consequently limited to phrases
retained inside that window and by the merge frequency cutoff. The feature
does not promise every n-gram from the original corpus at arbitrary distance.

## CLI contract

1. Add `--near WORD` as a required-argument long option local to
   `source/query-index.cpp`. Supplying it changes the positional argument from
   a letter bag to a literal phrase. `--near` and `--score` are mutually
   exclusive.
2. Validate both literal phrases strictly: nonempty lowercase `a-z0-9` words
   separated by exactly one space, with no leading or trailing space. Keep the
   original spelling for output. Extract a small single-entry parser from the
   validation already performed by `parse_score_sequence()` so the two modes
   cannot drift.
3. Keep `-n`/`--top` in near mode. It defaults to 100 and `0` means all, matching
   ordinary `query-index`.
4. Reject every other extraction or scoring option in near mode with the
   existing style of diagnostic, for example
   `error: --dict cannot be used with --near`. In particular, reject
   `-u`, `-m`, `-x`, `--pairs`, `-P`, both bonuses, both solo-word options,
   `-w`, `--csv`, `--require-completable`, and `-S`. These options either refer
   to a letter bag/DFS search or would give a misleading meaning to one
   aggregate phrase count.
5. Print `<aggregate-count> <phrase>` in count-descending, phrase-ascending
   order. With no matches, print nothing and exit successfully. Invalid syntax,
   incompatible options, or two missing anchors exit 2; index open/read failure
   remains an operational failure with exit 1.
6. If the two arguments are identical, run the direction once. For distinct
   multi-word arguments, merge the directional outputs and remove any identical
   full phrase before applying the final top limit.

## Trie search

Add file-local near-query code to `source/query-index.cpp`; the index reader
already exposes every primitive needed.

For one `anchor ... target` direction:

1. Resolve `anchor` with `aggregate_entry_position()` and start immediately
   after its trailing-space node.
2. Walk descendant trie characters while retaining the text after the anchor.
   A child space completes one intervening word. Do not probe `target` before
   the first such boundary, which excludes the `a + b`-word adjacent phrase by
   construction.
3. At every later word boundary, form its `EntryPosition` from the child's
   `next` and `count`, then call
   `continuation_entry_position(position, target, &match)`. On success, record
   `match.aggregate_count` and the full `anchor + gap + target` text.
4. Continue below that boundary as well, so two, three, or more intervening
   words are considered until the stored trie path ends.

Use the monotonic trie counts as an exact branch bound. Keep at most `TOP`
results for each direction in a worst-first heap. Once it is full, a node whose
count is strictly below the heap's worst count cannot contain a competitive
target match and can be skipped. Do not prune equal counts, because the
phrase-ascending tie break may still replace the current last row. Visit
children by count descending so a useful cutoff is established early.

For `-n 0`, retain all matches and sort at the end. Otherwise, taking the best
`TOP` from each direction is sufficient: the global best `TOP` must be in the
union of those two sets. Merge, sort, deduplicate by complete phrase, and then
apply the final limit.

Start with a depth-first traversal and one mutable path string, rather than a
frontier entry containing a separately allocated string for every trie node.
The latter can become expensive under a common anchor. If production timing
shows that count-ordered DFS does not establish a cutoff soon enough, the
follow-up optimization is a compact best-first frontier with shared path
crumbs, modeled on `SearchDriver`; it is not required for the first change.

## Source changes

### `source/query-index.cpp`

- Extend `Args` and `long_options` with near-mode state and the literal target.
- Track the first near-incompatible option while parsing, independently of the
  existing `--score` incompatibility tracking. `--top` is the sole shared option
  allowed in near mode.
- Factor strict literal-entry validation out of `parse_score_sequence()` and
  use it for both near endpoints.
- Open the index once, resolve the two possible anchors, and dispatch near mode
  before dictionary, pair, scoring-model, solo-word, `DfsClassList`, or
  `DfsAnagramSearch` setup.
- Add the directional trie walker, bounded result keeper, two-direction merge,
  and count/text printer as file-local implementation details.
- Update the leading file comment and `usage()` text to describe the three
  modes and the aggregate-entry rule.

### `source/index.h` and `source/index-reader.cpp`

No change is expected. `EntryPosition`, `children()`,
`aggregate_entry_position()`, and `continuation_entry_position()` already
provide root anchoring, descendant enumeration, direct target probes, and
aggregate counts. Only add an index-library helper if implementation proves
that constructing a child `EntryPosition` repeatedly is error-prone; do not
add an API preemptively.

### `source/test-query-index.sh`

Keep coverage smoke-sized and reuse the current synthetic index:

- `f --near ij` finds `f gh ij`: `f` is an aggregate anchor, `ij` is not a root
  entry, and `gh` supplies the required intervening word;
- `ij --near f` finds the same phrase through the other argument's anchor,
  proving that either CLI argument may be the one present in the index;
- `gh --near ij` does not print the adjacent `gh ij` phrase;
- two missing anchors fail with exit 2 and a focused diagnostic;
- `ab --near cd` is accepted when both are anchors, even if this fixture has no
  qualifying longer phrase;
- `-n` is accepted, while one representative DFS-only option and `--score` are
  rejected with `--near`; and
- malformed literal spacing/characters are rejected before any output.

These cases require no new fixture chains, so the synthetic corpus total of
1142 and all existing score expectations remain unchanged.

## Validation

1. Build in the repository environment:

   ```bash
   source ~/code/nutrimatic/.env/bin/activate
   conan build .
   source build/dep-info/conanbuild.sh
   meson compile -C build
   ```

2. Run the focused smoke test:

   ```bash
   meson test -C build query-index-cli --print-errorlogs
   ```

3. Inspect `./build/query-index --help` and manually exercise the synthetic
   one-anchor, reverse-role, adjacent-only, and neither-anchor cases.
4. Set the production index exactly as required by the repository:

   ```bash
   export IDX=~/code/nutrimatic/idx/wiki-merged.5.index
   ```

   Check a pair with one intervening word, a pair with several intervening
   words, both direction orders, stable count/text ordering, `-n 1`, and
   `-n 0`. Cross-check selected output counts with a direct
   `aggregate_entry_count()` probe or `query-index --score` on the full phrase;
   both use the same aggregate trailing-space count.
5. Measure a common-anchor query and a selective-anchor query to catch an
   accidental full-index traversal or excessive result storage. Before every
   timed run, check the host process table for both `query-index` and
   `dfs-anagrams`, as required by `AGENTS.md`. Timing is a sanity gate, not a
   reason to add the compact frontier unless the simple bounded DFS is
   observably unusable.
6. Run `/review` before any commit, then address findings and rerun the focused
   validation affected by the changes.

## Explicit non-goals

- Do not return arbitrary larger phrases with extra context outside the two
  endpoints. Supporting that interpretation of "contains both" requires
  searching unrelated root prefixes and can degenerate into a full scan of the
  1.2 GiB production trie.
- Do not interpret either endpoint as an anagram, regular expression, or
  substring of a word.
- Do not change the index format, build a reverse index, or alter the
  40-character corpus history window.
- Do not apply DFS segment, word, pair, or solo-word scoring. Near rows are one
  aggregate corpus phrase ranked by their observed count.
- Do not add parallel traversal in the first implementation.
