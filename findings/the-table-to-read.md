# The table to read

> **Status (2026-08-05):** Demoted. Sections below stand as a record of why the
> length-conditioned percentile was withdrawn, which is still the useful part.
> The framing -- "read the table and decide" -- does not, and this document is
> not the arbiter of the scoring model. Two reasons, both recorded the same day
> the table was written:
>
> **1. The headline question is the wrong one.** Asking whether `reality
> television` is "as good as" `with` treats per-entry quality as something to be
> discovered from the index. It is not. The tool exists to find meaningful
> pairs, so pairs outrank non-pairs, two pairs outrank one, and three outrank
> two -- by stipulation, as the objective. That ordering is asserted by
> `--word-bonus`, not derived from counts, and no table of counts can confirm or
> refute it.
>
> **2. The numbers are not a stable basis anyway.** Every count here is read
> against the current index traversal. Filtering connector words out of that
> traversal -- an open option -- changes which entries exist and what the top of
> each length class is, so the measured gap between the best pair and the best
> non-pair moves with a decision that has not been made yet. Calibrating on it
> now would calibrate on a moving target.
>
> What remains live from this document is only the withdrawal argument in "Why
> the percentile proposal was withdrawn" and the double-counting observation.
> The "How to settle it" section is superseded: the answer is to set
> `--word-bonus` and inspect results.

## The question

Per-entry quality is currently $\log c$, the raw corpus count. Should it be
conditioned on the entry's letter count?

Stated concretely against the live data:

**Is `reality television` (17 letters, 12,376) as good an entry as `with`
(4 letters, 22,533,416)?**

Under today's model `with` wins by 1800x. Under any length conditioning they
are equals -- each is rank 1 of its own length class. No statistic decides
this; it is a judgment about what the tool is for.

## The table

Best three entries at each letter count, from the complete phase-1 population
for the working bag (`$S6 -u toyfastmusketsalvo -m 4 --pairs`, 869,242
entries).

```
len  rank       count  entry

4    1       22533416  with
     2       17356527  that
     3       10250540  were

5    1        6170054  their
     2        4377295  state
     3        4068659  other

6    1        1692904  womens
     2        1418811  served
     3        1278617  single

7    1        2827524  station
     2        2708300  history
     3        2284708  against

8    1        1845778  released
     2        1481157  division
     3        1075588  military

9    1         708943  relations
     2         694523  there were
     3         654529  along with

10   1         959469  television
     2         603574  originally
     3         317429  rather than

11   1         419942  traditional
     2         337333  legislative
     3         200857  alternative

12   1         243790  state highway
     2         217602  together with
     3         145649  west virginia

13   1         172269  virginia state
     2          89552  traditionally
     3          68873  administrator

14   1         345247  administrative
     2          47922  average density
     3          30370  were originally

15   1          11622  television drama
     2           8978  more traditional
     3           8976  drama television

16   1          14917  administratively
     2           7852  their traditional
     3           5636  heavyweight title

17   1          12376  reality television
     2           5356  interstate highway
     3           5241  general relativity
```

## Why the percentile proposal was withdrawn

The original suggestion was: build the distribution of $\ln c$ conditioned on
letter count, score each entry by its percentile within its own length class,
then read the result. Three problems, found only after moving from a sampled
results file to the complete phase-1 population.

**1. The motivating bias mostly does not exist.** The sample suggested
$\ln c$ falling about 0.5 nats per letter. On the full population the medians
are nearly flat:

| letters | 8 | 10 | 12 | 14 | 16 |
|---|---:|---:|---:|---:|---:|
| median $\ln c$ | 3.00 | 2.94 | 2.71 | 2.64 | 2.48 |

About -0.04 nats per letter, not -0.5. The earlier figure was an artifact of
sampling from a top-10K results file, where selection pressure varies with
length.

**2. The transform is degenerate on this population.** Median count is about
15 across 869,242 entries, so the mass is junk. Every entry anyone would want
lands at percentile 99.99+, and the transform compresses exactly the region of
interest into nothing.

**3. What varies with length is the tail, and it is noisy and bag-specific.**
The maxima above are not monotone in length -- 7 letters peaks higher than 6
(`station` 2.83M vs `womens` 1.69M), 14 higher than 13 (`administrative` 345K
vs `virginia state` 172K). And they are the best entries makeable *from this
bag*, so the conditioning distribution is input-local and unstable between
queries. That is the same input-local calibration recorded as a failure in
[q_semantic_plausibility.md](q_semantic_plausibility.md), by a different
mechanism.

## The double-counting argument

Segments must tile the bag. One never chooses between a 4-letter and a
17-letter entry in isolation -- one chooses a partition summing to the bag
size. Selecting `reality television` (17) from a 29-letter bag leaves 12
letters that must become three 4-letter entries under `-m 4`, and those are
the millions-count ones.

Length is therefore already priced in by the partition constraint. Conditioning
per-entry quality on length may double-count it.

## How to settle it (superseded)

The original text said: read the table, and if `reality television` and `with`
feel like peers, pursue length conditioning with a tail-aware statistic;
otherwise close the per-entry question.

That is not how this gets settled. The preference for phrases is an objective,
so it is asserted and then tuned:

```bash
build/dfs-anagrams $IDX "$S6" -u toyfastmusketsalvo -m 4 --pairs -g 3 -n 6 \
    --word-bonus 1
```

At `--word-bonus 0` the top three-segment answers are one pair plus two
singles (`traditionally,however some,eight`); at `1` they are three pairs
(`that they,original movie,were sold`). The knob does what it is supposed to
do, and where it should sit is a question for inspection, not for this table.

Length conditioning is separate and still dead for the reasons above.

## Reproducing

No new flag is required and no search runs; this is post-processing over
phase 1.

```bash
build/query-index $IDX "$S6" -u toyfastmusketsalvo -m 4 -n 0 --pairs > entries.txt
```

Output is `count text` per line. Letter count and word count derive from the
text. The one thing phase-1 data cannot report is whether an entry is usable
in a complete answer; that needs `--require-completable` and its phase-2 cost.
Irrelevant for reading the table, but it would matter if a conditioning
distribution were built for production scoring, since uncompletable entries
would pad it.
