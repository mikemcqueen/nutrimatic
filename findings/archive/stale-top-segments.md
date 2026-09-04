# Filtering the review frontier against the classified sets

Date: 2026-09-02

`top-segments` now takes `--wfroot ROOT` and `-y`, which read the workflow's
standing verdicts. This finding covers what happens if `wf best gen
top.segments` passes them.

## Why

`gen top.segments` caps the frontier at `-n 1000`. Today those 1000 rows are
unfiltered, so pairs that already have a verdict occupy slots in the cap and
are then subtracted again at review time (`workflow/best/commands.py:399-409`).
A thousand pre-filtered candidates is strictly more review than a thousand
candidates some of which are already answered.

## The argv

`target.root` is already what `gen_dfs` passes to `--exclude-pairs`, so the
addition in `gen_top_segments` (`workflow/best/generate.py:188`) is
`["--wfroot", str(target.root), "-y"]`.

Add `fs.raise_if_not_file` for both classified files first.
`pair-exclusions.cpp:121` only *warns* on a missing file, and a warning would
produce an unfiltered frontier that the state machine then believes is
filtered.

## The two flags are not symmetric

`-y` ignores (`top-segments.cpp:105`): the YES pair is not counted, but its
line still contributes every other segment on it.

`--wfroot` rejects (`top-segments.cpp:101`): a line containing a NO pair is
dropped whole, so it stops contributing counts for its other segments too.
That reshapes the ranking, not just the membership. It is the same semantics
`--exclude-pairs` already applies at search time, so it is consistent, but a
regenerated frontier can differ in ways beyond "the classified ones are gone".

## The new precedence row

Passing these flags makes `top.segments` depend on the classified sets, which
it does not today. That needs a row in `workflow/best/state.py`.

`_review_needed` (`state.py:788`) keeps working unchanged: right after a
classify the archived round is newer than `top.segments`, so it declines. The
new row fires there — frontier behind the classified sets — and offers `gen
top.segments`.

Placement is between `_review_needed` and `_best_pairs_missing`:

- below G3, so a fresh frontier gets reviewed rather than immediately
  regenerated;
- above G4, so `best.pairs` is not built, the frontier regenerated, and
  `_best_pairs_out_of_date` made to demand it a second time.

Date it against `_generated(inputs.top_segments)`, not
`top_segments.stat()`. `setops._place(..., stable_mtime=True)` means a regen
that changes nothing leaves the content mtime behind, so a content-clock
comparison would report stale forever — the failure `_generated`'s docstring
(`state.py:311`) exists to describe. The marker clock also makes the loop
terminate: a no-op regen bumps `.top.segments.gen`, the new row declines, and
`_review_needed` still declines because the content mtime did not move.

The payoff: after each classify the same DFS refills 1000 fresh candidates,
instead of the frontier only moving when a search finishes.

## How the open decision was resolved

Implemented in `words`, 2026-09-02, by `plans/manual-best-pairs.md`. The
decision this section used to pose — how to keep `best.pairs` alive once `-y`
hides the YES pairs from the frontier — dissolved rather than being rerouted.

`best.pairs` stopped being derived from the frontier at all.
`generate.build_best_pairs` is gone, `gen best.pairs` is no longer a stage, and
the file is now an optional hand-edit that nothing generates. `gen dfs.best`
unions `classified/yes` with that file at run time, filters the union down to
what the target's letter bag can spell, and publishes the result beside its
results as `dfs.best.pairs`.

That is safe for the reason the `--pairs` flag already relies on: it is a pure
scoring flag, `dfs-class-list.cpp:281-286` looks each multi-word index entry up
at enumeration time, and enumeration is bounded by the bag — so a pair the bag
cannot spell is never emitted, never looked up, and cannot change a score
whether it is in the set or out of it. Running the same argument backwards is
what licenses the bag filter.

So `-y` breaks nothing. A brand-new target now reaches its first `dfs.best`
with every confirmed pair its bag can spell rather than with whatever its own
first review round happened to surface: 16 pairs for `s6/u-toyfastmusketsalvo`
where its `best.pairs` supplied 2, and 54 for each `s7/u-vindiesel` target
where they supplied 52, 48, and none.

The precedence row landed as `_frontier_behind_classified`, dated against the
generation marker as described above, but **below** `_top_segments_behind_dfs`
rather than above `_best_pairs_missing` — those rows are gone, and with them
the argument for the old placement. Generating from a newer DFS satisfies both
conditions at once, where a regen from the recorded source would bump the
marker past a finished search and lose it.

The citations above are stale: `pair-exclusions.cpp:29-36` is the warning, and
`top-segments.cpp:104` and `:95,102` are the ignore and reject paths.
