# Faster phase-2 exact completability search

## Scope

This note follows the rejected experiment in
`findings/search-by-descending-length.md`.

The workload of interest is the batch exact-validation phase used by:

```bash
query-index "$IDX" LETTERS \
  -n0 -m4 --require-completable -S20
```

For every extracted anagram class `c`, phase 2 asks whether the exact
remainder:

```text
full input bag - letters(c)
```

can be tiled by extracted classes. The answer is an exact boolean. Candidate
class order, completion spelling order, and score ranking are not observable
for this operation.

The immediate motivation is the visibly uneven progress trace described as
"waves" in `search-by-descending-length.md`. Globally scheduling removed
classes by descending length did not make those waves disappear or improve
the 50-character workload repeatably. This note revises the likely diagnosis
and collects more promising ways to improve the exact DFS.

The original analysis made no source changes. Follow-up work has since:

- implemented candidate optimization 1;
- used temporary decision counters for candidate optimization 2;
- used weighted losing-expansion counters to decide whether candidate
  optimization 3 merits a prototype; and
- removed the experimental counters after recording their results.

## Current algorithm

`DfsAnagramSearch::find_completable_classes()` prepares one shared phase-2
search and one shared exact memo table. Workers claim top-level classes through
one atomic cursor:

```cpp
size_t const class_index =
    next_class.fetch_add(1, std::memory_order_relaxed);
```

Each worker subtracts the claimed class from the full bag and calls:

```cpp
exact_remainder_completable(worker, remaining_length)
```

The recursive exact search:

1. looks up the complete exact mixed-radix remainder key in the shared memo;
2. asks the optional score-bound table for a conclusive reachability answer;
3. chooses the globally rarest remaining symbol;
4. scans the class-list bucket whose classes have that symbol as their rarest;
5. tests each candidate for exact fit;
6. subtracts a fitting candidate and recursively asks about the child;
7. stops on the first true child;
8. publishes the final boolean in the shared exact memo.

The flat memo is an atomic, open-addressed table with one eight-byte slot per
entry. A slot contains an exact key and its final true/false value. It has no
in-progress state: an entry becomes visible only after its recursive search
finishes.

Class metadata already has the important first-line fit optimizations:

- a support mask rejects classes requiring absent symbols;
- packed requirements check only multiplicities greater than one after the
  support test;
- class buckets are sorted by descending length;
- a binary search skips candidates longer than the current remainder;
- both exact and projected keys are maintained incrementally.

## Revised diagnosis of the progress waves

The current progress counter is not a work counter.

`exact_remainder_completable()` increments `worker->nodes` only after:

- an exact memo miss; and
- an inconclusive score-bound lookup.

It increments immediately before scanning the state's candidate bucket.
Consequently, none of the following advances the node counter:

- candidate classes examined;
- support-mask or multiplicity rejections;
- fitting child transitions that immediately hit the exact memo;
- top-level classes answered directly by the memo;
- hash-table probes;
- time spent scanning a large or unfavorable bucket.

One counted node can therefore mean a nearly empty bucket and no useful
children, while another can mean a long scan followed by many memo-hit
transitions. Reporting every 100,000 cache-miss states naturally produces
uneven time intervals even when CPU utilization is healthy.

### Single-thread evidence

A directional 40-letter run with one search thread still progressed in clear
waves:

| progress interval | approximate elapsed time |
|---|---:|
| validation start to 100k nodes | 11 s |
| 100k to 200k | 4 s |
| 200k to 300k | 3 s |
| 300k to 400k | 13 s |
| 400k to 500k | 5 s |
| 500k to 600k | 5 s |
| 600k to 700k | 9 s |
| 700k to 800k | 5 s |
| 800k to 900k | 9 s |
| later 100k intervals | about 6--7 s |

This eliminates parallel top-level scheduling as the fundamental source of
the waves. Parallel execution can add a final-worker tail and concurrent
first-miss duplication, but neither is required to produce the uneven trace.

### Parallel scaling evidence

On the same 40-letter input, using a deliberately shallow projected table:

| search threads | exact validation |
|---:|---:|
| 1 | 89.4 s |
| 20 | 5.2 s |

This is about 17.2x wall-time speedup on 20 requested workers. Exact states and
memo hits were nearly unchanged:

| search threads | exact states | memo hits |
|---:|---:|---:|
| 1 | 1,295,755 | 3,299,361 |
| 20 | 1,295,758 | 3,300,562 |

The three-state difference is normal scheduling-dependent publication
behavior. The strong scaling says the one-class atomic cursor already
distributes the bulk of the workload well. More elaborate scheduling should
target a measured tail, not the visual waves themselves.

### What the waves probably represent

The most likely contributors are:

1. **Unequal bucket scan volume.** Rarest-letter buckets differ substantially
   in size, and the number of candidates surviving length and support checks
   varies with the current exact bag.

2. **Memo-hit transitions are invisible.** A state can examine many fitting
   candidates whose children are already known false without adding a node.

3. **Boolean short-circuit behavior is irregular.** A true state may find a
   witness immediately or only after many unknown and known-false children. A
   false state must exhaust every fitting child.

4. **Static rarity is only a proxy for constraint tightness.** The globally
   rarest remaining letter need not have the smallest viable candidate set in
   a particular remainder.

5. **Progress boundaries can align with bucket-major top-level order.** That
   can make bucket transitions visually prominent without making scheduling
   order the underlying cost.

The practical conclusion is:

> Do not optimize for a smooth cache-miss-node trace. Measure and reduce
> candidate scans, memo-hit transition cost, duplicate expansions, and the
> actual final-worker tail.

## Score-bound cache policy is a separate regression

The automatic projected score table is poorly calibrated for existence-only
completability, even below the current 50-letter cutoff.

Directional measurements on `${S1:0:40}` were:

| configuration | bound setup | exact validation | exact states | memo hits | whole command | user CPU | peak RSS |
|---|---:|---:|---:|---:|---:|---:|---:|
| default 64 MiB, `-S20` | 16.1 s | 5.1 s | 1,295,758 | 3,207,644 | 36.81 s | 195.95 s | 636,468 KiB |
| `-C1 -S20` | 0.9 s | 5.2 s | 1,295,758 | 3,300,562 | 23.99 s | 65.20 s | 581,792 KiB |
| `-C1 -S1 -T1` | 1.1 s | 89.4 s | 1,295,755 | 3,299,361 | 98.16 s | 95.29 s | 582,212 KiB |

These are single directional runs, not alternating benchmark-quality samples,
but the difference is too large to dismiss:

- the 64 MiB projection prepared 5,013,504 states in 16.1 seconds;
- the 1 MiB projection prepared 119,808 states in about one second;
- neither configuration directly rejected or accepted any of the 1,121,661
  top-level classes;
- the rich table did not reduce the exact-state count;
- exact-validation wall time was unchanged;
- the rich table added about 15 seconds and substantial CPU time.

The current default changes abruptly at 50 letters: automatic projection is
enabled below 50 and disabled at 50 or above. The 40-letter result shows that
the cliff is not aligned with the actual setup-versus-validation tradeoff.

For completability, the default should instead be one of:

- a deliberately shallow and cheap projection;
- a reachability-specific preprocessing budget;
- no score projection unless a sample predicts enough conclusive rejection;
- a separate boolean projection design, described below.

Score-rich preprocessing remains useful for ranked `dfs-anagrams`; this
finding is specific to existence-only exact validation.

## Measurements needed before the next large change

Add counters that expose the work currently hidden by the progress trace.
Keep them out of normal builds or diagnostics once the experiments are
settled.

### Per-worker totals

- exact memo lookup calls;
- memo hits split into true and false;
- memo misses;
- hash probes;
- exact recursive expansions attempted;
- exact states successfully inserted;
- candidate classes examined;
- length rejections;
- support-mask rejections;
- multiplicity rejections;
- fitting transitions;
- children decided by the exact memo;
- children decided by a score/reachability bound;
- children recursively expanded;
- true states and false states;
- top-level classes completed.

The most important concurrency quantity is the explicit count of expanded
states whose final memo store finds that another worker has already published
the same key. Do not infer it from:

```text
recursive expansions attempted - unique states successfully inserted
```

Bound-decided states can be inserted without recursive expansion, a full table
can reject expanded-state stores, and the previously fixed insertion race
could create duplicate slots for one key. Weighted losing-expansion counters
must accompany the event count because a small number of losing states can
still contain disproportionate candidate-scan or descendant work.

### Bucket/rank totals

Record, for each forced rank:

- states expanded;
- bucket candidates examined;
- fitting transitions;
- true/false states;
- wall or CPU samples if cheap;
- average and high-percentile candidates per state.

This will test whether the wave boundaries correspond to a few expensive
buckets and identify which indexes could help.

### Scheduling and tail totals

Record:

- time at which the last top-level class is claimed;
- time at which every worker finishes;
- active-worker count over coarse time intervals;
- longest top-level validation;
- a small histogram of per-class work or duration.

The difference between last claim and final completion is the indivisible
recursive tail that internal task splitting could address.

### Better progress reporting

If smoother and more informative diagnostics are desirable, report one or
more of:

- top-level classes completed;
- candidate classes examined;
- unique exact states published;
- active workers.

Cache-miss nodes alone should not be presented as a linear measure of
completion.

## Candidate optimization 1: probe the child memo before mutation — done

Implemented in `DfsAnagramSearch::exact_remainder_completable()`. Fitting
children now probe both the exact memo and reachability bound before the
worker's bag, mask, and keys are mutated.

This is the narrowest and highest-confidence next prototype.

Today the candidate loop does:

```text
test class fit
subtract every required symbol
update bag mask
subtract exact key
subtract projected score key
recurse
    look up the child exact key
restore every required symbol
restore mask and keys
```

This full sequence is paid even when recursion immediately finds a memoized
child. The 50-letter workload reported about 35.15 million exact memo hits.

The child exact key is already available without changing the bag:

```text
child_exact_key = worker.exact_key - class.signature
```

Refactor exact memo lookup to accept an explicit key. For each fitting
candidate:

```text
if candidate consumes the whole remainder:
    return true

look up child_exact_key

if child is known false:
    continue

if child is known true:
    return true

subtract the class, recurse, and restore
```

When a projected bound exists, its child key is also known without mutation:

```text
child_score_key = worker.score_key - score_key_delta[class]
```

A conclusive reachability result can likewise avoid the bag walk.

### Expected effects

- eliminate subtract/restore work for exact memo hits;
- eliminate score-key updates on those hits;
- reduce writes to worker-local bag cache lines;
- preserve candidate order and the set of recursively expanded first misses;
- preserve every semantic and output invariant.

### Decision rule

Keep it if exact-validation wall time improves repeatably. It is valuable even
if state and hit counters remain identical; those counters describe decisions,
not the removed mutation work.

## Candidate optimization 2: memo-aware candidate traversal

The current one-pass loop can recursively solve an expensive unknown child
even though a later fitting candidate has a child already memoized true.

A memo-aware traversal would prioritize existing knowledge:

1. scan fitting candidates;
2. return immediately for any known-true child;
3. discard known-false children;
4. retain unknown child IDs;
5. recursively test only the unknown children.

Variants include:

- a full two-pass scan;
- buffering unknown IDs in a small local vector;
- remembering only the first few unknowns before continuing to search for a
  known true;
- enabling the second pass only after the memo reaches a maturity threshold.

This can reduce new state discovery and shorten positive states, but it adds
hash lookups and may scan a bucket twice.

Before implementing it, add a shadow counter:

```text
states where an unknown fitting child precedes a later known-true child
```

Also count how many unknown candidates would need buffering. If the event is
rare, retain the simpler child-probe optimization only.

Temporary shadow instrumentation left candidate order and recursive decision
logic unchanged. The saved 50-letter run in `results/idx.s1.50.stderr`
reported:

| quantity | count |
|---|---:|
| recursive expansions attempted | 7,397,167 |
| states with a later known-true child | 2,126,781 |
| unknown candidates buffered by the shadow model | 21,615,009,205 |
| exact-validation wall time with shadow instrumentation | 336.3 s |

Thus 28.8% of recursive expansions had a later known-true opportunity. This
is frequent enough to justify a memo-aware traversal prototype. The very
large buffering total argues against an unbounded candidate buffer, and the
shadow run's cost compared with the 68.9-second non-shadow comparison run
shows that the prototype must be measured without the experimental counters.
The instrumentation was removed after recording these results.

Candidate order is not observable for exact boolean validation, so this
experiment does not share the ranked-output risk of the rejected score-based
ordering in `dfs-codex-perf.md`.

### Bounded traversal prototype — kept at width 16

Implemented the bounded design from
`plans/fast-p2-search-optimization-2.md`. The exact child classifier checks,
without mutating the worker:

1. whole-remainder fit;
2. the exact child memo key;
3. the projected child reachability key; and
4. publishes conclusive projected answers in the exact memo.

`NUTRIMATIC_EXACT_MEMO_LOOKAHEAD=0` retains the immediate-recursion
optimization-1 control. Decimal widths from 1 through 64 select the bounded
path; invalid and oversized settings use the default. The selected default is
16 IDs, or 64 logical bytes per window; the fixed maximum array reserves 256
bytes per active lookahead frame. The control lives in a separate helper, so
width 0 does not allocate that array.

All timing runs below first checked the host process table, outside the
sandbox PID namespace, for both `query-index` and `dfs-anagrams`. Two early
runs made before that distinction was noticed were discarded. Final commands
used the repository setup and `/usr/bin/time -v`:

```bash
source ./setup.sh
pgrep -ax query-index
pgrep -ax dfs-anagrams
/usr/bin/time -v env NUTRIMATIC_EXACT_MEMO_LOOKAHEAD=WIDTH \
  ./build/query-index "$IDX" "${S1:0:50}" \
    -n0 -m4 --require-completable -S WORKERS
```

#### Intrinsic 50-letter results (`-S1`)

Each width was measured once because the control alone takes nearly 15
minutes in exact validation. All four outputs had SHA-256
`bf8d557baa98d619e8f9024bcf371d0b8faef3365a1f44e7d60dae10e82f6eae`.
Requested and actual search workers were both 1.

| width | phase 1 | setup | exact validation | whole wall | user | system | peak RSS KiB | exact states | memo hits |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 9 s | 1.5 s | 883.7 s | 904.56 s | 893.34 s | 0.93 s | 1,420,660 | 7,397,041 | 35,144,780 |
| 4 | 8 s | 1.4 s | 359.8 s | 379.13 s | 378.44 s | 0.56 s | 1,420,656 | 6,662,633 | 24,870,196 |
| 16 | 8 s | 1.4 s | 331.9 s | 351.27 s | 350.56 s | 0.69 s | 1,420,660 | 6,255,762 | 19,246,146 |
| 64 | 8 s | 1.4 s | 361.1 s | 380.53 s | 379.90 s | 0.61 s | 1,420,632 | 6,122,527 | 17,439,850 |

The wider windows continue reducing states and hits, but width 64 spends more
time scanning and probing. Width 16 is the intrinsic winner: 62.4% less exact
validation time than control.

| width | full windows | known-true wins | re-probes decided | buffered recursive expansions |
|---:|---:|---:|---:|---:|
| 4 | 1,449,116 | 1,561,041 | 0 | 1,517,058 |
| 16 | 1,066,605 | 2,780,337 | 0 | 1,110,187 |
| 64 | 945,794 | 3,206,928 | 0 | 976,952 |

The zero single-worker re-probe decisions mean the benefit comes from finding
already-known true children later in a filling window, not from earlier
recursive children resolving their buffered siblings.

#### Primary 50-letter results (`-S20`)

Runs alternated control and prototype widths. Requested and actual workers
were both 20, and every output had the same SHA-256 as the `-S1` runs.

| trial | width | setup | exact validation | whole wall | user | system | peak RSS KiB | exact states | memo hits |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| control A | 0 | 1.5 s | 50.4 s | 69.62 s | 1,022.23 s | 1.54 s | 1,420,800 | 7,397,041 | 35,147,526 |
| width 4 | 4 | 1.4 s | 41.9 s | 60.98 s | 851.30 s | 1.32 s | 1,420,512 | 6,662,709 | 24,872,035 |
| control B | 0 | 1.4 s | 51.3 s | 70.72 s | 1,040.34 s | 1.29 s | 1,420,336 | 7,397,041 | 35,147,756 |
| width 16 A | 16 | 1.4 s | 38.7 s | 58.06 s | 789.60 s | 1.27 s | 1,420,660 | 6,255,887 | 19,248,332 |
| control C | 0 | 1.4 s | 51.5 s | 70.83 s | 1,043.08 s | 1.42 s | 1,420,696 | 7,397,041 | 35,147,711 |
| width 64 | 64 | 1.4 s | 41.2 s | 60.19 s | 838.31 s | 1.18 s | 1,420,648 | 6,122,666 | 17,441,839 |
| width 16 B | 16 | 1.4 s | 38.8 s | 58.37 s | 791.18 s | 1.28 s | 1,420,568 | 6,255,884 | 19,248,871 |

The three controls span 50.4--51.5 seconds. Width 16 repeated at 38.7 and
38.8 seconds, a 23.9% reduction from the 50.9-second control mean. Width 16
also beats widths 4 and 64, so it is the selected default.

| width/trial | full windows | known-true wins | re-probes decided | buffered recursive expansions |
|---|---:|---:|---:|---:|
| 4 | 1,449,286 | 1,559,622 | 38 | 1,517,240 |
| 16 A | 1,066,794 | 2,779,317 | 63 | 1,110,373 |
| 64 | 945,987 | 3,206,555 | 43 | 977,138 |
| 16 B | 1,066,771 | 2,779,566 | 40 | 1,110,375 |

#### Shorter-workload regression checks

The 40-letter check used the normal projected-bound policy and 20 requested
and actual search workers. Width 16 retained output hash
`e24027c87188c0852e2ee8efd9545fd7e91c68e39c39c3ae1caad8258da7ff17`
and slightly improved both exact validation (1.4 versus 1.5 seconds) and
whole-command wall time (10.03 versus 10.52 seconds).

The 32-letter bound-off check used `-C0 -F`. Width 16 retained output hash
`c300bc95e4b446abcc670ab62b3a0c48e4ca69aae7c2e87cd403db665335e296`.
With 20 requested and actual workers, both paths rounded to 0.3 seconds exact
validation and whole-command wall was 1.80 versus 1.75 seconds. With one
worker, width 16 took 2.5 versus 2.2 seconds exact validation and 3.94 versus
3.66 seconds whole-command wall. This 0.28-second whole-command cost is small
in absolute terms and does not outweigh the 12.1-second parallel and
551.8-second single-worker improvements on the primary 50-letter workload.

Control/prototype comparisons also retained complete output across the
synthetic `wxyz` dead-end test, `-S1` versus `-S20`, bound-off, and a shallow
projected bound.

Final validation used:

```bash
source ~/code/nutrimatic/.env/bin/activate
conan build .
source build/dep-info/conanbuild.sh
meson test -C build dfs-search query-index-cli --print-errorlogs
NUTRIMATIC_EXACT_MEMO_LOOKAHEAD=0 \
  meson test -C build dfs-search query-index-cli --print-errorlogs
```

Both the selected default and explicit control passed both smoke tests. A
final unset-environment query reported width 16 and retained the 32-letter
output hash above. Explicit width-0 and width-16 shallow-bound (`-C1`) runs
also compared byte-for-byte equal. Invalid text and oversized width 65 both
fell back to the selected width 16.

### Full 90-letter result — recorded evidence, not a routine benchmark

A completed full-S1 run is recorded in `results/idx.s1.all.m4`:

```bash
build/query-index "$IDX" "$S1" \
  -m4 -n0 -S20 --require-completable \
  > results/idx.s1.all
```

| quantity | result |
|---|---:|
| phase-1 entries | 18,085,610 |
| concrete classes checked | 13,900,743 |
| classes decided by bounds | 0 |
| top-level exact validations | 13,900,743 |
| exact memo slots / bytes | 33,554,432 / 268,435,456 |
| exact states stored | 25,165,824 |
| exact memo hits | 35,202,204 |
| width-16 full windows | 18,605,634 |
| width-16 known-true wins | 8,242,257 |
| width-16 re-probes decided | 41 |
| width-16 recursive expansions | 18,742,363 |
| exact-validation wall time | 2,812.0 s |

The output also has 18,085,610 lines, SHA-256
`8a080ac91f89a4209debdc2f0571b766980ccb8d8eb048a942252927dc01be4f`.
Because its entry count equals the phase-1 count, exact validation retained
every candidate class. This makes full S1 observed completion-heavy evidence:
the filter did no useful rejection, but still had to prove every result.

The memo stored exactly 75% of its slots, which is the implementation's
insertion ceiling. Once this ceiling is reached, previously unseen results
continue to be computed but are not inserted. The diagnostic did not count
these skipped stores, so this run proves saturation but does not quantify the
resulting repeated work. Saturation is nevertheless a priority issue: compared
with the width-16 50-letter result, the class count grew about 2.7x while
exact-validation time grew from 38.7 seconds to 2,812 seconds, about 73x.
A skipped-at-capacity counter and a controlled larger-table comparison are
needed before attributing that entire increase to saturation.

The `[00:47:20]` timestamp is the end of phase 2, not whole-command wall time;
sorting and writing the 340 MiB result follow it, and the saved log has no
shell `time` summary.

**Do not run the full 90-letter command automatically or as part of routine
testing, benchmarking, or agent validation.** It is recorded here so future
work can use the evidence without repeating the run. Run it again only when
the user explicitly requests a full-S1 measurement.

## Candidate optimization 3: in-progress exact memo ownership

The memo currently distinguishes:

```text
empty / false / true
```

and stores only final values. Concurrent workers that miss the same state can
both recursively compute it.

An ownership-capable entry would distinguish:

```text
UNSEEN / COMPUTING / FALSE / TRUE
```

The exact search graph is acyclic because every recursive edge removes at
least one letter. Waiting on a smaller remainder therefore cannot form a
dependency cycle.

### Avoid immediate waiting

An encounter with `COMPUTING` should not necessarily block:

1. mark that child as deferred;
2. continue scanning other candidates;
3. return if another child proves true;
4. revisit or wait only if all remaining possibilities are in progress.

This avoids synchronizing many workers behind one hard state and lets boolean
short-circuiting find an independent witness.

Possible waiting mechanisms:

- bounded spin followed by `yield`;
- a duplicate-computation fallback after a wait budget;
- a coarse condition variable associated with a table shard;
- platform-specific wait primitives only if measurement justifies them.

Do not add a mutex or condition variable per memo slot.

### Representation

Packing four states needs two status bits rather than the current one verdict
bit. Workloads whose exact key fits the reduced packed range can use:

```text
key * 4 + state
```

Larger keys need a separate status representation or the current duplicate
computation behavior. An adaptive dense memo, described later, naturally has
two status bits.

### Decision rule

Prototype ownership only if explicit losing expansions contain material direct
candidate-scan or inclusive subtree work, or if active-worker traces show many
workers redundantly following the same dependency wave. Strong 40-letter
scaling suggests this is not the first change to make.

Temporary recursive-expansion and weighted losing-expansion counters
separated:

- expansions that finish after another worker has published the same key;
- expanded states whose stores are skipped because the table is full;
- candidate and fitting-transition work directly performed by losing states;
- inclusive candidate, fitting, and descendant-expansion work beneath losing
  states, reported as an upper bound because nested losing subtrees can
  overlap; and
- maximum inclusive work for any one losing expansion.

The counters were removed after the result below ruled out an ownership
prototype.

While adding these counters, review found that a failed insertion
compare/exchange advanced to the next hash slot without checking whether the
winner had inserted the same key. Two workers could consequently publish
duplicate entries for one exact key. The old difference between expansion
attempts and successful insertions could therefore undercount duplicate
expansions. Insertion now rechecks the contested slot before advancing. The
measurement below used the explicit losing-race counter rather than inferring
races by subtracting aggregate totals.

### Corrected 50-letter result

A bound-off `${S1:0:50}` run with 20 search workers reported:

| quantity | count |
|---|---:|
| recursive expansions attempted | 7,397,285 |
| unique states inserted | 7,397,041 |
| expansions losing a memo race | 244 |
| expanded-state stores skipped at capacity | 0 |
| all exact candidates examined | 316,345,123,315 |
| direct candidates examined by losing expansions | 24,209,863 |
| descendant expansions beneath losing expansions | 0 |
| maximum candidates examined by one loser | 397,532 |

The explicit identity holds:

```text
7,397,285 attempts =
7,397,041 insertions + 244 losing races + 0 full-table stores
```

Only 0.00330% of expansion attempts lost a race. Those losers did contain
uneven direct scans, which confirms that weighting them was necessary, but
their 24.2 million candidate checks were only 0.00765% of the 316.3 billion
candidate checks in the run. No loser recursively expanded a descendant.

This workload provides no case for in-progress ownership. It would add status
and synchronization overhead to the whole memo in order to remove a tiny
amount of direct scanning and no observed recursive subtree work.

## Candidate optimization 4: skip support-mask groups

Every newly expanded exact state currently walks one contiguous rarest-letter
bucket. The support-mask check is cheap per candidate, but a cheap rejection
repeated millions or billions of times is still expensive.

Build an exact-search view that groups candidate IDs by:

```text
forced rarest rank
class support mask
possibly class length
```

At a state:

1. skip an entire support group when:

   ```text
   group_support & ~bag_mask != 0
   ```

2. within surviving groups, skip lengths larger than `letters_left`;
3. perform multiplicity checks only on the remaining candidates.

This turns many per-class absent-symbol failures into one per-group failure.

### Ordering

Strict class-index ordering is not semantically required for exact
completability. Nevertheless, the first prototype should retain a predictable
order within groups and record changes in:

- unique states;
- true/false state count;
- fitting transitions;
- validation wall time.

Grouping can alter which witness is found first and therefore legitimately
alter the explored state set.

### Memory

Avoid copying full class records. A `uint32_t` class-ID arena plus compact group
descriptors is sufficient. Measure the total number of distinct support masks
per bucket before selecting a representation.

## Candidate optimization 5: dynamic most-constrained-symbol selection

The current forced-symbol rule chooses the globally rarest remaining symbol.
That is a static approximation to the exact-cover heuristic:

> Choose the constraint having the fewest viable choices.

It is correct but not necessarily well calibrated for a particular remainder.
The globally rarest symbol can have a much larger fitting candidate set than
another remaining symbol after support and multiplicity restrictions.

For existence-only search, any remaining symbol may be chosen. Every valid
completion must contain a class covering that symbol. The search can therefore:

1. estimate the viable candidate count for each remaining symbol;
2. choose the smallest;
3. enumerate all classes containing the chosen symbol.

The existing rarest-letter buckets cannot support this directly. A class is
stored only under its own globally rarest symbol, not under every symbol it
contains. Dynamic selection needs inverted postings:

```text
symbol -> class IDs containing symbol
```

Each class appears once for every distinct symbol in its support.

### Prototype stages

1. Build postings and choose the symbol with the smallest raw posting list.
2. Improve the estimate with remaining-length and support-mask group counts.
3. If worthwhile, sample multiplicity fits for the few smallest postings.

The first version should not scan every posting merely to choose which posting
to scan; selection overhead must stay well below saved candidate tests.

### Expected value

Dynamic MRV attacks the exponential cost of proving dead states and is more
promising algorithmically than changing the order of top-level remainder
queries. It may also remove bucket-related waves by avoiding locally bad
static choices.

### Applicability

This is immediately safe for exact boolean reachability. Applying it to
ordinary ranked `dfs-anagrams` would change traversal order, heap-floor timing,
and spelling expansion, and requires a separate benchmark and correctness
analysis.

## Candidate optimization 6: reachability-only complementary projections

The existing projected table was designed as an admissible completion-score
bound. It stores floats and uses one abstraction:

```text
some rare letters tracked exactly
all remaining letters merged into one wildcard total
```

For exact completability, score values are unnecessary. Only a one-sided
reachability certificate is needed:

```text
abstract state unreachable => concrete state unreachable
```

### Grouped-letter pattern databases

Partition input symbols into groups and track only the remaining total in each
group. A class consumes a vector of group counts.

Every concrete completion maps to a completion of the grouped state.
Therefore, if the grouped state cannot be tiled, the exact state cannot be
tiled. A positive grouped result remains inconclusive.

Examples:

```text
projection A:
  several rare symbols as singleton groups
  all other symbols in one group

projection B:
  a different set of singleton symbols
  two or three groups for the remainder

projection C:
  another independent partition selected to distinguish actions
  that collide in A and B
```

Query all projections and reject the exact state if any one reports
unreachable.

### Why complementary projections may beat one rich projection

One large wildcard group loses every identity relation among its members.
Adding more rare singleton dimensions can be expensive while still failing to
capture a constraint involving common letters.

Several small, differently partitioned abstractions can capture complementary
constraints without paying the Cartesian product of one combined table. This
is analogous to multiple pattern databases.

### Reachability-specific implementation

- store bits or compact status values, not floats;
- quotient classes by identical projected consumption vector;
- use bottom-up construction where practical;
- do not retain scores, rounding margins, or best-member data;
- select abstractions by dead-state rejection per setup byte and second;
- cap total preprocessing at a fraction of expected exact-validation time.

### Partition selection

Candidate policies include:

- rarest singleton prefix plus one wildcard group;
- greedy splitting of groups whose projected action collisions are highest;
- complementary exact-letter masks;
- several deterministic hash partitions of symbols;
- offline selection using representative 40- and 50-letter workloads.

Random or hashed partitions are safe when used only as relaxations; they affect
bound strength, not correctness.

### Evidence and risk

The rich current projection did not help the 40-letter workload, so simply
reimplementing the same rarest-prefix abstraction with bits is unlikely to be
enough. The value proposition is specifically:

- much cheaper construction; and
- multiple complementary identity groupings.

## Candidate optimization 7: adaptive dense two-bit exact memo

The sparse flat memo is appropriate when the theoretical mixed-radix state
space is large and only a small fraction is visited. For some medium inputs,
however, a dense compact status table is competitive in bytes.

The 40-letter preflight reported:

```text
141,557,760 theoretical states
```

At two bits per state:

```text
141,557,760 * 2 / 8 = 35,389,440 bytes
```

The current sparse memo used:

```text
4,194,304 slots * 8 = 33,554,432 bytes
```

A dense two-bit table is only about 1.8 MiB larger and would provide:

- collision-free lookup by exact mixed-radix key;
- no hash computation;
- no linear probing;
- natural `UNSEEN / COMPUTING / FALSE / TRUE` states;
- no need to size capacity from class count.

### Risks

- two-bit atomic updates require word-level compare/exchange;
- unrelated nearby states can share an atomic word and create false sharing;
- the dense address range is much larger than the actually visited 1.30
  million states;
- random access may lose more cache locality than hash elimination saves;
- larger inputs quickly make the dense representation impossible.

### Selection rule

Consider dense status only when:

```text
ceil(theoretical_states * 2 / 8) <= configured exact memo budget
```

and compare against the sparse table in alternating runs. Sparse remains the
general fallback.

An alternative three-bitset design—seen, value, and computing—uses more memory
but may simplify atomic operations. It should be considered only if the packed
two-bit update is measurably problematic.

## Candidate optimization 8: split only the recursive final tail

The top-level cursor uses the smallest simple scheduling unit: one class. It
cannot redistribute work once a worker has claimed a class and entered a deep
exact recursion.

If measurements show a material interval between the last top-level claim and
the final worker completion, allow an expensive validation to expose a small
number of child OR-tasks:

```text
root remainder
  child candidate 1
  child candidate 2
  ...
```

Any true child cancels the remaining siblings. False requires all siblings to
finish.

Possible policies:

- split only while idle workers exist;
- split only at depth one or two;
- split only after a validation exceeds a node/candidate threshold;
- cap outstanding tasks per top-level remainder;
- use worker-local deques with stealing.

This is lower priority because:

- the 40-letter run already scaled about 17.2x on 20 workers;
- it does not improve single-thread waves;
- cancellation, memo ownership, and task lifetime interact;
- fine-grained exact tasks may add more synchronization than work.

Keep it only if it reduces measured final-tail wall time without materially
increasing CPU time.

## Additional lower-level opportunities

These should follow an exact-search profile after the algorithmic prototypes.

### Skip unused projected-key maintenance

When no score/reachability bound is active, exact subtraction still updates
`worker->score_key`. Avoid that work in a bound-off exact-validation mode.
This is only one integer update per transition, so expected value is modest.

### Use a cheaper exact hash

Every memo lookup currently applies a two-multiply SplitMix-style finalizer.
A high-bit multiplicative hash may be sufficient for mixed-radix keys:

```text
slot = high_bits(key * odd_constant)
```

Do not change it without recording probe-length distributions; structured keys
can expose poor hashes that random-key microbenchmarks miss.

### Separate exact-hot metadata

Exact validation touches:

- length;
- signature;
- support mask;
- repeated multiplicities;
- packed letter offset/count;
- optionally projected-key delta.

Keep these in compact contiguous arrays and avoid touching member pointers,
scores, and other phase-3 metadata. Much of this split already exists in
`FitClass`, but exact signature and length accesses still span the phase-1
record and hot record.

### Prefetch candidate metadata

Once candidate-scan counters prove scanning dominant, try prefetching the next
few `FitClass` records and packed repeated requirements. This is a
constant-factor experiment only.

### Tune sparse memo capacity

The table capacity is the next power of two at least twice the class count,
while actual state count differs by workload. The resulting load factor can be
far below the allowed 75%, or it can reach the insertion ceiling.

The recorded 90-letter run reached that ceiling exactly: 25,165,824 stored
states in 33,554,432 slots. Subsequent unseen results were not cacheable.
Normal diagnostics should count stores skipped at capacity so saturation is
visible directly instead of inferred from the final state count.

Smaller tables reduce allocation, initialization, and memory traffic but
increase probes and risk reaching the insertion limit. Larger or growable
tables may avoid repeated work on long inputs but add allocation,
initialization, and memory traffic. Capacity should be based on measured
state-to-class ratios or a bounded growth policy, not changed blindly. The
90-letter saturation result makes this measurement a higher priority than
the lower-level hash, prefetch, and layout experiments.

### Persistent exact reachability

Repeated queries with identical:

- input bag;
- minimum length;
- phrase policy;
- dictionary;
- class corpus;

could reuse an exact reachability artifact. This does not help a one-shot
novel bag and has significant cache-key/versioning requirements, so it is a
separate product-level optimization.

## Alternative batch formulations

The current memoized top-down recurrence already turns millions of top-level
questions into one shared state graph:

```text
class c is completable iff reachable(full_bag - c)
```

More radical formulations are possible but lower priority.

### Forward reachability from the empty bag

Enumerate exactly tileable subbags by adding classes from the empty bag, then
test every top-level remainder for membership.

Potential advantage:

- generate positive states only, avoiding exhaustive proof of many dead
  states.

Main problem:

- naively trying millions of classes from every positive state is
  prohibitive;
- reverse canonicalization is less natural than forced-letter subtraction;
- the reachable positive set may itself be very large.

This becomes interesting only with a strong addition index or a compact class
decision diagram.

### Meet in the middle

Enumerate tileable bags up to approximately half the input length and look for
complementary pairs for each queried remainder.

Memoized DFS complexity is already based on distinct subbags rather than path
permutations, so halving recursion depth does not automatically halve the
state count. The branching and join indexes may dominate.

### Solution-DAG marking

Enumerate the exact reachable state DAG and mark each root class whose
remainder reaches zero. This is mathematically the same information as the
current memo and becomes different only if the DAG can be constructed more
efficiently bottom-up or in batches.

These approaches should not displace the simpler child-probe, indexing, and
projection experiments without evidence that top-down dead-state discovery is
the dominant cost.

## Recommended experiment order

### Phase 1: make the work visible

1. Add exact candidate, memo, duplicate-expansion, skipped-at-capacity,
   bucket, and tail counters.
2. Improve progress diagnostics to include completed classes or candidate
   tests.
3. Benchmark `-S1`, `-S4`, and `-S20` on 40 and 50 letters.

### Phase 2: remove avoidable work without changing traversal

1. Probe child exact memo keys before bag mutation.
2. Probe conclusive child reachability bounds before mutation.
3. Skip projected-key maintenance when bounds are off.
4. Re-profile exact validation.

### Phase 3: fix completability preprocessing policy

1. Compare automatic 64 MiB projection with shallow projections across
   32/40/50-letter inputs.
2. Replace the fixed 50-letter cliff with a reachability-specific policy.
3. Count descendant bound decisions, not only top-level decisions.

### Phase 4: exploit shared memo maturity

1. Measure later-known-true opportunities.
2. Prototype memo-aware candidate traversal if frequent.
3. Measure duplicate first-miss work.
4. Prototype in-progress ownership only if duplication is material.

### Phase 5: reduce first-time candidate scans

1. Measure support-mask group cardinalities.
2. Prototype support-mask grouping inside existing rarest buckets.
3. Build per-symbol postings and test dynamic MRV if bucket scans still
   dominate.

### Phase 6: stronger reachability certificates

1. Implement one tiny boolean grouped-letter projection.
2. Add complementary partitions.
3. Select projections by total setup plus exact-validation time.

### Phase 7: representation and tail experiments

1. Measure sparse memo saturation and compare a bounded larger/growable table
   on manageable inputs that reproduce the insertion ceiling.
2. Test a dense two-bit memo where its exact byte criterion fits.
3. Split recursive work only if measured tail time remains important.
4. Consider hash, prefetch, and layout changes after another profile.

## Benchmark protocol

Follow the repository instructions:

```bash
source ./setup.sh
export IDX=~/code/nutrimatic/idx/wiki-merged.5.index
```

Before timing:

```bash
pgrep -x query-index
pgrep -x dfs-anagrams
```

Use:

```bash
/usr/bin/time -v \
  ./build/query-index "$IDX" "${S1:0:50}" \
  -n0 -m4 --require-completable -S20 \
  > /dev/null
```

Alternate control and prototype runs. Record:

- phase 1;
- phase-2 setup;
- exact validation;
- whole-command wall time;
- user and system CPU;
- peak RSS;
- exact states and hits;
- all experimental counters;
- active-worker tail;
- output hash or complete-output comparison at a manageable size.

Useful workload matrix:

| workload | purpose |
|---|---|
| `${S1:0:32}` | fast cache-policy and correctness iteration |
| `${S1:0:40}` | measured medium case; dense two-bit candidate |
| `${S1:0:50}` | primary exact-search and wave workload |
| dead-end-heavy bag | stress exhaustive false states |
| completion-heavy bag | stress positive short-circuit behavior |
| `-S1`, `-S4`, `-S20` | separate intrinsic work from parallel effects |
| shallow and rich projection | isolate bound setup and descendant pruning |

Full 90-letter S1 is intentionally absent from the runnable matrix. Its
completed result is recorded above; agents must not execute it automatically.
Only repeat it in response to an explicit user request.

## Correctness

All proposed exact-search optimizations must preserve:

- one result bit per original class index;
- exact true/false completability;
- phrase and dictionary policy;
- repeat use of a class where the existing recurrence permits it;
- output ranking performed after the bitmap is complete;
- independence from `--word-bonus` and score ordering.

Candidate order and forced-symbol selection are not observable for boolean
reachability, provided:

- every completion contains at least one enumerated candidate;
- every enumerated candidate fits exactly;
- recursion strictly subtracts the selected class;
- memo keys represent the complete exact remainder bag;
- no projected positive result is mistaken for exact proof.

Keep tests minimal:

1. existing `dfs-search`, `dfs-cli`, and `query-index-cli` smoke tests;
2. the synthetic dead-end completability case;
3. dense versus projected output agreement where dense fits;
4. output hashes across `-S1` and `-S20`;
5. debug assertions for any new posting/group index.

Run `/review` before committing any implementation.

## Overall assessment

The failed descending-length experiment is useful negative evidence. It shows
that small-remainder-first top-level scheduling does not materially change the
shared exact state graph or improve its reuse.

The strongest immediate directions are instead:

1. avoid bag mutation for the tens of millions of already memoized children;
2. expose and reduce candidate bucket scans;
3. replace the score-oriented projection policy for completability;
4. use shared memo maturity when choosing which child to explore;
5. move toward dynamic most-constrained-symbol selection if static buckets are
   the dominant first-miss cost;
6. use several cheap boolean reachability projections rather than one costly
   score-rich abstraction.

The visual waves should be treated as an observability symptom. The underlying
performance targets are work per newly expanded exact state, duplicate
concurrent work, strength and cost of reachability certificates, and the
measured final-worker tail.
