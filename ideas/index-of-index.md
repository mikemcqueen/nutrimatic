# Static index preprocessing for the projected bound

## IDEA

regarding the ongoing research in findings/dsf-projected-cache-optimization.md,
i'm wondering, is there some additional pre-processing we can do on the index
that will give us additional useful data , for example, to help calculate the
number of trees or branches or whatever, for particular starting characters, or
rare characters or something.  i just feel like a lot of the tuning attempts
based on "live" parameters hasn't really hit paydirt.  and it seems to me there
*is* structure inhererent in the index (or there could be, in a secondary index
of the index), that could be leveraged for all-cases optimizations, or for speeding
up the building of the dense cache or projected cache for example.

## ANALYSIS

### Short answer

Yes, but not the kind of statistic that first suggests itself. Per-letter
statistics are measurably a dead end. The exploitable static structure is the
corpus's **word-level anagram dictionary**, and the reason it matters is that
the projected recurrence is approximating a function that does not depend on
the query bag at all.

### Measured: per-letter statistics are a dead end

The natural "rare characters carry information" idea has a precise form.
Every action `a` has adjusted weight `w(a) = log count(a) + R < 0`, where
`R = log(restart) - log(corpus_total)`; write `cost(a) = -w(a) > 0`. If a price
vector `p >= 0` satisfies

```text
p . v(a) <= cost(a)    for every corpus action a
```

then every complete partition of a bag `b` costs at least `p . b`, so
`-p . b` is an admissible upper bound on the score. The feasible set
`{p >= 0 : V p <= c}` depends only on the corpus, not on the bag, so `p` can be
precomputed once. Length-only is exactly the uniform-`p` member of that
polytope, so the LP bound is provably no worse.

It is also barely better. On the 28-letter `S6` prefix (92,694 classes,
16 distinct letters, `-m 4`):

| bound | root value |
|---|---:|
| length-only recurrence | -51.145 |
| LP price bound (linear `p`) | -51.922 |

Generalising from linear prices to separable per-letter-count functions —
`cost(a) >= sum_l g_l(v_a(l))` with nondecreasing `g_l`, encoded as nonnegative
increments so the LP can say "the second `u` costs far more than the first" —
does not rescue it. Against the exact recurrence on sampled sub-bags:

| bound | mean gap vs exact, 20 sub-bags of 8--14 letters |
|---|---:|
| length-only | 28.87 |
| separable per-letter-count | 28.26 |
| min of the two | 26.77 |

The gaps are in log units, against exact values around -50. Both relaxations
overestimate the achievable score by roughly `e^27`. Tuning the price vector on
the root bag alone makes it *worse* than length-only on sub-bags, because the
LP optimum is bag-specific even though the polytope is not.

This is the same wall the existing findings hit from two other directions:
length-only preserves 40--52% of rich prunes, and a 9-bit x 2 modular signature
preserves 79--89%. All three are separable or hash relaxations of what is
really a set-partition feasibility question. The binding constraint is not how
rare a letter is; it is whether a *specific combination* of letters is
spellable. No amount of per-letter statistics can represent that, so the
"weighted and modular scalar projections" family in the research inventory
should be treated as measured-weak rather than unexplored.

The rich exact-letter projection works precisely because it is the one
construction here that keeps joint rare-letter counts. What it is rebuilding,
per query, is a fragment of a static object.

### Structural fact 1: the bound function is query-independent

```text
H(B) = max over classes c fitting B of (score(c) + R + H(B - c))
H(empty) = 0
```

`score(c)` is `log` of the best member count for key `c`, a property of the
corpus. `R` is a property of the corpus and the fixed restart constant. The
class list a query builds is `{global classes c : c is a sub-multiset of bag}`,
and for any `B` contained in the bag, the classes fitting `B` are the same set
regardless of which bag they were extracted under.

So `H` is a single global function on letter multisets. Phase 1 and the
projected builder rediscover a fragment of it on every invocation. The only
genuinely query-dependent input is `min_word_len` — and that ranges over a
handful of values.

Two consequences follow immediately:

- A table built over **all** corpus classes rather than the bag-fitting subset
  is still admissible for every bag. It is weaker, because it admits actions
  the bag cannot supply, but it never underestimates. Staticness costs bound
  quality, not correctness.
- If the exact dimensions of a static projection are the **corpus-rarest**
  letters, most of that loss is recovered for free: a bag with no `q` has
  `q`-count 0 in the key, so any action requiring a `q` fails the exact
  dimension test. Only wildcard-bucket actions leak. Rare letters are both the
  ones most often absent from a bag and the ones the exact dimensions filter
  precisely.

### Structural fact 2: short classes are single words

With minimum word length `m`, a class of at most `2m - 1` letters cannot be
split into two words, so it is exactly one corpus word. At the production
`-m 4` this was confirmed directly: all 28,992 classes of 7 or fewer letters in
the 28-letter bag had `word_count == 1`.

The corollary is the useful part. The set of **completable** multisets of size
at most `2m - 1` is exactly the set of corpus word keys of that size. Deciding
whether a short residual bag is dead is a static membership test, not a search.
More generally the completable multisets of size `s` are exactly the sums of
word keys of size at least `m`, so each layer's feasible set is a sumset power
of one static dictionary and can be built bottom-up once.

This lands directly on an existing open item. The findings measure that
99.97--100% of edges to dead children land in layers with at most seven letters
remaining, and recommend prototyping a per-query "reverse-completable perimeter
through layer seven" costing 16--105 KiB. That perimeter is a per-bag shadow of
a corpus-static object that never needs to be recomputed.

That object was measured directly. Walking the whole index with `dump-index`
and canonicalising every single-word entry gives these distinct anagram-key
counts for `wiki-merged.5.index`:

| word length | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| distinct keys | 40,551 | 77,620 | 132,897 | 208,329 | 279,166 | 289,174 | 257,273 | 193,461 | 130,645 |

So at `-m 4` the complete set of completable multisets through layer seven is
40,551 + 77,620 + 132,897 + 208,329 = **459,397 keys**. A `<= 7`-letter key over
36 symbols packs into 42 bits, so key plus a four-byte score is about 4.6 MiB
for the entire corpus, covering every query. The whole 4--12 letter word-key
dictionary is 1,609,116 entries.

Layers of eight or more letters also admit two-word sums, so their feasible sets
are the sumset of the dictionary with itself and are **not** measured here. The
layer-seven boundary is the one the findings already identify as capturing
essentially all repeatedly encountered dead children, and it is exactly the
boundary below which the static object is provably single-word.

### Proposal A: word-key dictionary and an exact low-layer tail table

Build once per index, per `m`:

1. `W`: sorted-letter word key -> best `log count`, over all corpus words.
   Measured at 1,609,116 entries for lengths 4--12.
2. `T_N`: exact `H` for every completable multiset of at most `N` letters,
   built bottom-up by convolving `W` with itself. At `N = 2m - 1 = 7` no
   convolution is needed — `T_7` *is* the length-4..7 slice of `W`, 459,397
   entries, about 4.6 MiB packed. Going beyond layer seven requires the
   two-word sumset, whose size is not yet measured.

Query-time use:

- the recurrence terminates at layer `N` with an exact value instead of
  recursing to the empty bag;
- the dead/finite mask at low layers is an exact static lookup, removing the
  6.7--10.3% of fitting edges that currently discover dead children
  recursively, and removing the per-query preflight that would otherwise
  produce it;
- the number of fitting actions for any short residual is a table lookup, which
  is the closest well-posed version of "how many branches does this subtree
  have".

This is exact rather than a relaxation, so unlike the modular fallback it
cannot expose extra nodes.

### Proposal B: a static projected table as the always-available fallback

Recommendation 7 in the findings asks for a complete small exact-letter
projection as the primary always-available fallback. Structural fact 1 says
that table does not have to be a per-query build. Precompute it over the whole
corpus with corpus-static rarest letters and mmap it.

A concrete shape: the 8 corpus-rarest letters at counts 0..3, times a wildcard
span of 0..63, is `4^8 * 64` = 4.2M entries, about 16 MiB at four bytes. That
is a file, not a setup cost.

The measurement this needs before implementation is how much weaker the
corpus-wide table is than the bag-restricted table of the same shape, on the
existing `S6` and unrelated-bag workloads. Structural fact 1's second bullet
predicts the loss is concentrated in the wildcard bucket, but that is a
prediction, not a result.

### Proposal C (weakest): static branching census for shape selection

For each letter and multiplicity, the corpus-wide count of classes containing
at least that many copies, plus the best score among them. This bounds the
branching factor at any canonical node whose rarest remaining letter is that
letter, without touching the query.

Flagged as the weakest of the three because the gap measurements above say
final node counts are driven mainly by bound tightness, not raw branching. It
is a cheap feature to add to a depth-selection model, not a predictor on its
own.

### Reproduction

```sh
# dump the phase-1 class list for a bag
dump-classes idx/wiki-merged.5.index "${S6:0:28}" 4 > classes28.txt

# LP price bound and separable per-letter-count bound vs the exact recurrence
python prices.py  classes28.txt "${S6:0:28}" 3586472603
python prices2.py classes28.txt "${S6:0:28}" 3586472603

# corpus-wide distinct single-word anagram keys by length
build/dump-index idx/wiki-merged.5.index \
  | awk '<canonicalise single-word entries to sorted keys>' | sort -u
```

The corpus total for `wiki-merged.5.index` is 3,586,472,603.

# Round 2: what phase 2 actually spends time on

The proposals above were written without a profile of the concrete DFS. A
node-layer and candidate-scan instrumentation run changes which static objects
are worth building, and retires one of them.

## Measured: the search is candidate generation, not bound evaluation

Instrumented `NUTRIMATIC_PROJECTED_SCORE=1` runs, `-m 4 -n 1000`,
`wiki-merged.5.index`:

| | 28 letters, `-C 8` | 40 letters, `-C 32` |
|---|---:|---:|
| DFS nodes | 3,086,650 | 342,949,072 |
| nodes pruned on arrival | 3,080,422 | 342,767,768 |
| prune rate | 99.798% | 99.947% |
| nodes that expand | 6,228 | 181,304 |
| fitting children generated | 3,086,649 | 342,949,071 |
| classes fit-tested (scans) | 42,104,932 | 9,732,582,720 |
| scans per expanded node | 6,760 | 53,681 |
| children per expanded node | 496 | 1,891 |
| scans per fitting child | 13.6 | 28.4 |

At 40 letters the search does **9.73 billion class fit tests** to produce 343
million children, of which 181 thousand survive their bound test. The useful
search tree is five orders of magnitude smaller than the work done to find it.

This inverts the emphasis in `findings/dfs-projected-cache-optimization.md`.
That document optimizes the projected *construction* (43.4s setup at 40
letters) and treats the 36s search as bound-quality-limited. The bound is not
the limit: it already rejects 99.95% of what it sees. The limit is that a node
has to enumerate and fit-test ~53,681 classes to discover that.

## Measured: the length-only certificate removes 73% of the fit tests

Shadow experiment. At each expanding node, before scanning the bucket, test
each consumed length `len` once:

```text
rep + max_score(rarest_symbol, len) + restart + U(letters_left - len) <= floor
```

`max_score(symbol, len)` is the largest class score in that rarest-letter
bucket at that length — an over-estimate over the whole bucket, so the test is
conservative. `U` is the static length-only tail bound. Classes are already
stored contiguously by length within a bucket, so a rejected length is a range
skip, not a per-class test.

| | 28 letters | 40 letters |
|---|---:|---:|
| length-group tests | 36,181 | 1,878,833 |
| groups rejected | 18,662 (51.6%) | 1,143,624 (60.9%) |
| **scans skipped** | 20,222,320 (48.0%) | **7,085,431,067 (72.8%)** |
| per-class certificate rejects | 1,592,668 (51.6%) | 177,120,392 (51.6%) |

One test per length group removes 72.8% of the 9.73 billion fit tests at 40
letters, exactly and with no effect on output. The per-class row is what
score-descending suffix rejection inside a surviving group would add on top.

Cost: `max_score[symbol][len]` is one pass over the class list, and `U` is a
41-entry DP. Both are kilobytes. Neither has to be static — but both *can* be,
and a corpus-wide `U` is what makes the same table valid for every bag.

This is the largest measured win available in this area and it needs no new
data structure. It is now implemented; see Proposal D below for what the
timed version actually did.

## Measured: the exact tail table does not pay at production scale

Proposal A's `T_N` terminates the recurrence with an exact value below `N`
remaining letters. Splitting the 40-letter children by remaining letters shows
why that is worth much less than it looks:

| remaining letters | children | share | rejected by the static length certificate |
|---|---:|---:|---:|
| <= 9 | 47,934,686 | 14.0% | 94.3% |
| 10--14 | 64,050,975 | 18.7% | 94.3% |
| **15--25** | **190,549,317** | **55.6%** | **16.3%** |
| >= 26 | 40,414,093 | 11.8% | 99.9% |

An exact `T_9` can only convert the 5.7% of the `<= 9` band that the free
length bound already misses: **+0.80 percentage points** of all children. Even
an exact `T_14` — which needs the two- and three-word sumset and roughly 3.3
GiB dense over 26 letters — adds only +1.86 points.

Dense universe sizes over 26 letters, for reference:

| N | multisets of size N | cumulative | at 4 bytes |
|---:|---:|---:|---:|
| 7 | 3,365,856 | 4,272,047 | 16 MiB |
| 8 | 13,884,156 | 18,156,203 | 69 MiB |
| 9 | 52,451,256 | 70,607,459 | 269 MiB |
| 10 | 183,579,396 | 254,186,855 | 970 MiB |
| 12 | 1,852,482,996 | 2,707,475,147 | 10.1 GiB |

`T_8`/`T_9` are cheap enough to build and mmap, and they do remove 14% of DFS
nodes by answering them with a lookup instead of a bucket scan. But as a
*pruning* device the static tail table is retired: the free length bound is
already near-exact wherever the tail table can reach. Proposal A should drop
below Proposal D in priority.

(One caveat worth keeping: `T_N` for `N >= 2m` must be built over index
*segments*, not words. A 8- or 9-letter class can be a two-word phrase whose
index count is its own, not the product of its words.)

## The whole problem is the 15--25 remaining-letter band

55.6% of all children sit where the static length certificate rejects 16.3%,
and it is the band no exact tail table can reach. Everything else is already
handled by a table that costs 41 doubles.

The reason the coarse bound works at both ends and fails in the middle is
scale. `restart = 1e-6` against a corpus total of 3,586,472,603 gives

```text
R = log(1e-6) - log(3586472603) = -35.816
```

Every additional segment costs 35.816 in log units. The measured length-only
gap against the exact recurrence is 24--28 log units — *less than one
segment*. Near the root and near the leaves the segment count is nearly
determined and the coarse bound is effectively exact. In the middle band the
residual admits several different segment counts, the sub-segment gap is
decisive, and a bound that is off by three quarters of a segment resolves
nothing.

Any static object proposed from here should be evaluated by one number: its
rejection rate on children with 15--25 remaining letters at 40 letters.

## Proposal D: static group certificates — implemented, and it is the win

Shipped behind `NUTRIMATIC_LENGTH_CERTIFICATE=1`, with a score-descending
refinement behind `NUTRIMATIC_LENGTH_CERTIFICATE_SUFFIX=1` and a non-intrusive
`NUTRIMATIC_LENGTH_CERTIFICATE_SHADOW=1`. Full measurements are in
`findings/dfs-projected-cache-optimization.md` under "Concrete-search
length-group certificates". The headline numbers:

| workload | production shape today | best certified | ratio |
|---|---:|---:|---:|
| 28-letter `S6` | 1.164s (`d=14`) | 1.118s (`d=14`) | 1.04x |
| 38-letter `S6` | 27.621s (`d=15`) | 7.396s (`d=13`) | 3.73x |
| 40-letter `S6` | 47.104s (`d=15`) | 19.818s (`d=14`) | 2.38x |
| unrelated 29 | 40.923s (`d=17`) | 1.268s (`d=11`) | 32.3x |

The shadow counters reproduce the scratch-patch numbers in this document to
0.003%: at 40 letters `d=15`, one test per length group rejects 60.79% of
1,805,219 groups and removes 6,723,180,516 of 9,732,285,204 class fit tests
with byte-identical output. Tables are 15,304 bytes and 8ms to build.

Two things this document did not predict:

- **The certificate substitutes for projection richness.** On the unrelated bag
  the certified node count only falls from 11.1M at `d=8` to 6.8M at `d=13`,
  against 78.4M at uncertified `d=12`. The best certified point uses a 276 KiB
  table where the automatic selector chose 23.9 MiB. Bound quality is no longer
  the binding constraint there, which is a stronger statement than "the
  certificate is worth 2x".
- **Score-descending suffix rejection is not free.** It removes 84--96% of all
  fit tests rather than 49--91%, but at 40 letters `d=15` it made search
  *slower* (11.03s to 12.08s) while examining half as many classes. Confining
  the permutation to one length group preserves the entry-point tie-break but
  still costs successful-edge locality, the same effect the findings' earlier
  wildcard-length ordering experiment hit. It wins at the small depths a
  certified search should select and loses at over-rich ones.

The original static form of the proposal follows and is still the right shape:

1. `U[n]`: corpus-wide length-only tail bound, per index and per `m`. 41
   doubles.
2. `max_score[symbol][len]`: corpus-wide best class score by rarest symbol and
   consumed length. Under 6 KiB.
3. Within each `(symbol, len)` group, store classes in score-descending order
   with prefix maxima, so a surviving group can still stop early.

The implemented version is still bag-restricted: `max_score` is taken over the
extracted class list and `U` is built from the projected actions. Making it
corpus-static is now the open measurement, and it is worth doing for a
different reason than originally stated. A corpus-wide `max_score` and `U`
would let the certificate run *before* any projected table exists, which is
what would let the selector choose a much smaller `d` — or, on workloads like
the unrelated bag, ask whether a rich table is needed at all. The first number
to get is how much the skip rate drops when both tables are taken over the
whole corpus rather than the extracted class list.

One caution for that experiment: `U` currently comes from
`prepare_projected_length_bounds()`, which depends on the projected action set,
so the certificate cannot yet be used in `SCORE_BOUND_OFF` or dense modes. A
corpus-static `U` removes that coupling.

## Proposal E: letter-fiber abstractions as the always-available fallback

Recommendation 7 in the findings wants a complete small exact-letter
projection as a cheap fallback; Proposal B makes it static at 16 MiB. There is
a much smaller static object in the same family that measures better than
every scalar relaxation tried so far.

For a letter `l`, abstract a bag `X` to the pair `(X[l], |X|)` and an action
`a` to `(v_a(l), len(a))`. Dedupe actions by that pair keeping the maximum
score, and solve the resulting two-dimensional max-plus DP exactly:

```text
H_l(r, n) = max over (v, L) with v <= r, L <= n of  w + H_l(r - v, n - L)
H_l(0, 0) = 0
```

Every complete partition of `X` projects to a path from `(X[l], |X|)` to
`(0,0)`, so `H(X) <= H_l(X[l], |X|)` for every `l`, and the minimum over `l` is
admissible. Setting `r = 0` recovers a length bound restricted to `l`-free
actions, so `H_l` is never weaker than length-only.

Measured on the 28-letter `S6` prefix (92,694 classes, `-m 4`):

| bound | root value |
|---|---:|
| length-only recurrence | -51.145 |
| LP price bound (from the analysis above) | -51.922 |
| min over single-letter fibers | **-52.544** |

Against the exact recurrence on 20 sampled sub-bags of 8--14 letters:

| bound | mean gap |
|---|---:|
| length-only | 27.83 |
| separable per-letter-count (from the analysis above) | 28.26 |
| min of length-only and separable | 26.77 |
| min over single-letter fibers | 25.41 |
| min over two-letter fibers (6 rarest) | **24.50** |

So the fiber family beats every price-vector bound tried, for a table of
`36 x 9 x 41` doubles per index — about 100 KiB, or a few MiB with pairs. It
is the right shape for the always-available static fallback, and it is a
different construction from the failed ones: it is not a per-letter price, it
is an exact DP on the joint (letter count, length) marginal.

It is also still 24 log units from exact, i.e. it does not solve the 15--25
band. Build it as the cheap floor under everything else, not as the answer.
Caveat: these tables were built from the bag-restricted class list, so a
corpus-static version will be somewhat weaker.

## Proposal F: what a static rich table can and cannot be

Proposal B assumes the static projected table can stand in for the per-query
one. The dimension budget says otherwise. The 40-letter run selects **15 exact
letters** because the radix of each dimension is `bag[letter] + 1` — small,
because the bag is small. A corpus-static table has no bag to bound the radix,
so 15 exact dimensions over 26 letters is not representable at any cap worth
having. `4^8 x 64` is 16 MiB; `4^15 x 64` is 68 GiB.

Three ways out, in order of cost:

1. **One static table on the corpus-rarest letters**, as Proposal B says.
   Accept that it is a fallback, not a replacement, and measure it on the
   15--25 band.
2. **A small family of static tables keyed by rare-letter subset.** A bag's
   eight rarest letters are almost always drawn from the corpus's rarest
   twelve; `4^8 x 41` at 4 bytes is 10.7 MiB per subset, so a curated few dozen
   subsets is a few hundred MiB and covers most bags. Pick the subsets offline
   by frequency of occurrence across a query log.
3. **Cross-run memoization instead of a table** — Proposal G.

## Proposal G: a projected-state memo shared across different bags

Structural fact 1 says `H` does not depend on the query. The findings' section
on persistence only considers caching keyed by the root bag, which helps
nothing but a repeated identical query. The stronger statement: if the
projection dimensions and the action set are corpus-static, then an exact
value computed for a projected key during *any* run is valid during *every*
future run, for a completely different bag.

That makes an append-only on-disk memo keyed by projected state — not by root
bag — converge toward the static table without any run ever paying a full
build. First runs are slow, later runs page in what previous runs proved, and
an idle-time background filler is straightforward because entries are
independent and immutable.

The prerequisite is exactly Proposal F's static projection, including a
corpus-static rarest-letter rank order in place of the current bag-derived
dictionary frequencies. Both changes cost bound quality; that is the
measurement to run before building any of it.

## Proposal H: split the wildcard bucket statically

The projected key is currently `(exact letter counts) x (one wildcard span)`.
The wildcard dimension is pure length, which is why the analysis above finds
the loss concentrated there. Replacing it with two or three static letter-group
spans — for example vowels / frequent consonants / rare consonants — keeps the
abstraction exact and multiplies the state count by roughly the square of the
split rather than exponentially.

The static part is choosing the grouping. Offline you can afford to search over
candidate partitions of the alphabet and score each by rejection rate in the
15--25 band; online the findings' "adaptive group splitting" cannot. This is the
cheapest available attack on the band that matters.

## Measured dead ends

**Minimum segment count from letter multiplicity.** Since each segment costs
35.816, a lower bound on the segment count is worth a lot, and
`ceil(X[l] / maxmult[l])` is a 36-integer static table. It does not survive
contact with the corpus: the maximum multiplicity of a letter in any single
index entry is contaminated by junk entries.

| letter | a | s | f | m | 1 | 0 | 9 | q | w | j |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| max multiplicity | 25 | 14 | 15 | 15 | 18 | 36 | 39 | 4 | 6 | 6 |

With `maxmult[a] = 25` the bound never fires for any plausible bag. It would
need a count threshold and a length cap on the dictionary first, which is a
separate static-cleanup question.

**Replacing phase 1 with a static anagram dictionary.** Phase 1 is 0.61s and
1,594,663 trie nodes at 28 letters, 6,231,011 trie nodes at 40. It is under 1%
of phase 2. A static corpus anagram dictionary would be a fine artifact for
other reasons, but not for this.

## Revised order

1. ~~Proposal D~~ — done. Next steps inside it: make `max_score` and `U`
   corpus-static so the certificate no longer depends on the projected action
   set, then re-run the findings' depth-selection sweeps underneath it. Every
   depth conclusion in `findings/dfs-projected-cache-optimization.md` was
   measured against an uncertified search.
2. Re-ask whether the rich projected table is still worth its setup at all on
   short-to-medium bags. On the unrelated 29-letter bag a 276 KiB `d=11` table
   with certificates beats the 23.9 MiB exact-equivalent table by 32x, and
   `d=9`--`d=13` are all within 24% of each other. The interesting question is
   now the *floor*: how small can `d` go before search cost re-explodes, and is
   there a workload where `d=0` plus certificates is acceptable?
3. Proposal H (static wildcard splitting), evaluated on the 15--25 band. This
   remains the only proposal aimed at the band the certificate cannot reach:
   the certificate's rejection rate is highest near the leaves and at the root,
   for exactly the reason the length bound is — the segment count is nearly
   determined there.
4. Proposal E (letter fibers) as the always-available fallback under whatever
   scheduler the findings settle on. A fiber table is also a drop-in stronger
   `U` for the certificate: it is admissible, indexed by
   `(letter count, length)`, and about 100 KiB.
5. Proposal F/G (static projection plus cross-run memo), gated on measuring
   how much bound quality a corpus-static projection costs.
6. Proposal A (`T_8`/`T_9`) only for the 14% of nodes it turns into lookups,
   not for pruning.

## Reproduction

The layer histogram and scan counters were a scratch patch to `walk()` in
`source/dfs-search.cpp`, reverted after measuring. They add, at each expanding
node, a histogram of `letters_left` and a count of loop iterations and of
fitting children.

The length-group and per-class certificate tests are no longer a scratch patch.
They are `prepare_length_certificate()` and `walk_certified()` in
`source/dfs-search.cpp`, reached through
`NUTRIMATIC_LENGTH_CERTIFICATE`, `NUTRIMATIC_LENGTH_CERTIFICATE_SUFFIX`, and
`NUTRIMATIC_LENGTH_CERTIFICATE_SHADOW`.

```sh
export IDX=idx/wiki-merged.5.index
source ./s.sh
NUTRIMATIC_PROJECTED_SCORE=1 build/dfs-anagrams "$IDX" "${S6:0:40}" \
  -m 4 -n 1000 -C 32 -F

# fiber bounds vs the exact recurrence, from a phase-1 class dump
# (key, best member count, word count) per line
python3 fiber2.py classes28.txt "${S6:0:28}" 12345

# corpus-wide per-letter maximum multiplicity
build/dump-index "$IDX" | mawk '<single-word entries; max letter multiplicity>'
```
