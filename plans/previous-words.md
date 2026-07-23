# Plan: "Most frequent words that PRECEDE a word sequence"

## Context

Companion to `plans/next-words.md` (successor words). Here the user wants the reverse:
given a word sequence like `"quick brown fox"`, list the single words that most
commonly come **before** it — e.g. `(the, ...)` — ranked by corpus frequency.

## Why this is harder than successors

The Nutrimatic index is a **forward** character trie of phrases with counts
(`source/index.h`, `arch/architecture.md`). Confirmed from `source/make-index.cpp`:

- `do_line` normalizes text to lowercase alphanumerics + single spaces over a 40-char
  window (`HISTORY_WINDOW_SIZE`, line 16).
- `do_buffer` (lines 19-25) pushes the **whole current window** as a chain, then peels
  off the first word and repeats. So **every suffix starting at a word boundary** (within
  a 40-char window) is emitted as a chain, and the trie stores it plus all prefixes with
  counts.

Consequence:
- **Successor query is cheap** — the phrase is a *prefix*; descend to it and read its
  children. O(phrase length). (This is `next-words`.)
- **Predecessor query is expensive** — the phrase is stored keyed from *its own* start
  (`"the quick brown fox"` lives on the path beginning at `the`, not at `quick`). There
  are no backward pointers, so to find every `P` with `"P quick brown fox"` you must
  *search* the trie, not jump.

**Window caveat (both approaches):** a predecessor is only captured if `P + phrase` fit
within the ~40-char window. Fine for `the quick brown fox`; long target sequences lose
their leading context.

## Two approaches (a genuine fork — confirm with user)

### A. Scan the existing index (no rebuild) — recommended for exploration
New tool, e.g. `prev-words input.index "quick brown fox" [count]`, that works on the
current index:
- DFS the trie from `IndexReader::root()`. Track the first word `P` being spelled.
- At the first space (first word complete), attempt an **exact descent** of
  `" quick brown fox "` from that node via `reader.children(node, count, c, c, &out)`
  (same exact-char descent `explore-index.cpp:27-29` uses).
- On full match, record `(count_at_end, P)` — `count_at_end` is the frequency of
  `P quick brown fox`. Rank by count, print top `count` as `<count> <word>`.
- Pruning: most first-word branches die within a char or two past the space, so it's a
  heavily pruned walk — effectively a per-query index scan (seconds, not ms).
- Reuses `IndexReader` only; no FST. Modeled on `source/explore-index.cpp`.

### B. Build a reverse index (fast queries) — heavier
- Add a build step emitting **word-reversed** text
  (`"the quick brown fox"` -> `"fox brown quick the"`) into a second `*.reverse.index`
  (a `make-index` variant / flag + its own merge pass).
- Then "predecessors of X" = "successors of reverse(X)": word-reverse the query and run
  the `next-words` logic against the reverse index. Milliseconds per query.
- Cost: pipeline change, ~2x build time and storage.

## Open questions for the user (to finalize)

1. **Approach:** A (scan, no rebuild), B (reverse index, fast), or both (ship A now, add
   B later)?
2. **Match scope:** single immediately-preceding word only (mirrors `next-words`), or up
   to N preceding words (multi-word lead-ins like `over the`)?

## Files (approach A)

- **New:** `source/prev-words.cpp` (~100 lines, structured like `explore-index.cpp`,
  reuses `IndexReader::children`).
- **`source/meson.build`** — add `prev-words` to the `index_lib`-linked executable list
  (lines 66-68), alongside `next-words`.

## Verification (approach A)

1. Build: `meson compile -C build`; confirm `prev-words` is produced.
2. For `"quick brown fox"`, expect `the` near the top; cross-check a candidate with
   `find-expr <index> '"the quick brown fox "'` (its score/count should correspond).
3. Edge cases: sequence that never occurs (empty output); very long sequence (may hit the
   40-char window limit — document); `count` larger than available predecessors.
