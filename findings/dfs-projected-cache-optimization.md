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

The measurements below establish that this optimization is real, but they are
not the main subject of the remaining work. The research question is now how
to avoid constructing so many action/state edges and abstract states in the
first place, rather than which adjacent projection depth wins a timing sweep.

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

### Timing reproducibility

The transition counts are deterministic, but the wall-clock measurements are
not a permanent baseline. A newer local `d=15`, 40-letter run recorded
47.757s setup and 36.558s search with the same 5,919,322,956 successful
transitions, 342,949,072 final DFS nodes, and 24,804 solutions as the 29.736s
setup run above. The algorithmic comparisons should therefore use transition
and node counts alongside controlled paired timing runs.

A later local timing set recorded:

| letters | `-C` | `d` | actions | states built | setup | search | phase 2 | transitions | final DFS nodes |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 38 | 32 MiB | 15 | 124,513 | 2,984,149 | 32.796s | 9.979s | 42.775s | 4,159,322,534 | 105,067,790 |
| 40 | 32 MiB | 15 | 151,440 | 3,411,183 | 47.757s | 36.558s | 84.315s | 5,919,322,956 | 342,949,072 |
| 40 | 128 MiB | 16 | 222,643 | 8,787,405 | 169.550s | 19.446s | 188.996s | 18,670,931,559 | 183,381,681 |

All three runs report 20 preprocessing workers. The two 40-letter runs form
the most useful comparison within this set: `d=16` halves neither setup nor
search. It reduces final DFS nodes by 46.5% and search time by 46.8%, but
increases construction transitions by 3.15x and setup time by 3.55x. Total
phase 2 is consequently 2.24x slower than `d=15`, despite using the larger
cache and producing the tighter bound.

Later investigation established that other `dfs-anagrams` instances sometimes
run concurrently on this host. The timings above and the initial support-group
timings below were not preceded by a host-process check, so they are
observations rather than controlled speedup evidence. Future timing runs must
check the host process table immediately before the run and during long runs.
Deterministic state, transition, node, and solution counts are unaffected.

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

## What remains in the current recurrence

The action reduction implemented here recognizes only single-action
equivalence: actions with the same forced bucket and projected delta reach the
same child, so only their maximum score survives.

After that reduction, every fitting action is still evaluated. A successful
evaluation mutates the projected bag, loads or recursively constructs the
child, restores the bag, and contributes to the maximum. On the 40-letter
`d=15` workload this is still 5.919 billion successful transitions over
3.411 million constructed states, or roughly 1,735 successful transitions per
constructed state.

Projected construction also remains eager. It starts from every fitting root
action and constructs the resulting abstract dependency closure before the
concrete DFS has produced a score floor. Consequently, the current
implementation has not exhausted either of the two main ways to reduce setup:

1. prove that fewer action-to-child transitions need evaluation; or
2. construct bounds for fewer abstract states.

## Support-subset traversal experiment

The first diagnostics and fit-index prototype are opt-in:

```sh
NUTRIMATIC_PROJECTED_DIAGNOSTICS=1
NUTRIMATIC_PROJECTED_SUPPORT_GROUPS=1
```

Diagnostics partition action scans into wildcard-length, exact-support, and
multiplicity rejections, and partition fitting edges into successful and dead
children. They also count ready child loads, first-owner state claims,
ownership conflicts, dependency-spin iterations, and finite/dead constructed
states. Counters are worker-local during construction and merged afterward.
The ordinary projected loop remains unchanged when diagnostics are disabled.

The support-group prototype compresses the selected exact-letter support into
a `d`-bit key and groups action IDs by their exact support. At a state with
support `M`, it enumerates only submasks of `M` containing the forced rarest
bit. Those are exactly the action-support groups that can pass the existing
support test and obey forced-letter canonicalization. When no exact letter
remains it visits only the wildcard-only group.

The group index is temporary preprocessing metadata. On the 28-letter,
`d=14` case it contains 16,385 offsets and 73,522 action IDs: 425,168 bytes
(415.2 KiB) with the current 8-byte offsets. It is released with the other
projected action metadata before the concrete DFS.

### 28-letter measurement

These are paired runs on the same `S6` prefix with `-m 4 -n 1000 -C 8 -F`.
Every variant constructed 430,123 states, performed 213,302,595 successful
transitions and 994,904 `nextafter` calls, visited 3,086,650 final DFS nodes,
and produced byte-identical top-1000 output.

| traversal | threads | diagnostics | setup | search | action scans |
|---|---:|---|---:|---:|---:|
| bucket scan | 20 | off | 1.994s | 0.237s | not counted |
| support subsets | 20 | off | 1.397s | 0.208s | not counted |
| bucket scan | 20 | on | 2.365s | 0.211s | 4,787,820,579 |
| support subsets | 20 | on | 1.481s | 0.213s | 473,063,458 |
| bucket scan | 1 | off | 20.367s | 0.211s | not counted |
| support subsets | 1 | off | 10.538s | 0.209s | not counted |

Support grouping removes 90.1% of actual per-action fit checks. Setup improves
29.9% in the clean 20-thread pair and 48.3% in the clean serial pair. The
larger serial improvement is consistent with parallel construction hiding
some scan cost.

The ungrouped diagnostic partition was:

| outcome | count | share of scans |
|---|---:|---:|
| wildcard length rejected | 1,086,369,610 | 22.7% |
| exact support rejected | 3,332,842,204 | 69.6% |
| exact multiplicity rejected | 132,125,145 | 2.8% |
| fitting edge | 236,483,620 | 4.9% |

Of the fitting edges, 23,181,025 (9.8%) reached a dead child. Only 11,615 of
430,123 constructed states (2.7%) were dead. Reverse-only construction is
therefore unlikely to remove much state work on this case by itself, although
dead-child certificates could still avoid a meaningful fraction of fitting
edges.

The ungrouped run recorded 25,527 ownership conflicts among 236.5 million
child lookups, so conflicts are rare. The same conflicts accumulated
124,055,861 pause-loop iterations, which means a blocked lookup can wait a
long time; elapsed wait cycles are still needed before justifying a
continuation scheduler. Support grouping changed traversal order and recorded
98,301 conflicts but fewer pause iterations (101,767,621), while still
finishing sooner. Conflict frequency alone is not a useful optimization
objective.

The 38-letter, `d=15`, 20-thread validation also produced byte-identical
top-1000 output and identical state, transition, final-node, and solution
counts. Support grouping reduced setup from 30.367s to 23.852s (21.5%). The
two runs' search times were 7.895s and 19.063s despite identical node counts,
another example of the host's timing variability. The `nextafter` count
differed by two out of about 7.26 million because the group traversal changes
action order.

The 40-letter, `d=16`, 128 MiB validation also retained exactly 8,787,405
states, 18,670,931,559 successful transitions, 183,381,681 final DFS nodes,
and byte-identical top-1000 output. Ungated support-group runs recorded
131.075s and 112.012s setup versus an earlier ungated 169.550s reference; these
are not a controlled comparison. A later run checked before launch and during
construction found no competing `dfs-anagrams` and recorded 93.128s, but lacks
a similarly gated ungrouped pair. The support index contained 65,536 groups
and 222,643 action IDs and took 8.5--9.7ms to build.

The evidence promotes support-subset traversal from a speculative fit index to
a strong candidate for the projected builder. Index-build time is now reported
separately from recurrence construction.

### Wildcard-length ordering experiment

Sorting every support group by wildcard consumption allows its scan to stop at
the first action longer than the state's wildcard remainder. On the 28-letter
diagnostic case this behaved exactly as intended at the scan level: it removed
104,454,693 of 473,063,458 checks (22.1%), leaving 368,608,765. State,
transition, final-node, solution, and output counts were unchanged. Building
and sorting the 16,384-group index took 2.5--3.1ms.

The optimization was nevertheless a regression in the clean serial paired
runs:

| traversal within each support group | setup run 1 | setup run 2 |
|---|---:|---:|
| original projected-action order | 10.324s | 10.289s |
| wildcard length, then action ID | 10.797s | 10.854s |

The mean setup penalty was 5.0%. The successful transitions dominate this
workload, and their traversal order affects recursive construction and cache
locality. Removing cheap rejected checks did not compensate for the worse
successful-edge order. The wildcard ordering is therefore not retained.
Future fit indexes should preserve the existing successful-action order or
demonstrate a more valuable ordering objective; scan-count reduction alone is
not sufficient evidence. These paired runs also predated the host-process
gate, so the timing magnitude is tentative even though both pairs consistently
favored the original order.

## Final-query demand experiment

Final-query diagnostics are independently enabled with:

```sh
NUTRIMATIC_PROJECTED_QUERY_DIAGNOSTICS=1
```

They use a temporary one-bit-per-table-slot set and count concrete-DFS bound
lookups, distinct projected keys, lookups that prune, and DFS nodes reached
before the top-N sink has a score floor. Full projected-builder diagnostics
also enable these query counters; the separate switch avoids instrumenting
billions of construction edges when only final demand is needed.

| workload | states built | unique queried | built queried | bound lookups | rich prunes | length-only prunes | modular preserves | nodes before floor |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 28 letters, `d=14` | 430,123 | 324,005 | 75.3% | 3,086,592 | 3,080,422 (99.80%) | 1,592,898 (51.7%) | 2,834,857 (92.0%) | 57 |
| 40 letters, `d=16` | 8,787,405 | 5,202,667 | 59.2% | 183,381,299 | 183,299,923 (99.96%) | 74,271,639 (40.5%) | 144,571,967 (78.9%) | 381 |

Eager construction built 106,118 states never queried in the smaller case and
3,584,738 never queried in the larger case. This establishes meaningful
state-selection headroom, especially at `d=16`, but it is not a direct estimate
of lazy construction work: a queried state's exact rich value may require
unqueried descendants. The score floor also arrives almost immediately and
the DFS then issues bound queries at nearly every node, so a background
refiner would face demand quickly rather than enjoying a long warm-up window.

The very high prune rate makes admissible fallback quality critical. A
length-only recurrence was evaluated in shadow mode on every rich lookup. It
missed 1,487,524 rich prunes in the smaller case and 109,028,284 in the larger
case. It produced zero coarse-only prunes, as required by its relaxation, but
preserved only 51.7% and 40.5% of rich pruning respectively.

This rejects length-only fallback as the sole basis for online refinement.
Saving projected states would expose tens of millions of concrete nodes until
refinement caught up. Before implementing a lazy/background builder, test a
stronger still-cheap fallback—such as a second small projection or several
weighted scalar projections—against this same disagreement counter. Bound-gap
histograms would help distinguish near misses from cases needing much more
abstraction detail.

The first stronger shadow fallback uses four independent tables keyed by
remaining length and a deterministic 6-bit modular multiset signature. Each
table contains only 64 states per length layer. It deduplicates concrete
classes by `(length, signature delta)`, relaxes all count and fit constraints,
and computes a complete max-plus recurrence. Each table is independently
admissible, so their minimum is admissible and can also complement the rich
projection.

This fallback preserved 92.0% of rich prunes at `d=14` and 78.9% at `d=16`,
far better than length-only but still missing 245,565 and 38,727,956 rich
prunes respectively. It also proved 552 and 4,844 prunes that the rich
projection missed, confirming complementary value. The `d=16` modular run was
host-process-gated at launch and during construction and produced
byte-identical output.

Four 6-bit signatures are therefore promising as an additional complementary
bound, but not yet strong enough as the only fallback for absent rich states.
The next cheap sweep should vary signature width, hash selection, and table
count while reporting rich-prune coverage per table byte and per final-query
operation. If coverage saturates well below 100%, a small exact-letter
projection is a better fallback candidate.

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

## Research inventory

The following are unmeasured hypotheses unless explicitly described as
implemented. "Exact" below means exact with respect to the selected projected
recurrence, not exact with respect to the concrete bag.

| lever | main idea | guarantee | first useful discriminator |
|---|---|---|---|
| remove edges | certify losing actions with a cheaper upper bound | exact, if every skipped edge has a certificate | fraction of fitting edges rejected in shadow mode |
| remove actions | single- and multi-action dominance | exact | actions removed, weighted by how often their bucket is scanned |
| remove states | refine only states requested by the concrete DFS | admissible fallback; exact entries optional | projected-key query frequency and bound gap |
| avoid dead work | build backward from the empty bag | exact over generated finite states | fitting edges to `-infinity` children and forward/reverse state overlap |
| regularize the DP | layered max-plus shift-and-reduce | exact | reachable density by total-letter layer |
| improve the abstraction | complementary, cost-partitioned, or adaptively split projections | admissible | pruning gain per constructed transition |
| share state sets | decision diagrams or repeated-subtable compression | exact or conservatively quantized | repeated-subtable and value-delta entropy |
| cheapen an edge | packed state arithmetic, fixed-point scores, and fit indexes | exact or conservatively rounded | cycles per successful and rejected transition |
| amortize setup | background construction and persistent reuse | exact ready entries plus admissible fallback | repeated-query rate and overlap with final DFS |

The categories matter because they require different evidence. A faster fit
test will not help much if almost all time is in successful edges, while a
reverse builder is attractive only if dead descendants are common enough to
offset the finite states it generates that the root traversal would never
visit.

## Reduce action-to-child edges

### Hierarchical candidate certificates

Construct a very cheap coarser projection before constructing the selected
rich projection. The simplest is the `d=0` length-only table: it has one state
per remaining length and at most one deduplicated action per consumed length.

If `U` is an upward-conservative coarse bound, then for every rich projected
action `a`:

```text
candidate(a, state)
    <= action_score(a) + restart + U(project(child(a, state)))
```

Evaluate one promising action fully to obtain an exact feasible incumbent
`best`. Any other action whose upper envelope is no greater than `best` can be
discarded without loading or constructing its rich child. The fully evaluated
candidates themselves provide the lower incumbent; a separate lower-bound
table is optional. Group actions by consumed length and order each group by
descending score. For a length-only `U`, the child term is constant within the
group, so one failed threshold rejects the rest of that score-ordered group.

A cascade of progressively stronger projections could certify more losers.
There are two safe stopping modes:

- continue until every omitted action is certified, preserving the exact rich
  recurrence; or
- stop early and store the coarse envelope for the unresolved portion,
  producing a weaker but still admissible bound.

Never store the maximum of only the expanded candidates as an upper bound. It
is a lower bound on the projected maximum and can prune valid results.

This is the highest-priority construction experiment because it can reduce the
measured successful-transition volume. The first implementation should be
shadow-only: retain the current result but count edges and whole score-ordered
suffixes that the coarse certificate would have skipped.

### Composite action dominance

The current deduplication compares only actions with identical single-action
deltas. An action `a` is also globally dominated if a sequence of other actions
has the same total projected delta and at least its adjusted score:

```text
delta(a) = sum(delta(b_i))

score(a) + restart
    <= sum(score(b_i) + restart)
```

Consider any complete projected solution that uses `a`. Replacing `a` with the
other actions preserves the solution's total consumption and does not decrease
its score. The whole replacement solution can then be put into canonical
forced-letter order, because some remaining action always consumes the rarest
remaining exact letter. The replacement actions alone need not be executable
consecutively at the point where `a` appeared; they may interleave with the
original suffix. This whole-solution argument is what makes the removal exact.

The restart charge on every replacement action may make domination uncommon.
Run a bounded offline decomposition DP over action-sized deltas first and
report both raw removals and expected scan-weighted removals. The latter is
more informative because deleting an action in a rarely reached bucket has
little construction value.

### Conditional and local dominance

Global equal-delta dominance is deliberately strict. A coarse table can prove
additional dominance for one state even when no global replacement exists:

```text
upper(a, state) <= exact_candidate(b, state)
```

This is the same certificate mechanism applied pairwise. Cache the winning
action ID temporarily with each constructed state, try winners from neighboring
mixed-radix states first, and then certify the other actions. If winner regions
are stable, this becomes a cheap policy-plus-certificate computation rather
than a full maximum over every edge.

A raw Pareto rule such as "consume fewer letters with a higher score" is not
safe. Consuming fewer letters leaves more negative-cost work for the child.
Any such dominance test needs either an equal-delta replacement or an explicit
bound on the residual consumption.

## Construct fewer states

### Search-driven bounded refinement

Lazy construction can go further than delaying the current recursive closure:

1. Start concrete DFS with a nearly free length-only or other complete coarse
   bound.
2. Once the top-N sink has a score floor, record projected states for which the
   coarse bound failed to prune.
3. Refine states according to query count, bound gap, and estimated subtree
   leverage.
4. Limit refinement work and retain a coarse upper envelope for every
   unresolved action and state.

Every lookup therefore has an admissible answer. Ready rich entries improve
the bound; missing or unfinished entries fall back to the coarse table. This
does not recreate the coverage holes of the old partial exact cache.

The refinement can also run on otherwise idle preprocessing threads while the
single-threaded concrete DFS proceeds. The concrete search must never spin on
a `COMPUTING` rich entry: it uses the fallback, optionally enqueues the key,
and continues. Completed entries are already published child-first, so they
can be consumed safely. This changes wall-clock structure as well as work
selection: useful construction overlaps search, and irrelevant construction
can be cancelled when the DFS completes.

If a parent is computed using fallback child values, it remains admissible but
must be marked inexact and revisited after its children improve. Keeping exact
and fallback-derived entry states distinct avoids silently treating a stale
upper envelope as the exact rich recurrence.

### Backward construction from completable states

The recurrence is a shortest-path problem after negating scores, with the empty
bag as its goal. The current top-down builder starts at the projected root and
visits forward-reachable states, including states whose eventual value is
`-infinity`. Reverse construction starts with the empty bag and adds actions,
so it generates only states with a finite completion.

The forced-letter canonicalization has a simple reverse condition. If an
action's bucket is `b`, it may be added to a child only when the child contains
no exact letter rarer than `b`. Wildcard-only actions may be added only while
the child has no exact letters. Processing increasing total-letter layers then
finalizes every child before its parents.

This direction is not automatically better. It can generate finite states
within the root box that are not reachable by subtracting actions from the
root. The comparison is:

```text
current builder: forward-reachable, including dead states
reverse builder: goal-reachable, including root-irrelevant states
ideal work set:  intersection of the two
```

The existing counter omits fitting transitions whose child is `-infinity`, so
it cannot estimate this tradeoff. Count those edges and finite/dead states
before implementing a full reverse builder. A small shadow reverse reachability
bitset by total-letter layer can then estimate the intersection.

### Bidirectional relevance and a perimeter

If neither direction is sparse enough, keep compact reachability marks from
both sides and evaluate values only in their intersection. Another variant
builds a small reverse perimeter of completable states and lets top-down
construction stop when it reaches that perimeter.

This resembles a partial pattern database, but its fallback contract is
important here: a state outside the selected intersection still needs the
coarse admissible bound. The perimeter is a work-selection device, not
permission to return "no bound."

## Change the dynamic-programming organization

### Max-plus shift-and-reduce

For a fixed projected action, `child_key = parent_key - delta` is a shifted
dense-array access over a rectangular mixed-radix region. The recurrence can
therefore be viewed as a max-plus shift-and-reduce:

```text
H[parent] = max over eligible a of
    adjusted_score[a] + H[parent - delta[a]]
```

Rather than recursively scanning actions for one state, process total-letter
layers and apply an action to a run or tile of eligible parent states. The
mixed-radix digit limits describe those runs without per-state requirement
loops. Tile ownership avoids atomic maximum updates, and the shifted child
loads become predictable enough for vectorization and prefetching.

This does not reduce the mathematical edge count. It is attractive if the
reachable/finite density within a layer is high, because it replaces recursive
bag mutation, cache-miss calls, and atomic first-owner synchronization with
streaming work. It is unattractive for sparse layers. Report per-layer density
and valid-edge density before choosing between parent-major, action-major, and
tiled traversal.

The small maximum number of words suggests another formulation,
`F[k][delta] = best score using k actions`, followed by a maximum over `k`.
That is also a sequence of max-plus convolutions. It is worth keeping in the
inventory for SIMD or accelerator experiments, but it does not obviously
remove more work than the layered recurrence on a CPU.

### Nonblocking task-DAG scheduling

Within the existing recursive traversal, a worker that encounters a child
owned by another worker spins. If spin time is material, replace waiting with a
continuation: register the parent as dependent on the child, work on another
ready state, and resume the parent when the child publishes.

This retains sparse top-down construction and exact values, unlike a dense
layered pass. It is substantially more complex than the current recursion, so
instrument ownership failures and spin cycles first. If those are small, edge
reduction or packed arithmetic has more leverage.

## Improve the abstraction per unit of construction

### Several small complementary projections

Instead of paying the Cartesian-product cost of one large table, build two or
more small projections over different letter sets:

```text
bound(bag) = min(P1(project1(bag)),
                 P2(project2(bag)),
                 ...)
```

Each `Pi` is independently an upper bound, so their minimum remains admissible.
Complementary projections can retain different constraints while their total
state and action counts remain below those of one large projection. During
concrete DFS, check the empirically strongest table first and stop as soon as
one table proves a prune.

The selection objective should be estimated nodes avoided per construction
transition, not average bound value or table bytes alone. A second projection
should be chosen specifically on sampled states where the existing projection
is weak, rather than by another independent rare-letter rule.

### Additive cost-partitioned projections

Complementary projections can potentially combine more strongly than a
minimum. Every future action has negative adjusted log score:

```text
weight(a) = class_score(a) + restart < 0
cost(a) = -weight(a) > 0
```

Split each action cost among projections:

```text
cost(a) = sum(cost_i(a)), with cost_i(a) >= 0
```

Let each relaxed projection compute the minimum completion cost under its
assigned `cost_i`. For any concrete completion, the sum of the projected
minimum costs is no greater than that completion's exact cost. Negating the
sum therefore gives an admissible score upper bound:

```text
score_bound(bag) = -sum(projected_min_cost_i(project_i(bag)))
```

Positive cost assigned to a projection where the action delta is zero is
unexploitable: the abstract optimizer can omit that self-loop. It is still
admissible, but it wastes cost that another projection could use, so a useful
partition should assign that component zero. Uniform splitting is therefore
only a correctness baseline, not necessarily a sensible allocation. Saturated
or post-hoc cost partitioning may preserve more useful cost in each
abstraction, but its own construction overhead must be included.

### Adaptive group splitting

The current automatic shape is a corpus-rarest exact prefix and one wildcard
group. More general shapes include arbitrary exact masks, several wildcard
groups, and exact dimensions for common but high-impact letters.

Treat this as counterexample-guided abstraction refinement:

1. build a very small projection;
2. sample concrete states where its bound narrowly failed to prune;
3. identify the wildcard letter or group split that most separates the
   high-scoring aliased actions on those states;
4. refine only while predicted pruning value exceeds construction cost.

Useful split scores include reduction in projected-action collisions, reduction
in the candidate upper-envelope gap, and separation of the current winning
action from its competitors. This targets the actual query distribution and
avoids using projection depth as the research variable.

### Weighted and modular scalar projections

A broader cheap family maps the bag to a nonnegative weighted sum:

```text
key_w(bag) = sum(weight_w[letter] * count[letter])
```

Every concrete transition maps to subtraction of its weighted delta. Allowing
all abstract sequences that fit the scalar key is a relaxation, so a complete
one-dimensional table is an upper bound. Moderate integer weights distinguish
some letter mixtures while preserving a small contiguous table and strong
action deduplication.

Several weight vectors can be combined by minimum or cost partitioning. A
related quotient tracks total letters plus a modular multiset signature; total
letters supplies the acyclic layer while signature collisions weaken the
bound safely. Random weights are useful only as a baseline. Better weights
should be selected to separate high-score actions that collide in the current
projection.

The final-query shadow experiment now implements the quotient variant with
four deterministic 6-bit signatures. Its minimum preserves 92.0% of rich
prunes on the 28-letter case and 78.9% on the 40-letter `d=16` case, while
also adding a small number of complementary prunes. The result justifies a
parameter/weight-selection sweep but not immediate use as the sole online
fallback.

## Share work across many states

### Symbolic or decision-diagram construction

Many mixed-radix states may have the same finite/dead status, the same winning
action, or bound values differing only by a shared offset. A multi-valued or
edge-valued decision diagram can represent such state sets or value functions
without visiting each state independently. Transition application then acts
on sets of states rather than scalar keys.

This is a genuine construction-time idea, not just table compression, but it
is high risk. Dictionary scores may destroy value sharing even when
reachability compresses well, and decision-diagram variable order is critical.
Before implementing a package, measure:

- hashes of repeated subtables along each mixed-radix dimension;
- run lengths of equal finite/dead status and winning action;
- entropy of `H_rich - U_coarse` after conservative quantization; and
- decision-diagram node counts for reachability alone.

If only reachability compresses, use a symbolic finite-state mask to guide an
ordinary numeric DP. If value residuals also compress, an edge-valued diagram
becomes plausible.

### Coarse baseline plus residual

Store or construct the difference from a cheap complete table rather than the
absolute rich value:

```text
gap(state) = U_coarse(state) - H_rich(state) >= 0
```

The gap is likely to have more repeated zeros and small values than `H_rich`.
By itself this saves storage, not edge work. Its construction value appears
only when combined with conservative quantization, repeated-subtable sharing,
or bounded refinement that stops once the gap is large enough to prove a
concrete prune.

## Cheapen the remaining transition kernel

### Packed projected-state arithmetic

The successful kernel currently subtracts every exact requirement from the
worker bag, updates the support mask, recurses or loads, and restores the bag.
For the modest number of exact dimensions, pack counts into byte or guarded
nibble lanes and pack each action's exact delta in the same format. One or two
word/SIMD subtracts can then update the state; guard bits detect underflow, and
lane-zero detection reconstructs the support needed for the next bucket.

The mixed-radix score key still subtracts by one scalar delta. A prototype
should compare packed subtract/restore with the current support-mask plus
repeated-requirement fit path on the successful kernel, not only on rejected
actions.

### Fixed-point conservative scores

Quantize every adjusted action score upward to a fixed-point integer and run
the projected DP entirely in integer max-plus arithmetic. If at most `K`
actions can remain, either round each action toward positive infinity or add a
single `K`-dependent error allowance at lookup. The result is a slightly
looser but admissible bound.

This removes per-state floating error envelopes and `nextafter`, makes values
deterministic, and may make tiled SIMD reductions simpler. It is secondary
unless arithmetic or vectorization is shown to matter; atomic child loads and
memory traffic remain.

### Fit indexes and specializations

Smaller experiments within the recursive builder include:

- specialize zero- and one-exact-requirement actions;
- narrow packed requirements;
- sub-bucket by wildcard length and repeated-count shape;
- intersect precomputed action bitsets for count thresholds;
- cache support-compatible action lists by exact presence mask; and
- prefetch likely child slots after coarse score ordering.

These target scanned-but-rejected actions. Bitset or trie indexes can make a
multidimensional fit query cheaper, but they do not reduce the number of
successful child transitions. Promote them only if the new scan/failure
counters identify enough headroom.

## Amortize or reuse construction

For repeated identical queries, persist a completed projected table keyed by
the root bag, extraction options, restart score, projection definition, and an
index/version fingerprint. A long-lived process can also reuse immutable
per-class score and letter metadata, but root filtering, projection deltas, and
action deduplication remain query-specific.

Persistence does not help a one-shot command and invalidation is easy to get
wrong, so it should not displace algorithmic work without evidence of a
repeated-query workload. Background refinement is more generally useful
because it can exploit otherwise idle cores even for one query.

## Measurements still needed

The opt-in diagnostics now cover total action scans, fit failures by reason,
fitting-to-dead edges, ready child hits, first-owner claims, ownership
conflicts, dependency-spin iterations, and aggregate finite/dead states.
The remaining useful diagnostics are:

- finite versus dead constructed states split by total-letter layer;
- the winning action's position in score and coarse-envelope order;
- shadow coarse-envelope rejects, including whole rejected suffixes;
- bound-gap histograms and stronger-fallback disagreement on the now-measured
  concrete-DFS query stream;
- overlap among forward-reachable, reverse-completable, and final-query states;
  and
- per-bucket action, state, scan, fit, and winner distributions.

New hot-path counters should follow the implemented worker-local pattern and
be merged after construction so instrumentation does not add contention.

The best initial probes do not require a new production builder:

1. Build a shadow length-only envelope and count certified losing edges while
   leaving the current maximum unchanged.
2. Run composite-dominance analysis during action preparation without deleting
   actions.
3. Extend the aggregate fitting-to-dead counts to layer densities.
4. Replay projected keys requested by concrete DFS against a stronger cheap
   fallback; length-only preserved too few rich prunes.
5. Hash value and reachability subtables to reject or justify symbolic work.

## Research order

The recommended order is based on ability to remove work and cost of learning,
not on additional projection-depth timings:

1. Extend the implemented scan, dead-child, and ownership diagnostics with
   winner order, layer density, and final-query diagnostics.
2. Prototype hierarchical candidate certificates in shadow mode. Enable exact
   edge rejection only if the shadow rate is material.
3. Measure composite action dominance and the forward/reverse state overlap.
4. Prototype search-driven lookup with an always-available coarse fallback;
   then allow nonblocking background refinement.
5. Compare the existing forward recursion with reverse finite-state generation
   and a layered max-plus kernel on a small representative case.
6. Use actual prune misses to select a complementary projection or one adaptive
   group split. Evaluate construction transitions per useful prune.
7. Try packed state arithmetic and fixed-point values on whichever scheduler
   survives the higher-level experiments.
8. Pursue symbolic value construction only if repeated-subtable measurements
   show substantial structure.
9. Add persistent reuse only for a demonstrated repeated-query workload.

## Related research directions

The terminology above comes from pattern databases and abstraction heuristics,
but the proposals still need local validation:

- [Searching Without a Heuristic: Efficient Use of Abstraction][switchback]
  motivates on-demand hierarchical abstraction rather than exhaustive
  precomputation.
- [Online Saturated Cost Partitioning for Classical Planning][online-scp]
  is a useful analogue for moving abstraction work online and extending it
  during search.
- [On Creating Complementary Pattern Databases][complementary-pdb] selects new
  projections to complement observed weaknesses of existing ones.
- [Automated Pattern Database Design][automated-pdb] treats projection choice
  as an optimization problem and uses symbolic PDB construction.
- [External Symbolic Heuristic Search with Pattern Databases][symbolic-pdb]
  describes decision-diagram state-set operations for PDBs.
- [Counterexample-guided Cartesian abstraction refinement and saturated cost
  partitioning][cegar] supplies the closest research vocabulary for adaptive
  wildcard-group splitting.

[switchback]: https://ojs.aaai.org/index.php/AAAI/article/download/7563/7424
[online-scp]: https://ojs.aaai.org/index.php/ICAPS/article/view/15976
[complementary-pdb]: https://www.ijcai.org/proceedings/2017/601
[automated-pdb]: https://dl.aaai.org/Papers/Workshops/2006/WS-06-08/WS06-08-003.pdf
[symbolic-pdb]: https://cdn.aaai.org/ICAPS/2005/ICAPS05-006.pdf
[cegar]: https://edoc.unibas.ch/entities/publication/d96e1300-319e-4c33-9607-ebe5cea1517c
