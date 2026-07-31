i just don't understand why we aren't using *all* of the cache machinery as dfs-anagram?  index-query should *never* run longer than dfs-anagrams for the same input.  it should *always* be shorter wall time, no?

  Searched for 2 patterns, read 1 file

Good catch — that's a real gap, not a nitpick. Let me trace why.

should_prune() has a check that fires even without a score floor:

if (worker->path.empty())
  return root_score_bound_ready && root_score_bound == -HUGE_VAL;

root_score_bound == -HUGE_VAL means "the bottom-up DP found zero reachable completions from here" — pure reachability, not a ranking signal. That's exactly the answer --require-completable wants, and it would let a dead-end remainder get rejected in one DP pass instead of an exhaustive unpruned DFS. But prepare_score_bounds only runs when sink->supports_score_pruning() is true, and DfsFirstSolutionSink never opts in — so this check never fires, and the worst case (the dead-end remainders --require-completable exists to find) is exactly the case with zero pruning. That's backwards from what the flag is for.

The plan's reasoning ("existence check doesn't need score pruning") conflated two different things: ranking-based pruning (needs a floor, genuinely irrelevant here) and this floor-independent root reachability check (relevant and currently unused). That was a mistake in the settled decisions.

There's a real tradeoff in fixing it, though:

- Cheap fix: make supports_score_pruning() return true on the sink (leave score_floor() returning false). This builds the bound table for each candidate's remainder before searching it, turning a potential exponential blowup into a bounded DP. But it pays that table-construction cost per candidate class checked, whereas dfs-anagrams pays it once for the whole run — so with a large -n, aggregate cost could still exceed a single dfs-anagrams run.
- Correct fix: build the bound table once over the original full bag (reusing the classes already built in main()), then answer each candidate's completability as an O(1) lookup of the precomputed bound at the remainder's state key — no per-class DFS or per-class table build at all. This is the design that actually guarantees "never slower than dfs-anagrams," but it requires exposing new (currently private) API on DfsAnagramSearch to query a bound at an arbitrary letters-state, which is meaningfully more surface area than the plan scoped.
