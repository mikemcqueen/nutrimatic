# rebuild-pairs: nested cuts

Changes to the path rules of `plans/rebuild-pairs.md`. A pair cut may sit
directly below an `Nk` cut, so a second ppb round can rank its prospects
within the smaller pool by the first round's counts:

    pb3-dict/pp-1sq-3k/ppb3.pb/2k/pp-1sq-3k/ppb2.pb2

Here `2k` is every seed pair of the top 2,000 `ppb3.pb` words, and the second
`pp-1sq-3k` is the top 3,000 by `pair-prior` over the `ppb3.pb` counts of
those pairs that `ppb3.pb` ranked. In `private/ppb-pair-cuts.md` ("The count
behind a nested `pp-1sq` cut") that ranking, `q1*p²`, keeps all 9 in-cut
correct pairs within 2k, against 6 for the current nested cut's `c2*p²`. The
round-2 base `pb3` that the current path needs
(`ppb3.pb/1.5k/pb3/pp-1sq-3k/ppb3.pb`) is dropped: it only supplied the `c2`
count.

When this is built, its rules replace the matching sections of
`plans/rebuild-pairs.md`, and the rules they replace move to its "Previous
steps:" section.

## Cut kinds

| Cut | Kind | Its pairs |
|---|---|---|
| `Nk` | word | every seed pair of the top N words of the step above |
| `pp-NAME-Nk` | pair | the top N thousand pairs of its parent's pool by `pair-prior` |
| `union-Nk` | pair | as now |

`rebuild-pairs.py` keeps this as a table from cut pattern to kind, read by
the path check and by the `ppb` PAIRS lookup. The name `Nk` stays.

## Paths

A path is any number of `u-LETTERS` directories, then a step, then any
number of links, each of which is cuts followed by a step, and optionally
cuts at the end:

    step ( cuts step )* [cuts]
    cuts = Nk | pp-NAME-Nk | Nk pp-NAME-Nk | union-Nk

So between two steps there is a word cut, a `pp` cut, a word cut followed by
a `pp` cut, or a `union` cut. Rejected:

* a word cut below any cut
* a pair cut below a pair cut
* a `union-Nk` cut anywhere but directly below a `ppb` step that is directly
  below a `pp-NAME-Nk` cut, as now

Every cut narrows its parent: a word cut's parent is a step, and a pair
cut's pairs are a subset of its parent's.

## Rankings and pools

A cut takes its ranking from the nearest step above it and its pool from its
parent:

* `Nk`: the step's `words`; the pool is the seed pairs of those words. Its
  parent is always that step.
* `pp-NAME-Nk` directly below a step: as now, the step's `pairs.ranked`.
* `pp-NAME-Nk` below an `Nk` cut: the step's `pairs.ranked`, keeping only the
  rows whose pair is in the `Nk` cut's `pairs`. These are the `Nk` cut's
  pairs that the step ranked. Seed pairs the step never counted are left
  out and can't be prospects. Below the 1.5k cuts of `ppb3.pb` that is 23-34
  of 18-24k seed pairs on S7+S8, S7+S9 and S8+S9, none of them a known
  correct pair:

      awk 'NR==FNR{k[$1];next} ($2","$3) in k' NK/pairs STEP/pairs.ranked |
        pair-prior --ranked --manifest idx/pp.NAME --as-pairs -n N' - > pairs.tmp

  The `Nk` cut's `pairs` already passed the sentence's no-pairs, letters and
  used-letters filters, so there is no `pfilter`. Its inputs are the `Nk`
  cut's `pairs` and the step's `pairs.ranked`, so it goes stale with either.
* `union-Nk`: as now.

The ranking step can be a `ppb` step. In the example the second
`pp-1sq-3k`'s nearest step is `ppb3.pb`, so it uses the round-1 `ppb3.pb`
counts without any search for a `ppb` ancestor.

## ppb steps

A `ppb[N].pb[M]` step sits directly below a pair cut. That cut's `pairs` is
`--pp`.

Its PAIRS node is the nearest `pbN` step or `Nk` cut above it, passing over
`ppb` steps and pair cuts, and that node's `pairs` is PAIRS. A `ppb` step is
never a PAIRS node: its `pairs` holds only pairs in one of its combinations,
so it can lack pool pairs. In the example the PAIRS node of `ppb3.pb` is
`pb3-dict` and that of `ppb2.pb2` is `2k`. This replaces the "pool step" of
`plans/rebuild-pairs.md`.

The prospects are always within the PAIRS node's pairs: a pair cut's pairs
come from its parent's pool, and every pool between the PAIRS node and the
`ppb` step is the PAIRS node's or a subset of it.

A `ppb` step is rejected:

* when its parent is not a pair cut
* when the search for its PAIRS node reaches a `pbNwb` step first
* when its PAIRS node is a `pbN` step and N + M is less than that step's pick
  count, as now

An `Nk` PAIRS node has no pick-count rule: its `pairs` is every seed pair of
its words, not only the pairs in some combination. The two current rules
about `ppb` steps below an `Nk` cut (an `-xN` root pool, a pool below a `pp`
or `union` cut) go: they exist because an `Nk` cut could be a `ppb` step's
prospects, and it no longer can.

## pbN and pbNwb steps

Unchanged: a later `pbN` or `pbNwb` step reads the `pairs` of the cut
directly above it, of either kind. Below `Nk/pp-NAME-Nk` that is the `pp`
cut's pairs.

## rebuild-pairs.py

Steps and cuts no longer alternate, so every place that finds a node by its
position changes to find it by kind:

* `pool_of`, which steps back by two from the node above the step
* `path_error`: the pattern chosen by `index % 2`, the union check at
  `index - 2`, the step walk over `rest[::2]` and the cut at
  `rest[2 * index - 1]`, and the root-only suffix checks, which take a
  step's ordinal as its position
* the "alternating" wording of the module docstring and of the path error

## Existing directories

No directory under `tmp/` has a `ppb` step directly below an `Nk` cut, so
every existing path stays valid. The commands of existing nodes don't
change, so none goes stale.
