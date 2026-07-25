# Projected score-bound construction optimization

## Result

The projected recurrence should operate on projected actions, not concrete
anagram classes.

Several concrete classes can consume the same exact-letter multiplicities and
the same number of wildcard letters. They therefore have the same projected
key delta and the same child state. Only the largest class score can affect
the recurrence's maximum; every lower-scoring class with that delta is
dominated.

The implementation now builds one action per distinct projected delta and
forced-exact-letter bucket. It retains the highest class score for that action.
This changes preprocessing work without changing the projected bound.

## Measurements

All measurements use `idx/wiki-merged.5.index`, prefixes of `S6`,
`-m 4 -n 1000`, and a warm index. The 20-thread results use `-T 20`.

### 28 letters, `d=14`, `-C 8`

| builder | threads | setup | successful transitions |
|---|---:|---:|---:|
| concrete-class traversal | 1 | 21.929s | 292,735,991 |
| projected-action traversal | 1 | 11.706s | 213,302,595 |
| concrete-class traversal | 20 | 2.314s | 292,735,991 |
| projected-action traversal | 20 | 1.138s | 213,302,595 |

The optimized projected output is byte-identical to the complete exact dense
table's top-1000 output.

### 40 letters, `d=15`, `-C 32`, 20 threads

| builder | setup | search | phase 2 | successful transitions |
|---|---:|---:|---:|---:|
| concrete-class traversal | 226.549s | 20.207s | 246.757s | 27,133,454,068 |
| projected-action traversal | 29.736s | 20.776s | 50.512s | 5,919,322,956 |

The projection contains 151,440 distinct actions derived from 490,329 anagram
classes. Setup is 7.6x faster and total phase 2 is 4.9x faster. Final DFS node
and solution counts are unchanged: 342,949,072 nodes and 24,804 solutions.

At `d=16`, action deduplication reduces 490,329 classes to 222,643 actions.
The measured setup before the final instruction-level changes was 115.664s,
down from 510.705s, and transitions fell from 51.882 billion to 18.671
billion.

The output SHA-256 was identical for the measured `d=13`, `d=14`, `d=15`, and
`d=16` runs.

## Data layout and hot-loop changes

The action metadata is split into two 16-byte, 64-byte-aligned arrays:

- fit metadata: exact support, exact-requirement offset and counts, and
  wildcard length;
- value metadata: projected key delta and class score.

An unsuccessful fit test touches only fit metadata. Exact requirements are
copied into a projection-specific packed array, so the recurrence no longer
branches over wildcard requirements while mutating and restoring a worker
bag. Actions are stored contiguously by their rarest required exact letter,
with a separate wildcard-only bucket.

The temporary action storage is released after bound construction. It is not
part of the final DFS working set.

Callgrind attributed 70.3% of post-dedup instructions on the 24-letter serial
workload to the successful projected-transition kernel. Two smaller changes
target that path:

- compute the conservative floating-point error envelope once per state from
  separately conservative maximum magnitudes, instead of several arithmetic
  operations per edge;
- prevent GCC from partially inlining the recursive cache-miss path into the
  overwhelmingly common cache-hit transition path.

The host's WSL kernel has no usable `perf` PMU support, so hardware cache and
branch-miss counters were not available.

## Correctness argument

A projected flat delta uniquely identifies the exact-letter consumption vector
and wildcard consumption. Classes with the same bucket and delta reach the
same child from every state in which they fit. For child value `H`, their
candidate values differ only in class score:

```text
class_score + restart + H(child)
```

Retaining the maximum class score therefore preserves the recurrence exactly.

For rounding, the new per-state envelope uses the maximum class-score
magnitude and maximum child magnitude independently. Their sum is at least
the magnitude sum for every individual edge, so the resulting stored bound
remains upward-conservative.

## Remaining opportunities

The optimized builder is still transition-volume-bound. The most useful next
work is:

1. Replace "largest projection that fits" with an end-to-end shape selector.
   On the 40-letter workload, `d=16` uses the larger cache but is much slower
   overall than `d=15`. The measured `d=13`, `d=14`, and `d=15` phase-two
   totals were approximately 94.0s, 59.6s, and 50.5s respectively.
2. Prototype lazy projected construction after the score floor becomes
   available. The eager root recurrence computes abstract states that may
   never be queried by the concrete DFS.
3. Compare the recursive atomic scheduler with a total-letter-layered
   bottom-up scheduler. Layering eliminates dependency spinning and locked
   first-owner operations, but may compute more unreachable table cells.
4. Measure a one-exact-requirement specialization and narrower packed
   requirements. These are secondary until transition count falls further.
5. Explore arbitrary exact-letter masks or multiple wildcard groups. Better
   projection quality at the same state/action count could reduce both
   preprocessing and final DFS work.
