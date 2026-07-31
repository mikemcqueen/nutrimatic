# The pop-order invariant is approximate under a float log_score

`SearchDriver::seen` keeps one arrangement per word set, and the arrangement it
keeps is supposed to be the best-scoring one. That correctness argument rests
entirely on a monotonicity claim: **a path's priority never rises**, so matches
are popped in non-increasing score order and the first one to arrive wins.

Narrowing `Next::scale` (a `double`) to `Next::log_score` (a `float` holding
log2 of the whole priority) makes that claim approximate rather than exact.
Nothing observed to date depends on the difference, but the invariant is
load-bearing and the comment on `seen` used to state it without qualification,
so it is worth writing down.

## Why it stopped being exact

The old code carried the scale and the count as separate fields and recomputed
the product in `double` at every comparison:

```cpp
double scale;
bool operator<(Next const& n) const {
  return choice.count * scale < n.choice.count * n.scale;
}
```

A character transition did `new_next.scale = next.scale` — a verbatim copy, no
arithmetic, no rounding. Counts shrink monotonically down the trie. So the
product was exactly non-increasing along any path, and monotonicity was a fact
about the arithmetic rather than an approximation of it.

`log_score` folds the scale and the count into one number, which is what buys
the single-float compare on the hot path. But children share their parent's
scale and *not* its count, so `step()` has to take the count back out and put
the child's in:

```cpp
float const log_scale = next.log_score - log2f(float(next.count));
...
new_next.log_score = log_scale + log2f(float(tmp[i].count));
```

That round trip is the problem. When a child's count equals its parent's the
expression is `(p - x) + x`, and in floating point that is not required to
return `p`; it can land one ulp above it.

Equal counts are not a corner case. `IndexReader::children()` passes the parent
count straight through for the immediate-next node encoding
(`source/index-reader.cpp`), so any run of single-child trie nodes — every
interior stretch of a word — hits exactly this path.

## Size of the error

An ulp of a `float` at magnitude `m` is about `m * 6e-8`. Path scores reach
around -600 in log2 for a deep multi-word match, giving ~3.6e-5 per step. Errors
accumulate along a path; over the ~50 transitions of a long anagram the worst
case is roughly 1e-3 in log2, or ~1e-3 relative on the score.

`PrintAll` formats scores with `%#.4g`, four significant digits, i.e. ~1e-4
relative. So the drift is at the edge of the displayed precision and can wobble
the last digit.

The consequence for `seen` is narrower than that: an inversion only changes
which arrangement is kept when two arrangements' scores agree to within ~1e-3
relative. Both are then equally good answers by any standard the tool applies.

## Why it was documented rather than fixed

Both repairs cost more than the defect:

- **Carry `log_scale` alongside `log_score`.** Removes the round trip entirely,
  but adds 4 bytes and pushes `sizeof(Next)` from 28 back to 32 — undoing a
  large part of the 48-to-28 reduction this change exists to deliver.
- **Store `log_scale` and fold the count in at comparison time.** Keeps 28
  bytes, but puts a `log2f` inside `operator<`. A `priority_queue` runs O(log n)
  comparisons per push and pop with n in the millions, so that trades one
  `log2f` per push for roughly twenty, on the hottest path in the search.

Neither is worth ~1e-3 relative error in a heuristic ordering.

## If this ever needs revisiting

The signal would be `find-anagrams` reporting a *visibly* worse arrangement than
one it could have found — not a last-digit score difference. That would mean the
drift had grown well past the estimate here, which would point at paths far
deeper than ~50 transitions or scores far below -600 in log2, and the first thing
to check is the actual distribution of `log_score` magnitudes in the frontier
(`queue_median_score()` already samples it).

Note the underflow this change fixed cuts the other way, and is the reason the
tradeoff is worth taking at all: a `double` scale hits zero after ~28 restarts
(each multiplies by roughly 2^-36 with the usual `restart` of 1e-6), at which
point every path past that depth ties at exactly zero and the ordering collapses
completely rather than drifting by 1e-3.
