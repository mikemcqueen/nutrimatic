# Plan: "Most common words that follow a phrase" (successor-word query)

## Context

Nutrimatic's index is a **character trie of lowercased phrases with counts**, where
spaces are stored as ordinary characters (see `arch/architecture.md` and
`source/index.h`). Descending the trie through the characters of a phrase like
`"the quick brown "` lands on a node whose children are the possible next
characters; walking from there to the next space enumerates the whole words that
follow the phrase, each with its corpus frequency.

The user wants: **given a phrase, list the single words that most commonly come
after it, ranked by frequency** (successor n-grams) — e.g. `"the quick brown "` →
`fox`, ...

This is *not* what `find-expr` does (that ranks words *matching* a pattern via an
FST, and its restart/scale scoring distorts raw counts). The closest existing tool,
`explore-index` (`source/explore-index.cpp`), walks the trie descending
highest-frequency children first, but it works **character-by-character to a fixed
depth**, not whole-word continuations, and it doesn't stop at word boundaries. So
the capability is fully supported by the index but not exposed as a clean tool.

**Decision:** add a small standalone CLI tool that reuses `IndexReader` only (no FST,
no search lib), giving exact continuation counts. This is the simplest, most direct
fit and mirrors the existing `explore-index`/`dump-index` inspection tools.

## Approach — new tool `next-words`

New file `source/next-words.cpp`, modeled on `source/explore-index.cpp`
(same `IndexReader::children` traversal + `by_count` sort helper).

Usage:
```
next-words input.index "phrase " [count]
```
- `count` = max number of following words to print (default e.g. 20).
- The phrase should end at a word boundary; the tool appends a trailing space if the
  argument doesn't already end in one (analogous to `find-expr` forcing a trailing
  space so matches are whole words).

Algorithm:
1. Open the index with `IndexReader reader(fp)` (see `source/explore-index.cpp:57`).
2. **Descend the exact prefix.** For each character `c` of the (space-terminated)
   phrase, call `reader.children(node, count, c, c, &out)` and follow the single
   returned `Choice` (its `next` node and `count`). If any character has no child,
   the phrase never occurs → print nothing (or a stderr note) and exit. This is the
   same exact-character descent `explore-index` uses when `*path != '\0'`
   (`source/explore-index.cpp:27-29`).
3. **Best-first walk to the next space.** Seed a
   `std::priority_queue` of `{int64_t count, Node node, std::string partial}` ordered
   by `count` descending, with the children of the prefix node (skip an immediate
   space). Then loop:
   - Pop the highest-count entry.
   - `reader.children(node, count, CHAR_MIN, CHAR_MAX, &kids)`.
   - For each kid: if `kid.ch == ' '`, the word `partial` is complete — record
     `(kid.count, partial)` and print it; else push `{kid.count, kid.next, partial+kid.ch}`.
   - Stop once `count` words have been printed (or the queue empties).

   Because counts are non-increasing down the trie, best-first guarantees words are
   emitted in strict descending frequency order — the first `N` popped completions are
   the top `N`. Reuse the `by_count` comparator idea from
   `source/explore-index.cpp:18-20` for the priority-queue ordering.
4. Output one line per following word: `<count> <word>` (frequency first, matching the
   score-first convention of `search-printer.cpp`).

Only dependency is `index_lib` (via `IndexReader::children`, `source/index.h:70-73`).

## Files to modify

- **New:** `source/next-words.cpp` — the tool (≈80 lines, structured like
  `explore-index.cpp`).
- **`source/meson.build`** — add `next-words` to the `index_lib`-linked executable
  list at lines 66-68:
  `foreach p : ['make-index', 'merge-indexes', 'dump-index', 'explore-index', 'next-words']`.

No changes to the index format, search engine, or expression parser.

## Alternative (documented, not chosen)

You can approximate this today with `find-expr index '"the quick brown " A+ " "'` — a
quoted prefix plus a wildcard word. Downsides: results carry the restart/scale
*score* rather than the raw following-word count, whole phrases are returned (not the
isolated next word), and the FST search is heavier. The standalone tool gives exact
counts and clean output, so it's preferred.

## Verification

1. Build: `cd /home/mike/code/nutrimatic && meson compile -C build` (or the repo's
   configured build dir) and confirm `next-words` is produced.
2. Sanity vs. `explore-index`: for a known prefix, run
   `build/explore-index <index> "the quick brown " 3` and eyeball that the top
   character continuations agree with the top word from
   `build/next-words <index> "the quick brown " 10`.
3. Cross-check a couple of continuations against `find-expr`:
   `build/find-expr <index> '"the quick brown " A+ " "'` — the highest-scoring
   following words should match the head of the `next-words` list.
4. Edge cases: a prefix that doesn't occur (expect empty/notice); a prefix already
   ending in a space (no double space appended); `count` larger than the number of
   available continuations (prints all, then stops cleanly).

Note: needs a built `*.index` file to test against; if none is present locally, build a
tiny one via `make-index` from a small text sample (`arch/architecture.md` §2).
