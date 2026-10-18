# bag-stats: Searches Without Pick Steps

Finding 6 of the second `/code-review medium` pass on the working tree that
merges `--ss pf` into the bag-stats walk. Line numbers refer to that tree.

## The finding

Searches with no ppb or pb steps (`--ss p`, `wb,p`, `pf`, `wb,pf`, ...) are
special-cased in two callers:

- `bag_stats` (`bag-stats-cpu.cpp`): `if (!walk.steps.empty()) return
  run_bag_stats(...)`, otherwise `count_remainder(walk, walk.bag, ...)`,
  one row and the completion list.
- `list_bag_stats` (pgui): `if (walk.steps.empty())` returns
  `count_remainder(...).best`, otherwise it walks the rows.

`walk_units()` also says 1 unit without steps. `PickWalk::walk` reads
`steps_[0]`, and `RowWalker::walk` computes `fitting_.size() - 1`, which
underflows if it is ever given empty steps. The review proposed letting
PickWalk treat zero pick steps as one row over the whole bag. Then
`run_bag_stats`, `RowWalker` and the listing would share one path, and both
special cases could go.

## Why one path doesn't fit

Walking zero steps as one row is easy to make correct. Size `fitting_` and
`inner.groups()` at `max(steps.size(), 1)` and let `walk()` call `leaf(bag,
letters)` when there are no steps. The two cases still differ in ways the
walk can't absorb.

**Threads.** The walk splits work by unit, its first pick, one unit per
block per thread. A search without pick steps is one remainder, so it would
be one unit on one thread. `count_remainder` gets its parallelism by
splitting that one remainder by first word bag instead (`count_first`). Put
through the walk, `--ss wb,wb,p` or `--ss wb,pf --all` would run on one
thread where it now runs on up to `--threads`.

**Completions.** The walk's leaf only counts. Listing completions needs the
`CompletionSink` that `count_remainder` hands `RestCounter`, and
`print_score_row` and `after_score` use `count_remainder` for `--score` rows
anyway. So `count_remainder` stays whatever the walk does, and the
step-less search would still call it for `--completions` and `--all`.

**Output.** `check_common_args` already rejects `-n`, SORT options,
`--score` and the rank output options without a ppb or pb step, and
bag-stats-cuda requires one. `print_score`, `print_top`, the rankers and the
census have nothing to do for a step-less search. Of `run_bag_stats` only
`print_rows` would apply, to print one row.

**Listing.** `list_bag_stats`'s two branches aren't one listing in two
forms. With pick steps, each row is a completion named by its pb pick's
PAIRS rows, with the row's TOTAL_PAIRS. Without them, the completions are
the sink's word-bag or PAIRS-row completions of the one remainder. Each
branch already calls a single function for its case.

So the two `steps.empty()` branches are the entry point of a different
search: one remainder, counted by `count_remainder`. The walk only serves
searches with pick steps.

## What is real

- `walk_units()` returns 1 without steps, "where nothing walks them". That
  holds, but the 1 is made up. `with_walk` builds `unit_blocks(1)` for a walk
  that is never walked.
- `PickWalk` and `RowWalker` don't say they need at least one step, and fail
  by indexing past the end instead of being refused.

## Recommendation

Keep the two branches. Make the split explicit:

1. `walk_units()` returns 0 without steps, so a step-less `Walk` has no
   blocks and `units()` matches what is walked.
2. The `PickWalk`, `RowWalker` and `walk_units` comments say the walk takes
   at least one pick step, and that a search without one is counted by
   `count_remainder` on the whole bag.
3. The `Walk` comment in `bag-stats-cpu.cpp` says the same, so a new
   consumer of `Walk` knows to check `steps.empty()`.

**Decision:** pending.
