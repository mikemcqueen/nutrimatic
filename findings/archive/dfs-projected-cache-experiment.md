# Projected score-bound experiment

## Status

An opt-in first implementation of the single-wildcard projection is in the
working tree. It exists to compare complete projected coverage with the
current partial dense-prefix fallback before changing production policy.

Enable automatic selection of the largest corpus-rarest exact prefix that
fits the shared `-C` budget:

```sh
NUTRIMATIC_PROJECTED_SCORE=1 build/dfs-anagrams ... -C MiB -F
```

Force a particular number of exact letters for a sweep:

```sh
NUTRIMATIC_PROJECTED_SCORE_D=d build/dfs-anagrams ... -C MiB -F
```

The implementation:

- keeps exact multiplicities for the selected corpus-rarest letters;
- merges every other letter into one `wild_left` count;
- uses upward-rounded four-byte values;
- retains root-slab compaction when at least one exact letter is selected;
- eagerly constructs every reachable projected bound, sharing states between
  preprocessing threads; and
- leaves the final DFS and its concrete candidate traversal unchanged.

The old behavior remains the default, so the executable supports direct A/B
comparisons. The environment variables are experiment scaffolding, not a
proposed public interface.

## Initial correctness checks

- A forced `d=0` table on a 20-letter real-index input retained the same
  top-1000 output as the exact dense table. It used 21 logical slots and 128
  aligned bytes.
- A forced all-exact projection retained the same output, node count, bound
  transition count, and reachable bound-state count as the existing exact
  recurrence.
- The focused unit test now compares wildcard-only projected output with
  exhaustive output.
- Existing DFS search and CLI smoke tests remain unchanged and pass.

Every initial 24/26/28-letter projected run also produced byte-identical
top-1000 output relative to the dense-prefix run.

## Insufficient-cache A/B results

These use prefixes of `S6`, `-m 4 -n 1000`, and deliberately set `-C` below
the complete float table's minimum. The table uses one clean serial A/B pass
per configuration after the competing CPU-heavy job had stopped. All three
pairs produced byte-identical top-1000 output.

The timing columns come from the program's `std::chrono::steady_clock`
phase-2 measurements. GNU `time`'s elapsed clock was not usable on this host:
one short run reported negative elapsed time. Its `user` and `sys` fields are
CPU accounting, not values to add to wall time.

| letters | `-C` | complete float | mode | retained layout | setup | search | phase 2 | bound transitions | final DFS nodes |
|---:|---:|---:|---|---|---:|---:|---:|---:|---:|
| 24 | 1 MiB | 1,244,160 B | dense prefix | 262,144 exact slots | 0.003s | 1.662s | 1.665s | 28,503,059 | 4,327,608 |
| 24 | 1 MiB | 1,244,160 B | projection | `d=12`, 124,416 slots / 497,664 B | 1.280s | 0.067s | 1.348s | 18,890,881 | 1,375,467 |
| 26 | 1 MiB | 2,985,984 B | dense prefix | 262,144 exact slots | 0.005s | 66.353s | 66.358s | 64,451,824 | 1,444,709,401 |
| 26 | 1 MiB | 2,985,984 B | projection | `d=12`, 114,048 slots / 456,192 B | 0.359s | 0.195s | 0.554s | 43,366,785 | 4,207,855 |
| 28 | 8 MiB | 8,957,952 B | dense prefix | 2,097,152 exact slots | 0.014s | 30.676s | 30.690s | 551,739,555 | 30,174,286 |
| 28 | 8 MiB | 8,957,952 B | projection | `d=14`, 839,808 slots / 3,359,232 B | 2.314s | 0.155s | 2.469s | 292,735,991 | 3,086,650 |

The 26-letter case is the clearest early signal. Complete projected coverage
reduced phase 2 by about 120x and the final traversal by 343x while also
reducing bound construction edges by about one third. The 28-letter case
improved phase 2 by 12.4x; the 24-letter case improved it by 1.24x. This is the
failure mode the design was intended to fix: a large exact prefix still leaves
unbounded interior nodes, while the smaller projection supplies a bound
everywhere.

## Prefix-depth sweep

The 26-letter, 1 MiB workload gives the following single-run tradeoff. The
automatic `d=12` result above supplies the `d=12` row; it was not repeated.

| `d` | projected bytes | setup | search | phase 2 | bound transitions | final DFS nodes |
|---:|---:|---:|---:|---:|---:|---:|
| 8 | 19,584 | 0.122s | 1.129s | 1.251s | 6,099,641 | 23,821,777 |
| 10 | 69,120 | 0.142s | 0.568s | 0.710s | 11,859,394 | 12,336,557 |
| 11 | 179,712 | 0.227s | 0.332s | 0.559s | 22,718,433 | 7,277,778 |
| 12 | 456,192 | 0.359s | 0.195s | 0.554s | 43,366,785 | 4,207,855 |

Larger `d` tightens the bound and reduces final DFS work, but increases
abstract setup work. Here `d=11` and `d=12` are essentially tied end to end,
despite `d=12` doing nearly twice as many setup transitions. The production
selector therefore should account for setup cost instead of blindly consuming
the whole cache budget.

## 28-letter cache cliff

The existing 8 MiB dense-prefix run completed phase 2 in 30.690s and visited
30,174,286 final DFS nodes. Reducing its prefix cache to 4 MiB made phase 2
take 212.812s and visit 4,035,783,694 nodes. The requested stop arrived just
after that baseline exited; no 4 MiB projected counterpart or further
benchmark was run.

## Current conclusion

The staged small-input experiment is strong enough to continue developing
the projection. Complete abstract coverage can eliminate the catastrophic
runtime cliff caused by an incomplete exact prefix, and it does so with much
less memory than the full dense table.

The next implementation question is table-shape selection. `d=11` versus
`d=12` shows that maximizing exact dimensions is not automatically best.
Before trying 30--32 letters, add a cheap selector or calibration rule that
balances projected preprocessing work against the tighter final-search bound.
