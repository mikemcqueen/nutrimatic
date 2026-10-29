# bag-stats-cuda --leftovers --hll on the GPU: plan review

A review of `plans/bag-stats-cuda-hll-gpu.md` against the code as of
9bf6e03: `source/bag-stats-cpu.cpp` (`count_leftovers`, `LeftoverSketch`),
`source/bag-stats-run.{h,cpp}` (`PickWalk`, `check_common_args`),
`source/bag-stats-cache.{h,cpp}` (`RemainderIndex`, `RestTable`),
`source/bag-stats-cuda.cpp` and `source/bag-stats-gpu.cu`.

## Status

All six findings are applied to `plans/bag-stats-cuda-hll-gpu.md`.

| # | Finding | Status |
|---|---------|--------|
| 1 | PICKS row 1 is never counted | applied: lane 0 adds 1 to `by_picks[0]` per fitting unit |
| 2 | bag-stats-cuda rejects `--ss pb,pb`, has no `--leftovers` | applied: new Options section |
| 3 | `--pp` support in the kernel is dead code | applied: pb-only kernel; ppb gets its own kernel later |
| 4 | cache-size N can exceed RestTable's cap | applied: prints `cache-size CAP (capped; N needed)` |
| 5 | `--ss pb` isn't described | applied: one warp per unit, lane 0 records it; `--ss pb` test |
| 6 | Shared atomics contend on `left` | applied: `__match_any_sync` groups, leader adds |

These parts of the plan hold up against the code:

- Keys. `RemainderIndex::number` is `Σ values[letter] × stride`
  (`bag-stats-cache.h:30`), linear in the letter counts, so
  `number(bag) − Σ number(pick)` equals `number(rest)` exactly whenever every
  pick fits. The bound passed to the index is `UINT64_MAX − 1`, so
  `number + 1` never wraps.
- Hash. `mix64` (`bag-stats-gpu.cu:52`) has the same constants and shifts
  as `mix_bits` (`mix-bits.h:7`). The plan hashes the 64-bit number with
  `mix64`, not `mix_key(Key128)`, which mixes differently; that's the right
  choice.
- Registers. `LeftoverSketch::add` takes the register index from the top
  `HLL_BITS` bits and the rank from the leading zeros of
  `(mixed << 14) | 1 << 13`, at most 51, so it fits a byte. `estimate` reads
  only the registers, and `merge` is a max, so a GPU `atomicMax` over 32-bit
  words, narrowed to bytes afterwards, gives the same estimate.
  2^14 × 89 × 4 bytes is 5.8 MB, well inside the 4080's 64 MB L2.
- Linking. `bag-stats-run.cpp` and `bag-stats-cache.cpp` already build into
  `bag_stats_run_lib`, which `bag-stats-cuda` links, so moving
  `LeftoverSketch` and the printing there is enough to share them.
- Inputs. `load_bag_stats_inputs` loads the inner dictionary only when
  `search.measured` is set, and `--leftovers` steps aren't measured, so the
  `-` for DICT works the same in both tools.
- Work order. With only pb steps, unit u's later picks all start past u, so
  low units really are the long ones and taking units in order puts the long
  units first.

## Bugs in the plan as written

### 1. PICKS row 1 is never counted

The CPU counts `by_picks` in `enter`, which `PickWalk::descend(step)` calls
as `enter(step − 1, rest)` before it tries the candidates for `step`
(`bag-stats-run.h:252`). The walk calls `descend(1, …)` once for every unit
whose first pick fits the bag (`bag-stats-run.h:221`), so:

- `by_picks[0]` counts every unit whose first pick fits;
- `by_picks[k]`, for `0 < k < steps − 1`, counts every fitting prefix of
  k + 1 picks;
- `by_picks[steps − 1]` is overwritten with the number of combinations
  (`bag-stats-cpu.cpp:1179`).

The plan only adds ballot popcounts "at every step past the first", which
covers `by_picks[1 .. steps − 2]`. Nothing adds to `by_picks[0]`. For
`--ss pb,pb` the output's `PICKS,COMBINATIONS` row `1,…` would be 0, so it
won't be byte-identical to `bag-stats`. The planned `pb,pb` test would catch
this.

Fix: when the warp takes a unit and the unit's first pick fits the bag,
lane 0 adds 1 to `by_picks[0]`. With one step, `by_picks[0]` is the last
entry and gets overwritten from the total, as on the CPU.

### 2. bag-stats-cuda rejects --ss pb,pb and has no --leftovers option

Three things stop `bag-stats-cuda --leftovers --hll --ss pb,pb` from
reaching `run_leftovers_gpu`, and the plan doesn't mention any of them:

- `bag-stats-cuda.cpp:94` fails any `--ss` without a wb step: "--ss needs a
  ppb or pb step and a wb step". That check has to skip `--leftovers`.
- `--leftovers` and `--hll` are options in `bag-stats.cpp`'s own table
  (`bag-stats.cpp:92`), not in `BAG_STATS_COMMON_LONG_OPTIONS`. `bag-stats-cuda`
  needs both in its table and usage text. `leftovers` is already a field of
  `BagStatsArgs`, so `check_common_args` already rejects the rank options
  for it (`bag-stats-run.cpp:78`).
- The check that `--leftovers` takes pb steps alone lives in `bag-stats.cpp`
  (`bag-stats.cpp:146`), next to the `--exact`/`--all`/`--completions`
  check. Without it in `bag-stats-cuda`, `--ss pb,ppb` or `--ss pb,wb` would
  reach the kernel. Since `leftovers` is common, the pb-only check can move
  into `check_common_args`, and both tools get it. The `--exact`/`--all`
  half stays in `bag-stats`, which has those options and `bag-stats-cuda`
  doesn't.

The plan's own rule, "`--leftovers` requires `--hll`" in `bag-stats-cuda`,
also needs a parse-time message, like `bag-stats`' "--hll goes with
--leftovers".

## Contradiction

### 3. The --pp support in the kernel is dead code

The plan says "`--leftovers` takes no ppb steps", and `bag-stats.cpp:147`
enforces that. Yet the kernel section carries `--pp` support:

- `step_list`, `list_offset` and `list_size` in `LeftoverParams`;
- "With `--pp`, steps point at different lists as on the CPU";
- the `repeats_bag` rule and a list index in every stack frame.

With pb steps alone, every entry of `inputs.steps` is `&pair_bags`
(`bag-stats-run.cpp:300`), so in `PickWalk::descend`:

- `steps_[step − 1] == steps_[step]` always holds, so every step starts at
  the previous pick + 1;
- `repeats_bag` (`bag-stats-run.cpp:349`) only compares picks from a
  different list, so it never fires.

The kernel can take one array each of keys, numbers, pairs and letters,
start every step past the previous pick, skip the repeat check, and drop
the list index from its frame. That's less shared memory per warp and one
fewer comparison loop per candidate. The plan should drop the `--pp`
sentence, or say why it keeps it.

## Needs a decision

### 4. cache-size N can exceed what RestTable will hold

`RestTable`'s capacity is
`min(rest_cache_entries(MB), index.total(), UINT32_MAX)`
(`bag-stats-cache.cpp:37`). Row indices are `uint32_t` (`rows_index_`), so
the cache never holds more than 4,294,967,295 entries:
4,294,967,295 × 48 bytes is 196,607 MB. `index.total()` (the number of
sub-bags of the bag) caps it too.

The plan's own figures go past that: the `$S1` `pb,pb` run has about 45.8G
distinct leftovers, more than 10× the cap, or about 2.1 TB at 48 bytes
each. `pb,pb,pb` has more. For those runs the `cache-size` line would name
a `--cache-size` that the table silently clamps, and the run would still
miss.

It's fine on the trimmed `$S1` bags the plan has in mind, as long as their
leftovers stay under the cap. The plan should say which of these it does:

- print `min(N, cap)` and add a note when it clamps, such as
  `cache-size 196607 (capped; N needed)`;
- print N anyway, and leave `report_rest_table`'s "cache full" message to
  tell the user at run time;
- raise the cap by widening `rows_index_` to 64 bits. That changes
  `REST_CACHE_ENTRY_BYTES` and every existing `--cache-size` meaning, so
  it's a separate plan.

`index.total()` needs no extra handling. When it's smaller than the
leftovers it can't be, since every distinct leftover has its own number
below `total()`.

## Smaller gaps

### 5. --ss pb (one step) isn't described

The kernel description assumes middle steps followed by "last step by
lanes". With `--ss pb` there are no middle steps and the unit is itself
the leaf: `PickWalk::walk` subtracts the first pick and `descend(1, …)`
calls `leaf` right away. `bag-stats --leftovers --ss pb` is legal, so
`bag-stats-cuda` has to handle it.

One warp per unit would give one combination per warp, with 31 lanes idle.
That's correct and only 593K units on `$S1`, so slow-but-correct is
acceptable. The other option is lanes striding the units directly when
there's one step. Either way the plan should say which, and the test list
could include a `--ss pb` comparison; it costs one line.

### 6. Shared-memory atomics at the last step contend on `left`

At the last step all 32 lanes share the same remainder, so
`left = rest letters − candidate letters` is the same for every lane whose
candidate has the same length. Each fitting lane does two 64-bit
`atomicAdd`s on shared `combinations[left]` and `rows[left]`, so lanes
with the same length serialize. Pair bags come in PAIRS file order
(`load_pick_bags` doesn't sort them), and their lengths span a narrow range
above `--min-pb-len`. A warp's 32 candidates will often share a handful of
lengths, so the serialization is several-way, on every combination.

The 5–50 hour estimate assumes the fit check is the bottleneck. To keep it
so, combine the lanes first: `__match_any_sync(fitting, left)` groups the
lanes that share a `left`; each group's leader adds the group's popcount
to `combinations[left]` and the group's summed rows (reduced with shuffles
or a loop over the mask) to `rows[left]`. The register update stays per
lane, since each lane's hash is different and the plain read before
`atomicMax` already skips most of them.

The same applies, less sharply, to `by_picks` at the middle steps, which
the plan already handles with one popcount per ballot.
