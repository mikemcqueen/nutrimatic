# Integrating the `dfs-codex-1` optimization work

## Summary

The independent `dfs-codex-1` branch contains two changes worth integrating
here:

1. parallel concrete DFS; and
2. the concrete-search length-group certificate.

Its projected-builder work is largely superseded by this branch's projected
action quotient and bottom-up wildcard-vector evaluator.

The important interaction is that certificates and parallel search both make
concrete search cheaper relative to projected setup. The projection-depth
selector must therefore be recalibrated after they are integrated, and will
probably favor shallower projections, especially for top-1 and top-10.

## Code review

The reviewed parallel implementation is independent commit `30561e0`,
`Parallelize concrete DFS search`.

During review, four concurrency or worker-state bugs were found in the initial
patch and fixed in that commit:

- generic sinks could be invoked concurrently without a thread-safety
  contract;
- search workers started with an incorrect modular signature;
- modular diagnostics read the stale search-global signature instead of the
  worker-local one; and
- draining and reusing `DfsTopN` retained its previously published score
  floor.

Those fixes are recorded in
`../nutrimatic/reviews/cr-ca2543d.md`. The corrected implementation:

- enters parallel search only when the sink explicitly guarantees concurrent
  `emit()` and `score_floor()` calls;
- protects `DfsTopN`'s heap and deduplication index with one mutex while
  publishing its monotone score floor through atomics for lock-free reads;
- initializes and queries modular state from each search worker; and
- clears the published-full flag under the heap mutex when results are
  drained, so a reused sink cannot prune against its previous contents.

The atomic floor may be stale while search is active, but only in the safe
direction: a worker can observe an older, lower floor and do extra work; it
cannot use that staleness to prune a valid result.

No remaining high-severity correctness problem was found in `30561e0`. Two
caveats remain.

### Cutoff ties are nondeterministic

Exact-score ties at the Nth result can produce different equally scoring
spellings. Parallel traversal and score-descending certificate ordering both
change arrival order, while score-floor rejection uses `<=` in
`../nutrimatic/source/dfs-output.cpp`.

Byte-stable cutoff ties are not an integration requirement. Expected use
retains roughly one million results, so different equally scoring spellings in
the final score bucket are immaterial. Keep the current numeric cutoff
semantics rather than adding work to stabilize the fringe.

Validation should compare the exact sorted prefix above the cutoff-score
bucket, plus the retained count and cutoff score. A hash of the first 99% of a
large output is a convenient smoke signal, but it is not the correctness
definition and production code should not be shaped around it.

### Partial thread-creation failure misreports the worker count

If only some worker threads can be created, the shared task counter still
ensures all work is completed, but `actual_search_threads` is assigned before
thread creation and can report the requested worker count rather than the
number actually launched. This is a diagnostic issue, not a
search-correctness issue.

### Verification

The independent workspace:

- builds successfully with `conan build .`;
- passes `test-dfs-search`;
- passes `test-dfs-output`;
- passes `test-dfs-class-list`; and
- passes `git show --check` for the parallel commit.

## Parallel-search validation

A process-gated serial/parallel differential used:

```text
38-letter S6 prefix
-m 4 -n 1000 -C 32 -F -T 20
d=13
projected support groups enabled
length-certificate suffix rejection enabled
```

The results were:

| mode | setup | search | phase 2 | DFS nodes |
|---|---:|---:|---:|---:|
| serial | 0.881s | 6.945s | 7.826s | 103,587,733 |
| 20-thread concrete search | 0.881s | 0.848s | 1.728s | 103,687,032 |

This is:

- an 8.2x concrete-search speedup;
- a 4.5x phase-2 speedup;
- only 0.096% more DFS nodes; and
- identical retained output.

Both runs produced SHA-256:

```text
398abaeaeb5245dfe071f1f11d933742591230b685a6be2ac724abc35a4ffec4
```

The parallel run used roughly 40% more aggregate CPU. That is a favorable
trade for wall-time latency. Static task generation and lock-free floor reads
are appropriate for this traversal; top-N output locking is rare relative to
the hundreds of millions of visited nodes in this top-1,000 measurement.

The expected production target is closer to top-1,000,000. At that size,
output expansion and heap locking may occupy a materially larger share of the
run. Parallel-search retention and thread-count selection must therefore
include a million-result benchmark rather than extrapolating from this result.

## Recommended integration order

### 1. Port the group-only length certificate

This is the strongest algorithmic result in
`../nutrimatic/findings/dfs-projected-cache-optimization.md`.

It:

- exactly skips 49--91% of class scans on the measured workloads;
- costs only kilobytes of tables and milliseconds of preparation;
- does not depend on projection richness; and
- preserves the existing class traversal order.

For this branch, construct the length-only tail bound `U` directly from the
bag-restricted extracted class list. This keeps the certificate independent of
the projected evaluator and makes it available even before a projected table
is selected. Quotienting preserves the best score for each consumed length,
so this is equivalent to deriving `U` from the projected action set for the
certificate's purpose.

### 2. Port parallel concrete DFS

Port the corrected design from `30561e0`, including:

- explicit sink opt-in covering concurrent `emit()` and `score_floor()` calls,
  with serial fallback for every other sink;
- per-worker bag, mask, score key, path, counters, and modular state;
- shallow independent search tasks;
- a shared atomic task cursor;
- a single lock protecting the top-N heap and deduplication index;
- a separately published atomic floor whose staleness can only reduce pruning;
  and
- reset of the published-full flag under the heap lock when output is drained.

This should be a manual port. The independent patch is based on a substantially
different `dfs-search.cpp`, and its `SearchWorker` includes fields and call
paths specific to that branch's certificate and recursive projected builder.

### 3. Recalibrate projection depth with both changes enabled

Neither branch's existing selector should remain unchanged.

Certificates reduce the number of candidates examined, while parallel search
reduces the wall cost of the remaining tree. Projected setup therefore becomes
relatively more expensive, favoring shallower tables.

For example, this branch's 48-letter top-1 measurements were:

| `d` | setup | search | phase 2 |
|---:|---:|---:|---:|
| 11 | 0.374s | 7.568s | 7.942s |
| 12 | 0.516s | 3.478s | 3.994s |
| 13 | 0.945s | 1.968s | 2.913s |
| 14 | 3.769s | 0.905s | 4.674s |

Applying even a conservative fraction of the measured parallel-search gain
makes `d=12` plausibly competitive with or better than `d=13` before adding
the length certificate. The combined optimum must be measured rather than
inferred.

### 4. Retain this branch's quotient and bottom-up evaluator

The independent projected-action deduplication is the same mathematical
quotient already implemented here: projected actions with the same resource
delta reach the same child, so only the largest score can affect the
recurrence.

This branch's bottom-up wildcard-vector evaluator should remain the
foundation. It:

- reuses exact-fit decisions across the entire wildcard vector;
- avoids recursive atomic ownership and wait loops;
- scales well across preprocessing threads; and
- already halved whole-program time at the winning 48-letter depth.

Do not import the independent recursive projected builder wholesale.

### 5. Keep suffix rejection adaptive or experimental

Score-descending suffix rejection removes more scans than the group-only
certificate, but:

- reorders successful work;
- can lose cache and traversal locality;
- sometimes regresses at rich projection depths; and
- makes cutoff-tie selection nondeterministic.

A better follow-up is a block-max hierarchy inside each original length group.
Store the maximum class score for each contiguous block and skip blocks whose
maximum cannot reach the floor. That retains original class order and locality
while recovering some of suffix rejection's additional scan reduction.

## Assessment of the other ideas

### Worth retaining

#### Persistent projected tables

Persistence is a major win for repeated bags or for querying the same bag with
different result limits. This branch already measured a warm projected-table
load around 0.135 seconds versus several seconds of construction.

This does not help a one-shot novel bag, and is different from the more
speculative corpus-static cross-bag memo.

#### Support indexing

Support-subset traversal materially helps the independent recursive builder.
When temporarily combined with this branch's bottom-up vector evaluator,
however, it reduced setup by only about 12%. It remains a secondary layout
optimization rather than a reason to replace the current evaluator.

#### Static wildcard splitting

Splitting the wildcard total into two or three static letter-group totals is
one of the few speculative ideas aimed directly at the difficult
15--25-remaining-letter band. It deserves a later measured prototype after the
certificate, parallel search, and selector have established a new baseline.

### Low priority or effectively retired

#### Per-letter LP prices and fibers

Linear prices, separable per-letter functions, modular bounds, and
single-letter fibers improve on a length-only bound but remain far too weak to
replace a rich exact-letter projection. They may serve as cheap fallback
bounds, not as the primary long-input optimization.

#### Exact low-layer tail table

The low-layer tail table reaches the part of the search where the length-only
bound is already nearly exact. The measured incremental pruning was under two
percentage points. It can turn some terminal searches into lookups, but is not
a leading wall-time opportunity.

#### Reverse perimeters and dead-state masks

These have limited upside under bottom-up evaluation. More than 96% of the
measured bottom-up fitting transitions already reached finite children, so a
full viability mask cannot remove much work.

#### Background or demand-driven rich construction

Cheap fallback bounds missed a small number of high-leverage queries that
exposed several times more concrete DFS nodes. Synchronously filling those
holes moved work from parallel setup to serial search, while background
construction would compete with parallel concrete search for CPU and memory
bandwidth. This is not a near-term priority.

#### Corpus-static rich tables and cross-bag memoization

The global bound function is query-independent when the index, minimum word
length, restart model, action set, and projection are fixed. However, a
corpus-static projection loses the small bag-derived radices that make
15-dimensional query projections practical. Its bound-quality loss must be
measured before investing in static tables or an append-only cross-bag memo.

## Interpreting the reported speedups

The reported 32x win on the unrelated 29-letter bag is real end to end, but it
combines certificate pruning with correcting an exceptionally poor automatic
projection choice:

```text
production selector: d=17
best certified point: d=11
```

It should not be interpreted as a 32x intrinsic speedup from one certificate
test. The certificate's more portable result is that it changes the
setup-versus-search curve enough to make much smaller projections optimal.

Likewise, the 2.4x and 3.7x S6 wins combine candidate certification with depth
reselection. That combination is exactly what production should optimize, but
the causes should remain separate in future A/B reporting.

## Likely next wall-time ceiling

For one-shot long inputs, the strongest combined path is:

```text
projected action quotient
  + bottom-up projected evaluation
  + group length certificate
  + parallel concrete DFS
  + target-aware depth selection
```

The next candidate-generation experiments should be:

1. contiguous block-max certificate metadata that preserves traversal order;
2. instrumentation of support and multiplicity failures only inside length
   groups that survive certification; and
3. a support or multiplicity index designed from those post-certificate
   counters rather than from the original scan distribution.

Finally, phase 1 should be re-evaluated after these changes. In this branch's
48-letter top-1 measurement, phase 2 took 2.913 seconds while the whole program
took 5.21 seconds, leaving roughly 2.3 seconds outside phase 2. If certificates
and parallel search reduce phase 2 below that, class extraction and index
startup become the dominant Amdahl floor.

The static anagram dictionary dismissed in
`../nutrimatic/ideas/index-of-index.md` was correctly low priority when phase 2
dominated. It may become worthwhile for optimized top-1 long inputs once the
balance changes.
