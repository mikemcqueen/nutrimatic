# Search exact remainders by descending removed-class length

## Status

Tested and rejected. The stable counting-sort prototype did not improve the
50-character workload repeatably, so it was removed.

This is specifically about the batch exact-validation order in
`DfsAnagramSearch::find_completable_classes()`. It is not the score-based
candidate ordering previously measured and rejected for ordinary ranked
`dfs-anagrams` in `findings/dfs-codex-perf.md`.

## Result

The prototype used the `uint32_t` order vector described below. Two descending
order runs bracketed a control run that retained construction of the vector but
claimed the original bucket-order class index. That control isolates scheduling
from the vector's allocation and setup cost.

| validation order | exact validation | whole command | user CPU | exact states | memo hits |
|---|---:|---:|---:|---:|---:|
| descending length | 63.2 s | 79.39 s | 1177.08 s | 7,397,046 | 35,147,399 |
| original bucket order control | 62.7 s | 79.18 s | 1174.50 s | 7,397,043 | 35,147,995 |
| descending length | 62.1 s | 76.51 s | 1158.42 s | 7,397,046 | 35,147,467 |

Peak RSS was 1,422,008 KiB for both descending runs and 1,422,016 KiB for the
control. The control deliberately retained the 19.6 MiB vector; production
bucket order does not pay that memory.

The descending runs straddled the control, and their exact-state and hit counts
were effectively unchanged. There is no repeatable wall-time improvement to
justify four additional bytes per class, so the decision rule at the end of
this document says to remove the change. Production remains bucket-major.

## Workload and current baseline

The motivating workload is:

```bash
source ./setup.sh
query-index "$IDX" "${S1:0:50}" \
  -n0 -m4 --require-completable -S20
```

Before timing, check that no other `query-index` process is running:

```bash
pgrep -af -x query-index
```

Measured before the exact-memo rewrite and long-input cache policy:

| phase | wall time |
|---|---:|
| phase 1 | about 11 s |
| projected score-bound setup | 30.5 s |
| exact validation | 67.7 s |
| whole command | 111.7 s |

The projected table contained 15,335,424 states but made no direct decision
for any of the 5,145,575 candidate classes:

```text
phase 2 completability: 5145575 classes checked,
0 rejected by bounds, 0 accepted by exact bounds,
5145575 exact validations
```

Disabling that table reduced the whole command to 85.2 s. Replacing the
sharded `unordered_map` memo with the exact flat atomic memo, and skipping the
automatic projected table for long completability queries, produced:

| phase | wall time |
|---|---:|
| phase 1 | about 9 s |
| phase-2 setup | 1.6 s |
| exact validation | 59.4 s |
| whole command | 76.2 s |

The flat memo used 16,777,216 eight-byte slots (128 MiB), computed about
7.40 million exact states, and served about 35.15 million memo hits.

## Current validation order

`find_completable_classes()` assigns work through one atomic class cursor:

```cpp
size_t const class_index =
    next_class.fetch_add(1, std::memory_order_relaxed);
```

That cursor follows `DfsClassList::classes()` order. Classes are grouped by
their forced rarest-letter bucket. Within a bucket they retain descending
class-length order, but length is not globally descending across buckets.

For each class `c`, validation asks whether this exact remainder is reachable:

```text
full input bag - letters(c)
```

Removing a longer class produces a smaller remainder. Removing a shorter
class produces a larger remainder whose recursive search descends through
smaller remainder states.

## Hypothesis

Validate classes globally from longest to shortest.

This should populate the shared exact memo with small, cheap remainder states
before workers begin the larger remainder searches that can reuse them.
The intended dependency direction is:

```text
long removed class
    -> small exact remainder solved early
    -> memoized descendant
    -> reused by later, larger remainder searches
```

The current bucket-major order repeatedly starts a new rarest-letter bucket
with long classes only after earlier buckets have also processed their short
classes. That can delay useful small states and leave expensive validations
near the end of the batch.

The 50-character progress trace arrived in pronounced waves, with long gaps
around several bucket boundaries and a parallel tail after most workers had
finished. That behavior is consistent with the hypothesis, but does not prove
it. Progress counters are worker-local and reported in coarse increments, so
the trace is only a reason to test ordering, not evidence of the cause.

## Why this differs from rejected score ordering

`findings/dfs-codex-perf.md` measured sorting ordinary DFS choices by:

```text
class score + score bound of child bag
```

That ordering reduced nodes but found a worse spelling-heap floor, increased
spelling expansion, and slowed ranked output. It was correctly rejected.

Batch completability has none of those concerns:

- it does not maintain a top-N score floor;
- it does not expand class paths into spellings;
- it asks one boolean question per class;
- output order is reconstructed later from the class-parallel result bitmap;
- changing validation order cannot change ranking or which classes survive.

The proposed order is about memo dependency size, not score.

## Correctness

Validation order is not semantically observable.

Each class still receives exactly one top-level validation. The shared memo
stores an exact boolean keyed by the complete remaining letter bag. The
recurrence and class candidates within each exact state remain unchanged.
After validation, results must still be written by original `class_index`:

```cpp
results[class_index] = result;
```

The returned `completable` bitmap therefore remains parallel to
`DfsClassList::classes()`, regardless of scheduling order.

The important invariant is:

> The scheduling structure contains every original class index exactly once.

No class records, candidate buckets, member ranges, or output tie-breaks should
be reordered in place.

## Implementation options

### Option 1: counting-sort an index vector

This is the preferred first prototype.

`DfsClassRecord::key_length` is an eight-bit field and the input bag is capped
at 128 letters, so ordering does not require a comparison sort.

1. Count classes by `key_length`.
2. Prefix-sum the counts in descending-length order.
3. Fill a `std::vector<uint32_t>` containing every original class index.
4. Have the existing atomic cursor claim positions in that vector.
5. Translate the claimed position to the original class index.

Conceptually:

```cpp
std::array<size_t, DFS_MAX_BAG_LETTERS + 1> counts = {};
for (size_t ci = 0; ci < classes.size(); ++ci)
  ++counts[classes[ci].key_length];

std::vector<uint32_t> validation_order(classes.size());
// Fill length DFS_MAX_BAG_LETTERS down through zero.

size_t const position =
    next_class.fetch_add(1, std::memory_order_relaxed);
size_t const class_index = validation_order[position];
```

At 5,145,575 classes, a `uint32_t` order vector costs about 19.6 MiB. Class
count is already constrained to `UINT32_MAX` by phase-2 preparation.

Advantages:

- linear construction time;
- deterministic;
- small implementation;
- preserves the existing dynamic one-class work distribution;
- easy to remove if the benchmark is negative.

Costs:

- one extra pass over the class records;
- roughly four bytes per class during validation;
- another memory stream before starting the workers.

### Option 2: per-length atomic cursors

Avoid the order vector by giving workers a cursor for each length and making
them drain lengths from longest to shortest.

This is awkward because equal-length classes are not globally contiguous:
class storage is rarest-bucket-major. It would need either:

- one cursor per `(length, rarest bucket)` range plus range discovery; or
- repeated scans over all rarest buckets.

That is more bookkeeping than the index vector and creates extra shared
cursor contention. It should not be the first prototype.

### Option 3: length bands rather than strict ordering

Use a few bands, for example:

```text
33+ letters, 25-32, 17-24, 9-16, 0-8
```

Workers finish one band before moving to the next. This uses less ordering
metadata if bands can be generated cheaply and may offer enough dependency
direction without a full index vector.

It is a fallback experiment only if the strict order helps but its setup or
memory cost is objectionable.

## Tie order within one length

The first prototype should preserve original class order within equal lengths.
A stable counting sort provides that automatically.

There is no correctness requirement for the tie order, but retaining it makes
the experiment narrow and reproducible. Do not mix in score, rarest-rank,
support-size, or estimated branching heuristics until the length-only result
is known.

## Expected effects

Possible wins:

- earlier population of small exact remainder states;
- more memo hits near the roots of later validations;
- fewer duplicate computations caused by concurrent first misses;
- less time with only one or a few long-running workers at the end;
- smoother utilization across the requested `-S20` workers.

Quantities that may remain unchanged:

- number of classes checked;
- output and surviving class count;
- the set of exact memo keys eventually stored;
- score-bound decisions;
- phase-1 time.

Quantities that can legitimately change with scheduling:

- exact states computed, because racing workers may discover overlapping
  states in a different order;
- exact memo hits;
- nodes visited;
- CPU time and worker utilization;
- progress-message timing and ordering.

## Reasons it may not help

1. **The memo already converges on the same states quickly.**
   If nearly all useful descendants are discovered within the first few
   bucket ranges, global ordering adds setup and memory traffic without
   increasing reuse.

2. **Small remainders may not be the expensive states.**
   They are cheaper individually, but the dominant cost may be scanning large
   candidate buckets in middle-sized states that length ordering does not
   predict.

3. **The search short-circuits on the first completion.**
   A larger remainder may be cheap when its first candidate path completes,
   while a smaller dead remainder may require an exhaustive search.
   Remainder size is only a heuristic.

4. **Memory bandwidth may already dominate.**
   The flat memo reduced synchronization almost completely, but the
   50-character run still averaged about 15 of 20 cores. Adding a 20 MiB order
   vector and another class-record pass could compete for the same bandwidth.

5. **Global length order may synchronize hard cases.**
   Workers could enter similarly shaped dead-end searches at the same time,
   increasing duplicate first-miss work before any one worker publishes the
   result.

## Benchmark plan

Keep tests and performance experiments separate.

Before each timing:

```bash
pgrep -af -x query-index
```

Use the exact requested workload and redirect stdout so terminal I/O is not
measured:

```bash
source ./setup.sh
/usr/bin/time -v \
  ./build/query-index "$IDX" "${S1:0:50}" \
  -n0 -m4 --require-completable -S20 \
  > /dev/null
```

Run baseline and prototype more than once in alternating order if host load is
variable. Record:

- phase-1 time;
- phase-2 setup time, including construction of `validation_order`;
- exact-validation wall time;
- whole-command wall time;
- user and system CPU;
- peak RSS;
- exact memo states and hits;
- nodes visited;
- voluntary and involuntary context switches.

Also test:

| input | purpose |
|---|---|
| `${S1:0:40}` | quick iteration and regression signal |
| `${S1:0:50}` | primary comparison |
| a longer feasible S1 prefix | confirm scaling direction |
| dead-end-heavy bag | ensure size order does not synchronize exhaustive work |
| `-S1`, `-S4`, `-S20` | separate memo reuse from parallel-tail effects |

The primary success criterion is lower exact-validation wall time at 50
letters without a material peak-RSS regression. A node-count reduction alone
is insufficient if setup or memory traffic makes wall time worse.

## Correctness checks

Keep coverage minimal:

1. Run `dfs-search`, `dfs-cli`, and `query-index-cli`.
2. Hash complete output at a manageable real-index size with `-S1` and `-S20`;
   hashes must match.
3. Compare prototype output against the current class-order implementation.
4. Exercise the synthetic dead-end class case in `test-query-index.sh`.
5. Assert in a debug build that `validation_order` contains each class index
   exactly once.

Suggested smoke command:

```bash
source build/dep-info/conanbuild.sh
export IDX=~/code/nutrimatic/idx/wiki-merged.5.index
meson test -C build dfs-search dfs-cli query-index-cli --print-errorlogs
```

## Decision rule

Keep the change only if the 50-character exact-validation wall time improves
repeatably after including order-construction time.

If it is neutral or negative, remove it rather than layering on additional
tie-break heuristics. If it wins, next isolate why:

1. compare exact-state and hit counts;
2. measure the final-worker tail;
3. test length bands;
4. only then consider in-progress memo ownership or more elaborate scheduling.
