# Projected dense score-bound tables

## Conclusion

The dense-prefix fallback is a dead end. The better direction from
`../nutrimatic/ideas/dfs-partial-dense-cache.md` is to stop choosing which
exact `H(bag)` entries survive and instead choose which concrete bags are
merged into a smaller abstract state space.

A raw subset of exact states leaves coverage holes. The measurements in the
idea document show that retaining only half the exact dense table increases
the final DFS node count by 226--390x. A miss removes the score bound entirely,
so it reopens the whole subtree below that state. Selecting a more favorable
prefix, suffix, depth band, or sparse retention order can refine this failure
mode but cannot remove it.

A projection retains an admissible score bound for every concrete state. It
degrades the tightness of each bound rather than deleting bounds. That is the
more promising use of a fixed dense budget.

## Basic projection

Choose a set `D` of letters whose remaining multiplicities are tracked exactly.
Merge every other letter into a wildcard pool whose remaining total is still
tracked exactly:

```text
key_D     = mixed-radix key over the exact letters
wild_left = number of remaining letters outside D
index     = key_D * (W + 1) + wild_left
```

Here `W` is the input multiplicity of all wildcard letters.

A class fits an abstract state when:

1. each of its requirements for letters in `D` fits; and
2. its total number of letters outside `D` is at most `wild_left`.

This forgets wildcard identity but never wildcard quantity. Every concrete
completion therefore maps to an abstract completion with the same class scores
and restart charges:

```text
A(project(bag)) >= H(bag)
```

The abstract value is an admissible upper bound. It may miss a prune but cannot
incorrectly prune a result.

At one endpoint, `D` is empty and the table is just `best[letters_left]`. At
the other endpoint, all but at most one distinct letter are exact and the
projection reproduces the concrete state space. This provides a continuous
memory/quality control without ever falling back to an unbounded state.

## Mapping the design onto this branch

This branch differs from the branch described by the idea document, but several
current changes make the projection easier to integrate:

- The score cache already has a separate key and per-class
  `score_key_deltas`. Those can become the projected key and projected deltas;
  the projection should not inherit the dense prefix's rarest-most-significant
  key.
- Retain upward-rounded four-byte float values.
- Retain root-slab compaction when the initially forced root letter belongs to
  `D`. All non-root states have a lower count for that exact letter.
- Eagerly and concurrently construct the complete abstract table. Delete the
  serial lazy dense-prefix construction.
- Return score-cache bytes not required by the selected abstraction to the
  support and fitting-candidate caches.

A flat projected index can also be maintained incrementally. If a class has
exact-letter delta `delta_D` and wildcard length `wild_length`, its flat delta
is:

```text
delta = delta_D * (W + 1) + wild_length
```

Subtracting a fitting class cannot borrow across either the mixed-radix digits
or the wildcard count.

The projected DP needs projection-specific class traversal. The existing class
buckets are keyed by a class's globally rarest letter. They are sufficient
when `D` is a corpus-rarest prefix, but an arbitrary `D` can classify a letter
as wildcard even when it is globally rarer than the forced exact letter.
Build action buckets keyed by each class's rarest required letter in `D`, plus
a wildcard-only action list.

## The 32-letter, 32 MiB workload

The measured input has 17 distinct letters and 23,514,624 theoretical concrete
states. In the current implementation:

```text
effective non-root states = 11,757,312
complete float bytes      = 47,029,248 (44.85 MiB)
```

At `-C 32`, dense-prefix mode instead allocates 8,388,608 exact slots, consumes
the whole budget, disables candidate caching, and constructs large dependency
closures serially during search.

Using the approximate corpus-rarity order from the idea document, a projection
that keeps the first 15 letters exact and merges `a` and `e` has:

```text
projected states before root compaction = 10,077,696
projected float bytes after compaction  = 20,155,392 (19.22 MiB)
```

That would leave roughly 12.8 MiB of a 32 MiB budget for support and candidate
caching while providing a score bound at every state. Keeping 14 rare letters
exact would use approximately 10.68 MiB after float storage and root
compaction.

These sizes do not establish that merging `a` and `e` is optimal. It gives
strong compression because those letters have high multiplicity, but confusing
two common letters may also weaken the bound substantially. Choosing which
letters to merge is the actual tuning problem.

## Tune merges, not entries

Generalize the abstraction from a rarest-prefix length `d` to:

```text
D = an arbitrary set of exact letters
G = all remaining letters merged into one wildcard group
```

For a wildcard group `G`, its contribution relative to keeping those letters
as separate exact dimensions is:

```text
compression ratio =
    (sum(m[s] for s in G) + 1) /
    product(m[s] + 1 for s in G)
```

The desired group saves many states while merging identities that rarely make
the score bound materially tighter. The objective is not to maximize score
table size. It is to minimize:

```text
abstract setup time
  + final DFS search time
  + cost from reducing support/candidate cache space
```

This is especially important for `-n 1000`: a slightly weaker but much cheaper
bound may win end to end because the heap fills early and produces a strong
score floor.

### Offline selection experiment

Extend the experiment harness from `ABSTRACT_D=d` to an arbitrary exact-letter
mask. It can collapse one completed exact table into candidate projections
without first implementing the real abstract recurrence.

For the 17-symbol query, first test every two-letter wildcard group:

1. Enumerate at most 136 pairs.
2. Discard projected tables that exceed the intended score-cache allocation.
3. Run the final DFS against each projected table.
4. Rank projections by nodes and search time per byte.
5. Implement the real abstract DP for the most promising few and measure total
   setup plus search time.

The emulated value is optimistic because it takes the maximum of completed
exact bounds in each abstract cell. A real abstract recurrence can chain
several incompatible wildcard choices and produce a looser value. The harness
is therefore a selector and rejection tool, not a substitute for measuring the
real DP.

### Cheap production heuristic

Corpus-rarity order is a sensible initial default because it preserves the
existing forced-symbol behavior, but it should not be assumed optimal.

A possible greedy proxy is projected action collision. Classes that have
different concrete requirements but the same projected consumption can be
combined incompatibly by the abstract recurrence. For each possible letter
split, estimate:

```text
reduction in high-scoring projected-action collisions /
increase in projected table bytes
```

Weight classes by score and by how often their forced-symbol bucket is likely
to be visited. Offline exact-table experiments on the 21/24/28/32-letter bags
can determine whether this proxy predicts actual pruning well enough for
automatic per-query selection.

## Generalizations

### Multiple wildcard groups

The single wildcard pool is one alphabet partition:

```text
{z}, {v}, {b}, ..., {a, e, i, o, n}
```

A more general projected state tracks the remaining total in every group. A
class consumes a vector of group counts. Every concrete path still maps to an
abstract path, so the bound remains admissible and complete.

Start with one group containing every letter and greedily split the group whose
separation yields the greatest pruning improvement per additional abstract
state. This gives a smoother tuning mechanism than deciding that every
selected dimension must be a singleton and all remaining letters must share
one pool.

### Complementary projections

Another option is to build several small projections over different letter
sets and take the minimum:

```text
bound(bag) = min(A1(project1(bag)),
                 A2(project2(bag)),
                 ...)
```

Each `Ai` is independently an upper bound on the exact completion score, so
their minimum remains admissible. Complementary projections can capture
different constraints without paying the Cartesian-product memory cost of one
large projection. Lookup cost and the sum of preprocessing work must be
measured, but two or three small tables may offer better bound quality per byte
than one larger table.

### Exact refinements over an abstract fallback

Once complete projected coverage exists, exact partial caching becomes less
dangerous. An exact-cache miss can fall back to the projected bound rather than
to no bound.

Exact residual bounds could then be built only when:

- the projected bound fails to prune but is close to the score floor;
- the residual concrete state product is below a work limit; and
- the subtree is likely to amortize the construction cost.

This turns exact entries into optional refinements rather than structural
coverage. Selection policies based on visit count, residual size, or estimated
nodes saved become plausible only after the projected fallback exists.

## Proposed implementation order

1. Remove `SCORE_BOUND_PREFIX` and its lazy construction path.
2. Implement `D = empty`, indexed only by wildcard letters remaining, to
   establish the abstract recurrence and an always-available fallback.
3. Implement a corpus-rarest-prefix `D`, retaining float values, root-slab
   compaction, and parallel eager preprocessing.
4. Sweep `d` and cache-budget splits on 21/24/28/32-letter inputs, measuring
   total setup plus search time and byte-identical output.
5. Extend the degradation harness to arbitrary exact-letter masks and tune
   which letters are merged.
6. Add projection-specific action buckets and support arbitrary `D`.
7. Explore multiple groups or complementary projections only if a single
   tuned wildcard group remains too weak.
8. Consider bounded exact refinements on top of the projected fallback.

The central design rule is:

> Exact holes are catastrophic, while deliberately weaker complete coverage
> degrades smoothly.
