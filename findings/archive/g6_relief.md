# Speeding up high-`-g` runs in dfs-anagrams

Context: `-g 5` takes ~13m and `-g 6` ~72m on the same letters/options. What
follows is why the gap is so steep and what additional pruning bars are
available, ranked by leverage per line of code.

Current state of `-g` (`exact_depth`): `dfs-all-runner.cpp:78-96` plus the
`max_depth` clamp in `dfs-search.cpp`. The two bound sources are
`should_prune()` (projected score table) and
`walk_certified()` / `DfsSearchData::certificate_rejects()` (length
certificate).

## Why `-g 6` is 5.5x `-g 5`

Both bounds are **depth-agnostic**. `length_tail_bounds[left]` is "best score
achievable from `left` letters using *any* number of segments," and the
projected `score_bounds` table is the same thing over bag states. Under `-g k`
the unconstrained optimum usually lives at 2-3 segments: long, high-count
entries with few boundary penalties. So at every node the bound quotes a
completion the search is forbidden to take, over-crediting by roughly

    (k - k_free) * |segment_boundary_log_score|

plus the score gap between long entries and the shorter ones a k-way split
forces. That gap widens monotonically with k. Meanwhile the tree is bigger and
the heap floor is lower. The only depth-aware pruning in the search today is
the crude `next_letters_left / min_word_length < segments_owed` test.

## Bars worth adding, in order of leverage

### 1. Depth-index the tail bounds

Turn `length_tail_bounds[left]` into `T[left][segments_owed]`: the same DP in
`prepare_length_certificate()` (`dfs-search.cpp:407-431`) with one more
dimension.

    T[0][0]    = 0
    T[left][s] = max over len of  best_score[len] + boundary + T[left-len][s-1]

Table size is (letters+1) x (depth+1) x 8 bytes, about 2 KB; build cost is
negligible. `certificate_rejects()` then indexes
`T[letters_left - length][segments_owed - 1]` instead of
`length_tail_bounds[letters_left - length]`.

Three wins at once:

- the certificate becomes exactly depth-calibrated instead of quoting the
  3-segment optimum;
- `T == -HUGE_VAL` is an exact feasibility test that subsumes the current
  `min_word_length` division;
- it catches the direction not checked at all today: remainders too *long* for
  the segments owed, given the maximum class length.

Use the same rounding-envelope treatment as the existing code.

### 2. Fold that into `should_prune()`

`remaining_bound` and `T[letters_left][segments_owed]` are both valid upper
bounds, so take the `min`. One comparison, zero memory. `should_prune()` does
not currently receive `letters_left`, but its caller `walk()` has it. This is
what brings the projected table back into useful territory at high k.

### 3. Collapse the last level into a hash probe

When `segments_owed == 1` the remaining bag must be *exactly* one class, and
classes are already keyed by letter multiset (`DfsClassRecord::signature`). So
instead of scanning the rarest-symbol bucket with multiplicity tests, look the
remaining signature up in a `uint64 -> class index` map.

At `-g 6` the final level is where nearly all scan work happens, so this is the
biggest constant-factor win available.

Wrinkle: `signature` uses `signature_digits` in ascending *symbol* order, while
`score_key` / `exact_root_key` are in *rank* order. A second delta array
parallel to `score_key_deltas`, built in the class basis, is needed — a third
copy of the same loop in `prepare_hot_classes()`.

### 4. Seed the floor

The heap starts empty, so `sink->score_floor()` fails outright until N
solutions land, and every bound above is inert during that window — which at
high k is a long window. Two versions:

- `--min-score` flag so a floor can be carried in by hand (~20 lines; the
  number can come from a truncated run);
- automatic greedy/beam preflight over a restricted class list to fill the heap
  in seconds.

This is the cheapest real relief for an existing `-g 6` run.

### 5. Score-order the top level

Within a bucket, candidates descend by length, which
`first_length_candidate()`'s binary search and the certificate's group layout
both require. But depth 0 runs its loop exactly once and needs neither, so a
precomputed descending-`best_member_log_scores` permutation there makes the
first solutions found near-optimal and the floor near-final almost immediately.
Pairs naturally with item 4.

### 6. Per-symbol depth tail (optional)

`certificate_max_score` is indexed by rank x length, but the tail is not
per-symbol. A rarest-letter-aware tail would be tighter; items 1-3 should give
most of the win first.

## Measure before building

Compare `stats` between the two runs: `bound_prunes`,
`certificate.group_rejects` / `scans_skipped` / `scans_kept`, and total nodes.

- Prune rate collapses from `-g 5` to `-g 6` -> item 1 is the fix.
- Node counts comparable but scan volume dominates -> item 3 is the fix.

Worth the 15 minutes given the runtimes involved.

## Suggested order

1. Items 1 + 2 as one change (small, principled, strictly tightens every `-g`
   run).
2. Item 4a for immediate relief.
3. Item 3 if the numbers say scan volume.
