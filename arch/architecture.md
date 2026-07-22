# Nutrimatic Architecture Overview

Nutrimatic is a search engine over *word/phrase frequencies from Wikipedia*. You
give it a pattern (regex-like or anagram), and it returns real English phrases
that match, **ranked by how common they are**. The whole system is built around
one data structure — a compressed **trie of character sequences** with
occurrence counts — plus an **FST-based pattern matcher** that walks that trie
best-first.

There are three stages: **build the index**, **merge indexes**, and **query**.

---

## 1. The index data structure (`index.h`, `index-reader.cpp`, `index-writer.cpp`, `index-walker.cpp`)

This is the heart of everything — a library (`index_lib`) used by every tool.

The index is a **trie** (prefix tree) of character strings (lowercased phrases
like `"the quick brown fox "`), where each node stores a frequency count. It's
serialized to a flat file in a space-efficient binary format, described in the
comment at the top of `index.h`:

- Nodes are laid out **children-before-parents**, and each node references
  children by a *byte offset backward* from itself.
- There are several encodings depending on how big the counts/offsets are (1, 2,
  or 8 bytes), plus special compact cases: a leaf-parent form, and a "single
  child, same frequency, immediately preceding" form. The top nibble of a marker
  byte selects the format.

Three classes wrap it:

- **`IndexWriter`** (`index-writer.cpp`) — consumes a *sorted* stream of strings
  via `next(text, same, count)`, where `same` is the shared-prefix length with
  the previous string (standard trie-building trick). It buffers a "chain" of
  pending nodes and emits them bottom-up.
- **`IndexReader`** (`index-reader.cpp`) — `mmap`s the file read-only. The key
  method is `children(node, ...)` → decodes a node into a list of
  `{char, count, next-node}` `Choice`s. `root()` is just the file length (root
  node sits at the end).
- **`IndexWalker`** (`index-walker.cpp`) — iterates the *entire* trie in sorted
  order (a DFS with an explicit stack), yielding every string + count. Used by
  the merge tool.

---

## 2. Building an index (`remove-markup.cpp`, `make-index.cpp`)

**`remove-markup`** (uses libxml2 + tre) is an optional Wikipedia-markup
stripper — an alternative to the external `wikiextractor`. It emits
`BEGIN ARTICLE:`/`END ARTICLE:` delimited plain text.

**`make-index`** reads that plain text on stdin and produces `*.index` files:

- `do_line` normalizes text to lowercase alphanumerics + single spaces.
- It slides a **40-char window** (`HISTORY_WINDOW_SIZE`) over the text, and for
  each position emits every trailing sub-phrase as a "chain" (`do_buffer` peels
  off word by word). This is what captures phrase frequencies at every length.
- Article **titles get counted 10×** (`TITLE_MULTIPLIER`) to boost them.
- Every million chains (`CHAINS_PER_FILE`) it sorts them and writes one
  `wikipedia.NNNNN.index` shard via `IndexWriter`. Each occurrence has count 1 at
  this stage; duplicates become adjacent after sorting and get summed during
  merge.

---

## 3. Merging (`merge-indexes.cpp`)

`make-index` produces many shards full of count-1 duplicates.
**`merge-indexes min in1.index in2.index … out.index`** does a k-way merge:

- A `priority_queue` of `IndexWalker`s (one per input) yields all strings in
  global sorted order (`ReaderCompare`).
- A `FrequencyCutoffWriter` sums counts for identical strings and **drops any
  phrase occurring fewer than `min` times** (the cutoff argument), while
  carefully preserving prefixes that are still needed by surviving longer
  strings. This is what shrinks the shards into a usable index, and why the
  README runs it in two passes with cutoffs 2 then 5.

---

## 4. Pattern matching with FSTs (`expr.h`, `expr-parse.cpp`, and friends)

Queries are compiled into **finite-state transducers** using the OpenFST library
(`expr_lib`, depends on `fst_dep`).

- **`expr-parse.cpp`** is a recursive-descent parser for the Nutrimatic query
  language → an FST. Grammar: `Expr(|) → Branch(&) → Factor(concat) →
  Piece(*+?{}) → Atom`. Atoms include literals, `[...]` char classes, groups
  `(...)`, quoted exact strings `"..."`, anagrams `<...>`, and character classes
  like `A` (a–z), `C`/`V` (consonant/vowel), `#` (digit), `.`, `_`. Notably,
  outside quotes it inserts optional spaces between characters so matches can
  span word boundaries.
- **`expr-anagram.cpp`** builds an FST that accepts any permutation of the given
  pieces (with a length constraint + "contains each part" constraints
  intersected together). `CollapseIdentical` dedups repeated letters for
  efficiency.
- **`expr-intersect.cpp`** (`&`) intersects FSTs pairwise in a balanced tree,
  optimizing between steps.
- **`expr-optimize.cpp`** runs the standard `RmEpsilon → Determinize → Minimize`
  pipeline to shrink the automaton.
- **`expr-filter.cpp`** converts the optimized FST into a fast lookup table:
  `ExprFilter` implements the abstract `SearchFilter` interface (`search.h`) with
  `is_accepting(state)` and `has_transition(state, char, &next)` — plain array
  indexing, no FST overhead during search.

---

## 5. The search driver (`search.h`, `search-driver.cpp`, `search-printer.cpp`)

`search_lib` ties the index and a filter together. **`SearchDriver`** does a
**best-first search** over the product of (trie ⨉ filter automaton):

- A `priority_queue` of `Next` states, ordered by `count * scale` — so **the most
  frequent matches come out first**.
- `step()` pops the best node, expands its trie children, and keeps those the
  filter accepts (advancing the automaton state). "Crumbs" (`Crumb`) let it
  reconstruct the matched string by walking parent pointers backward.
- When it reaches an accepting automaton state, it emits the string with a score;
  `seen` dedups.
- The `restart` mechanism: at a space, it can "restart" from the trie root with a
  scaled-down weight, letting multi-word results be scored as a product of
  independent word frequencies rather than requiring the whole phrase to appear
  verbatim.

`PrintAll` (`search-printer.cpp`) just loops `step()` and prints `score text`,
trimming trailing spaces.

---

## 6. The user-facing tools

| Binary | What it does |
|---|---|
| `find-expr index "expr"` | The main query tool: parse expr → `ExprFilter` → `SearchDriver` → print. Requires a trailing space so matches are whole words. This is what the web CGI calls. |
| `find-anagrams`, `find-phone-words` | Special-purpose searches built directly on `SearchFilter` without the full expr parser (anagrams of a word; words spellable from a phone number). |
| `dump-index`, `explore-index` | Debug/inspection tools that walk the raw trie. |
| `test-expr` | Unit tests for the expression parser. |

---

## End-to-end flow

```
Wikipedia dump
  → remove-markup / wikiextractor        (plain text)
  → make-index                           (many count-1 .index shards, sliding window)
  → merge-indexes (×2 passes, cutoffs)   (one merged, deduped, thresholded trie)
  → find-expr index "<pattern>"          (compile to FST, best-first walk of trie, ranked results)
```

The recurring themes: everything is a **sorted trie of phrases with counts**,
stored in a tightly packed `mmap`-able format, and querying is a
**frequency-ordered best-first traversal** of that trie constrained by an **FST
compiled from the query**.
