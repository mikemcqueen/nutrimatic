# Plan: bag-stats-cuda --unique counts pair usage on the GPU

## Outcome

`bag-stats-cuda` takes `--unique` with `--ss` of pb steps alone, as
`bag-stats` does without `--leftovers`: it prints each PAIRS row the
combinations use, by how many pair combinations it's in, most first, ties
by row; the first N with `-n N`; with `--count` (`--uc` for both) as
`COUNT left right`.

```
bag-stats-cuda --uc --ss pb[,pb...] [-n N]
    [-u LETTERS] [-s N | --no-nopairs] [--min-pb-len N] [--progress N]
    LETTERS PAIRS DICT
```

The walk is `walk_leftover_picks` with a per-pair-bag count added in place
of the leftover counts. `--ss pb,pb` on S1 −glllpu (1.6G combinations)
should take about a second of GPU time, where the `--leftovers --hll` walk
of the same combinations takes 0.7s; `pb,pb,pb` runs at about the
`--leftovers` walk's 12G combinations/s.

The output has the same rows, order and `--count` format as
`bag-stats --unique`: both print through `print_pair_usage` (below), and the
count and the sort are the same until a product of pair counts passes 2^64,
where `bag-stats` divides the wrapped product. The tests use that.

## The count

A combination is a set of distinct pair bags, one per step. It stands for
the product of their PAIRS: one row of pairs per choice of one pair from
each bag. A pair in bag `b` is in `rows / pairs[b]` of those rows, so its
count is

    usage[b] = Σ over combinations C containing b of Π_{c ∈ C, c ≠ b} pairs[c]

Every pair of `b` has `usage[b]`: a pair has exactly one letter bag, so the
walk only needs one count per pair bag, and the host expands it to rows.

Counts are `uint64_t` and wrap without a check. Overflow past 64 bits is a
known edge case we are not concerned with.

## Summing over the walk tree

The walk is a tree: depth d holds the pick of step d, a leaf is a full
combination. For a node at depth d with pick `b`:

- `P(node)`: the PAIRS product of the picks above it, `picked[d − 1].rows`
  (1 for a unit).
- `w(node)`: 1 at a leaf; otherwise Σ over its children `pairs[child] ·
  w(child)`.

Then `usage[b] += P(node) · w(node)` at every node, and summed over the
nodes of `b` that is the formula above. No division, and one add per node.

In `walk_leftover_picks`, with `below[GPU_MAX_PAIR_STEPS]` per thread
(warp-uniform: above the last step the whole warp walks one prefix):

1. **A pick at depth d** (the unit, or a middle step finding a candidate):
   `below[d] = 0`.
2. **The last step** from `picked[last − 1]`: each lane that fits candidate
   `i` does `atomicAdd(&usage[i], picked[last − 1].rows)` and adds
   `pairs[i]` to its own sum. The warp sums the lanes (64-bit
   `__shfl_xor_sync`), and that is `w` of `picked[last − 1]`, which is then
   finished.
3. **A middle step at depth d running out of candidates**: `picked[d − 1]`
   is finished with `w = below[d − 1]`.
4. **Finishing the node at depth d with `w`**: if `w` is nonzero, lane 0
   does `atomicAdd(&usage[picked[d].index], P · w)`; for d > 0,
   `below[d − 1] += pairs[picked[d].index] · w`.
5. **One step** (`--ss pb`): `start_unit` adds 1 to `usage[unit]` for each
   unit that fits.

`usage` is one `unsigned long long` per pair bag in sorted order, in global
memory: 0.2–0.5MB on the S1 lists, which stays in L2. The last step's add
is the only one per combination; everything above it is one per node.

## Kernel changes

```
enum class WalkCountType { LEFTOVERS, PAIR_USAGE };
```

`walk_leftover_picks` gets a `WalkCountType COUNT_TYPE` template
parameter, and `LeftoverParams` gets `unsigned long long* usage`:

- `LEFTOVERS`: today's kernel, unchanged.
- `PAIR_USAGE`: the sums above; no `add_combination`, `add_register`,
  `by_picks` or shared `WalkCounts` (0 bytes of shared memory). `last_step`
  returns the warp's `w`. `pick` skips `numbers` (`if constexpr`), and
  `numbers` is not uploaded.

`walk_leftovers` takes the count type, takes the pair bag count from
`inputs.pairs.size()` (`numbers` is empty for `PAIR_USAGE`), allocates
`usage` (count entries) in place of the leftover counts, and downloads it.

`bag-stats-gpu.h` gains:

```
// Walks the combinations run_leftovers_gpu walks and writes to `out`, one
// per pair bag in `inputs` order, how many PAIRS rows of them each of the
// bag's rows is in. Calls progress(units) as run_leftovers_gpu does. A
// CUDA call failing prints `program` and CUDA's error on stderr and exits
// 1.
void run_pair_usage_gpu(std::string_view program,
                        GpuLeftoverInputs const& inputs,
                        std::function<void(size_t)> const& progress,
                        std::vector<uint64_t>* out);
```

## Host changes

`bag-stats-cuda.cpp`:

- `OPT_UNIQUE` and `--unique`. `parse_args` sets `pair_usage` when
  `--unique` is given without `--leftovers`, before `check_common_args`,
  which already checks `--ss` and allows `-n` for it and rejects
  `--top-segments` ("--unique can't take --top-segments"). `--count` sets
  `pair_counts` and needs `--unique` ("--count goes with --unique").
  `--unique` with `--leftovers`: "--unique doesn't go with --leftovers".
  The `--leftovers` branch after `check_common_args` becomes `if
  (out->leftovers || out->pair_usage)`: it checks `GPU_MAX_PAIR_STEPS`,
  naming the mode in the error ("--unique takes at most 16 pb steps"), and
  returns before the wb-step checks.
- `run_leftovers`' input building moves into `leftover_inputs(inputs,
  index, &gpu, &sorted)`, which can't fail. `index` is the
  `RemainderIndex const*` that numbers the bags, null for `--unique`, which
  leaves `numbers` empty and `bag_number` 0. `run_leftovers` keeps building
  the index and its "too many sub-bags" check. `sorted` is the sorted
  position → pair bag map.
- `run_pair_usage(args, inputs)`: builds the inputs, runs
  `run_pair_usage_gpu` with the same progress as `run_leftovers`, maps the
  counts back to pair bag order through `sorted`, then ranks and prints
  them as below.
- `main`: `if (args.pair_usage)` → `stage("walking")`, `run_pair_usage`.
- Usage line and `--unique` help, as `bag-stats`' without the
  `--leftovers` part.

`bag-stats-run.{h,cpp}` (shared): `BagStatsPairUsage` moves here from
`bag-stats-cpu.h`, with

```
// Each row of `pair_rows` whose bag has a nonzero count in `counts`, one per
// pair bag, with that count: most first, ties by row, the first
// args.rank.top when it is set.
void rank_pair_usage(BagStatsArgs const& args,
                     std::vector<std::vector<std::string>> const& pair_rows,
                     std::vector<uint64_t> const& counts,
                     std::vector<BagStatsPairUsage>* out);

// Prints `used` as bag-stats --unique does. Returns the exit status.
int print_pair_usage(BagStatsArgs const& args,
                     std::vector<BagStatsPairUsage> const& used);
```

`list_pair_usage` ends with `rank_pair_usage`, and `count_pair_usage` calls
`print_pair_usage`, so `bag-stats` keeps its output.

## Tests

`test-bag-stats-cuda.sh`:

Each check reaches one more depth of the walk and prints counts (`--uc`).

- One step: `--uc --no-nopairs --min-pb-len 6 --ss pb` on
  `catdoglowanttan` with the test's `pairs`, as `test-bag-stats.sh`'s `--ss
  pb` check: `1 act god`, `1 cat dog`, `1 low ant`.
- The last step: `test-bag-stats.sh`'s `--uc --ss pb,pb` check,
  with the same expected output: `2 low ant`, `1 act god`, `1 cat dog`.
- The middle steps' sums: `--uc --no-nopairs --min-pb-len 2 --ss
  pb,pb,pb` on `abcdefghijk` with these pairs, four disjoint bags of 1, 2,
  2 and 3 pairs:

  ```
  a,b
  c,de
  d,ce
  f,gh
  g,fh
  i,jk
  j,ik
  k,ij
  ```

  Any three bags make a combination, four in all, so `ab` is in 2·2 + 2·3
  + 2·3 = 16 rows, `cde` and `fgh` in 2 + 3 + 6 = 11, and `ijk` in 2 + 2 +
  4 = 8:

  ```
  16 a b
  11 c de
  11 d ce
  11 f gh
  11 g fh
  8 i jk
  8 j ik
  8 k ij
  ```

No check runs the CPU `bag-stats`.

## Timing

**Do not run the CPU `bag-stats` for timing**, on `$S1SMAL` or any other
bag: no baseline runs, no samples. Only `bag-stats-cuda` is timed.

Check `pgrep -x` for `query-index`, `dfs-anagrams`, `bag-stats` and
`bag-stats-cuda` on the host first, then, for `--ss pb,pb` and
`pb,pb,pb`:

```
seed-pairs -s1 > $SP/seed.pairs
bag-stats-cuda --threads 0 -s1 -u '' --min-pb-len 13 --ss SS --unique \
    $S1SMAL $SP/seed.pairs $DICT
bag-stats-cuda --threads 0 -s1 -u '' --min-pb-len 13 --ss SS \
    --leftovers --hll $S1SMAL $SP/seed.pairs $DICT
```

The `--leftovers --hll` walk visits the same combinations, so the
difference is what the last step's global add costs next to the
HyperLogLog update it replaces.

Record the times in `findings/bag-stats-cuda-unique.md`.
