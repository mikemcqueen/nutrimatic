# Why `--no-repeat` PAIR Is a Run-Length Test and the Others Are Not

`DfsTopN::admits()` runs three tests, and they do not share a shape. The PAIR
test compares each position in a class path only with the one before it, which
looks too weak: a repeated entry could appear anywhere in the path, and the
obvious implementation is a hash set of the entries seen so far. Two
monotonicity invariants make the adjacency scan sufficient there, and neither
is visible at the emit site.

## Class indexes are nondecreasing along a path

`walk()` passes the class index it just chose as the next level's
`entry_point` (dfs-all-runner.cpp:128), and the next level computes
`start = std::max(begin, entry_point)` (dfs-all-runner.cpp:176).
`walk_certified()` receives that same `start` and only ever moves forward. So
a path never revisits a class index below one it already used, and every
occurrence of one class forms a contiguous run.

This is also the reason a result can repeat an entry at all: the tie-break is
`std::max`, not `entry_point + 1`, so the class just chosen is still a
candidate at the next level.

## Repeated classes carry nondecreasing member indexes

Phase 3's expansion canonicalization (the `canonical` test in `emit()`) keeps a
successor only when the repeated class's member indexes stay nondecreasing
across the path. Permutations of one member multiset are interchangeable, so
only the sorted one is expanded.

## Consequence for PAIR values

Equal `(class_index, member_index)` pairs can therefore only land in adjacent
positions. Since a class holds one member per spelling, equal member text
implies an equal pair, so whole-entry repeats are exactly the adjacent equal
pairs. A PAIR value is whole-segment equality, so one backward comparison per
position decides it, with no hash set and no per-node state.

## WORD values and `--disable-repeats` cannot use that pass

A word can recur in two *different* entries — `"hot dog"` beside
`"dog house"` — and those are different classes in different positions, which
the adjacency invariant says nothing about. Both word-level tests therefore
scan the whole result.

- A named WORD is counted across every position, rejecting the second
  occurrence. The loop is per named word on the outside and per position on
  the inside, so the running count is a local that dies with the word. The
  transposed form is the same work but needs one counter per named word live
  across the scan, and `emit()` runs concurrently on several search threads,
  so that storage could not live on the borrowed policy.
- `--disable-repeats` is that same rule widened to every word there is, so
  there is no named list to count against. It collects the result's words and
  rejects the first one it has already seen. A result's words are disjoint
  parts of the bag and each holds at least one letter, so their number is
  bounded by `DFS_MAX_BAG_LETTERS` and the scratch array is a fixed stack
  buffer.

Both word tests count an occurrence wherever it falls, including twice inside
one entry, so `--disable-repeats` rejects a self-repeating entry such as
`"step by step"` on its own. That is intended: it is the blunt instrument, and
a run that wants those phrases back asks for them by not passing it.

The quadratic scan inside that buffer is over the words of one result, not the
corpus, so it stays small.

## Where the tests cannot go

Not in phase 2. Phase 2 chooses anagram *classes*; the member that makes a
class a segment is not chosen until phase 3, and one class can hold both a
one-word and a multi-word spelling (`onset` / `on set`, or `klmn` / `kl mn` in
the CLI fixture). A class-level test would reject `klmn` beside `kl mn`, which
shares no word and is not a repeat under any of the three tests.

Not in the traversal either. A rejected candidate is skipped, not pruned: its
successors still go on the expansion queue, because `(0,0)` on a repeated
class may be a repeat while its descendant `(0,1)` is legal. Successor scores
come from `spelling_upper_log_score()`, which needs no built spelling, so a
rejection costs only the scan.

## Known cost

`DfsTopN` publishes a score floor from its heap and phase 2 prunes against it
(`should_prune()`, dfs-all-runner.cpp:37-59). Rejected candidates never enter
the heap, so an aggressive ban list fills it more slowly, holds the floor
lower, and weakens phase-2 pruning. The search itself is unchanged; it just
gets less help from the floor.
