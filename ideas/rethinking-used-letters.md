# Rethinking `--used-letters`

## Idea

Change `-u`/`--used-letters` in `dfs-anagrams` and `query-index` from
anonymous letter subtraction into a frozen, ordered search prefix.

For example:

```text
full letters:       onetwothreefour
frozen prefix:      ["four"]
remaining bag:      onetwothree
initial score:      score("four")
continuation DFS:   rooted at onetwothree
backtracking limit: the continuation root
```

The search would behave as if `four` had already been selected as the first
word. It could descend through choices made from `onetwothree`, but it could
never backtrack to a state before `four`.

## What the current behavior already guarantees

As a search-space constraint, the current implementation is already close to
this behavior. Both programs subtract the `-u` letters before constructing
phase 1 and phase 2. Consequently, the search begins at the remaining bag and
has no earlier state into which it could backtrack.

What is missing is the fact that the removed letters represented actual,
ordered word selections. The current handling loses:

- exact spelling and word boundaries, because spaces are stripped;
- ordering between words, because repeated `-u` arguments are concatenated;
- the prefix words' corpus scores;
- their position in emitted results;
- their exact identity within an anagram class.

The last point matters because phase 2 searches letter classes, while phase 3
expands each selected class into all of its spellings. Forcing the class
containing `four` would not by itself pin the spelling `four`.

## Recommended representation

Treat the supplied words as an immutable prefix outside the mutable DFS path.
Keep the continuation class list, bag, and score cache based on the remaining
letters, as they are today. Seed phase 2 with:

- the exact prefix text or member records;
- the prefix's accumulated score;
- the number of committed selections, or at least a flag indicating that a
  first selection already exists;
- a fresh continuation entry point of zero.

The continuation's first selected class would pay the normal restart cost
after the prefix. Backtracking would restore only continuation choices; the
immutable prefix would never be restored.

Keeping the prefix outside the ordinary class-index path also avoids distorting
the existing maximum-depth calculation. The tail can retain its current depth
limit derived from the remaining letters.

## Why not visit the forced word as an ordinary DFS child?

An arbitrary forced word cannot safely be sent through the normal root
selection machinery.

At each node, DFS chooses the rarest remaining symbol and scans only the class
bucket associated with that symbol. A forced word may not contain the root's
rarest symbol, so it might not be a legal ordinary root candidate even though
its letters fit the bag.

The DFS also passes the selected class index down as an `entry_point` to
eliminate permutations. Carrying the forced word's class index into the
continuation could incorrectly exclude valid tail classes with smaller
indexes. The committed prefix should be exempt from that canonical ordering;
the continuation should be re-rooted with `entry_point = 0` and canonicalize
only its own choices.

## Resolving and scoring the prefix

The current continuation `DfsClassList` is built from the remaining bag, so it
will not contain a prefix word whose letters have already been removed.
Prefix words therefore need to be resolved separately against the index.

An exact index lookup is preferable to building all phase-1 classes from the
full bag:

- it validates the requested spelling;
- it obtains the exact member's corpus count and word count;
- it avoids expanding the larger full-bag phase-1 search space;
- it lets rarity ordering and caches remain specialized to the remainder.

For forced selections `p[0] ... p[k-1]`, the initial log score would contain
their exact member scores plus the restart cost between separate selections.
The first continuation selection would add one more restart. A quoted
multi-word phrase treated as one indexed member would instead use that
member's phrase score and word-bonus rules.

`dfs-anagrams` output would prepend the pinned prefix text and expand only the
continuation classes. Deduplication should still use the complete spelling,
including the prefix.

## Effect on `query-index`

The practical effect in `query-index` is smaller because it reports possible
next entries rather than complete search paths.

For candidate generation and exact completability, a frozen prefix followed by
a continuation rooted at the remaining bag is letter-for-letter equivalent to
the current subtraction behavior. The existing completability operation
already removes each candidate and asks whether its remainder can be tiled.

The observable changes in `query-index` would therefore mainly be:

- validating that the prefix consists of actual indexed selections rather
  than arbitrary letters;
- retaining exact word boundaries and order;
- optionally displaying the prefix with each suggestion, if that is desired.

Its current ranking of next candidates need not change: the prefix contributes
the same score constant to every candidate.

## Semantic decisions

Before implementation, settle these details:

1. **Multiple selections.** A clear convention would be one forced indexed
   selection per option occurrence:

   ```text
   -u four -u two
   ```

   Under that convention, `-u "four two"` would mean one indexed phrase
   selection rather than two separate selections. Alternatively, spaces could
   always split words, but that would make forcing an indexed phrase
   impossible without another syntax.

2. **Index validation.** Forced selections should probably be required to
   exist exactly in the corpus index. Otherwise `dfs-anagrams` has no
   defensible score for them.

3. **Minimum word length.** `-m` should probably constrain future selections,
   not reject committed history. A short word supplied through `-u` has
   already been chosen.

4. **Dictionary filtering.** If `--dict` describes the allowed vocabulary, it
   should probably validate the forced prefix as well.

5. **Exhausted bags.** If the prefix consumes every supplied letter,
   `dfs-anagrams` could emit the prefix itself as the sole completed solution.
   `query-index` would have no next candidates. This differs from today's
   shared subtraction helper, which rejects an empty remainder.

## Scope

The DFS mechanics needed for a committed root are modest: its worker already
has a mutable bag, accumulated score, path, and score key. The substantial
parts of the change are preserving exact word identity, calculating prefix
scores, extending output to include pinned members, and keeping the forced
prefix outside the DFS's canonical class ordering.

In short, the idea is feasible. Current subtraction already creates the desired
backtracking boundary; the new feature would make that boundary a real,
scored, ordered word prefix rather than an anonymous reduction of the letter
bag.
