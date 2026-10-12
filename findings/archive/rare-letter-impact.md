# Rare-letter placement as a segment-review aid

Date: 2026-09-01

This is a design finding only. It considers whether rare letters can expose
useful segment-review candidates that are not obvious from a global
result-count ranking. It does not propose using letter rarity as evidence that
a segment is bad; that judgment remains human.

## Conclusion

Rare letters provide a meaningful way to structure and measure review, but
they do not give a low-result-count segment hidden elimination impact.

The cleanest case is a letter that occurs exactly once in the working letter
bag. Every result row then has exactly one DFS segment containing that letter.
Grouping rows by that segment partitions the surviving result set into
disjoint placement buckets whose result counts add up to the total number of
surviving rows.

This is more interpretable than summing segment intersections, which can count
one row several times. It can show how much of a constrained letter's
placement has already been reviewed and where the largest unresolved placement
buckets remain.

It does not replace the simple baseline of reviewing the next 50 unreviewed
segments in descending result-count order. A letter-placement view should
initially be a read-only diagnostic and a possible source of a small number of
diversity candidates.

## Two meanings of rare

There are two different notions of letter rarity.

- **English rarity:** letters such as `k`, `w`, and `y` occur in relatively few
  plausible words. Their placement may therefore be constrained even when the
  working bag contains several copies.
- **Working-bag rarity:** a letter occurs only once in the exact bag being
  searched. This gives the letter-placement calculation its disjoint-partition
  property.

English rarity is a useful intuition, but it is not itself a statistical
badness signal. Working-bag rarity gives an exact property of the current
result set.

## Unique-letter partition

Let `U` be the surviving result rows after hard-NO filtering. Let `l` be a
letter that occurs exactly once in the working bag. For each segment `s`
containing `l`, define:

```text
P_l(s) = rows in U whose l-containing segment is s
```

Each row belongs to exactly one `P_l(s)`. Therefore the placement buckets are
disjoint and:

```text
U = disjoint union over s of P_l(s)

|U| = sum over s of |P_l(s)|
```

Rejecting `s` still eliminates only the rows containing `s`. Letter rarity
does not increase that direct impact. The partition instead answers different
questions:

- Which placements dominate this letter in the surviving result set?
- How much placement coverage has already been reviewed?
- How large is the unresolved tail?
- Which large placement buckets are absent from an ordinary small review
  batch?

Confirmed YES means that a placement is plausible, not that every row in its
bucket is plausible. A row with a confirmed-YES rare-letter segment can still
contain another hard-NO segment.

## Current S7/G4 measurement

The measured input was:

```text
/home/mike/code/words/final/.wf/best/s7/u-vindiesel/m4/g4/dfs.best
```

Rows containing current hard-NO segments were removed with
`filter-segments --wf`. This left 365,961 surviving rows.

The S7 sentence letters are:

```text
the answers i already know ive done it tired and now im weak
```

After subtracting `vindiesel`, the working bag is:

```text
aaaaadddeeeeehiiikkmnnnnooorrrstttwwwwy
```

Its letter multiplicities, from lowest to highest, are:

```text
h:1 m:1 s:1 y:1 k:2 d:3 i:3 o:3 r:3 t:3 n:4 w:4 a:5 e:5
```

The unique letters `h`, `m`, `s`, and `y` therefore admit exact placement
partitions. Their observed concentration was:

| Letter | Distinct | Top 1 | Top 5 | Top 10 | Top 50 | YES coverage |
|---|---:|---:|---:|---:|---:|---:|
| `h` | 32,204 | 6.1% | 16.9% | 22.2% | 28.3% | 22.7% |
| `m` | 37,529 | 7.6% | 22.8% | 28.1% | 32.4% | 29.8% |
| `s` | 48,235 | 5.4% | 13.9% | 19.8% | 23.3% | 19.8% |
| `y` | 17,917 | 9.4% | 31.0% | 38.4% | 44.3% | 40.0% |

"Confirmed-YES placement" is the percentage of surviving rows whose
letter-containing segment is present in:

```text
/home/mike/code/words/final/.wf/classified/yes/yes.pairs
```

The leading `y` placement buckets were:

```text
result count  coverage  segment
      34,485      9.4%  wide roadway
      23,863      6.5%  kennedy widow
      22,429      6.1%  kitty hawk
      17,089      4.7%  wayne newton
      15,495      4.2%  wide eyed
       6,691      1.8%  diane sawyer
       6,473      1.8%  water deity
       4,835      1.3%  eyes wide
       4,764      1.3%  wind deity
       4,564      1.2%  winona ryder
```

This supports the observation that constrained `y` placements already rise
to the top of the ordinary segment ranking. Much of their substantial mass is
therefore obtained "for free": confirmed-YES `y` placements already account
for 40.0% of the surviving rows.

It also shows the limit. The `y` partition contains 17,917 placements, and its
top 50 cover only 44.3% of the rows. The remaining mass is a very long tail.
Selecting segments merely because they contain `y` does not provide an
effective ordering for that tail.

The contrast with `h`, `m`, and `s` is consistent with the English-rarity
intuition: their placement distributions are less concentrated than `y`'s.
The useful quantity, however, is the concentration measured in the current
result set, not a generic English letter-frequency table.

## Proposed letter-placement report

A read-only report for a once-occurring letter should show:

```text
letter
letter multiplicity in the working bag
surviving row count
distinct placement count
placement segment
placement result count
placement coverage percentage
classification or reviewed status
cumulative coverage
```

For example:

```text
letter y: 1 copy, 365,961 surviving rows, 17,917 placements
reviewed placement coverage: 40.0%

result count  coverage  cumulative  status     segment
      34,485      9.4%        9.4%  YES        wide,roadway
      23,863      6.5%       15.9%  YES        kennedy,widow
      22,429      6.1%       22.0%  YES        kitty,hawk
       ...
```

The report should validate the partition rather than silently produce
overlapping counts: every accepted row must have exactly one segment containing
the selected letter, and the placement result counts must sum to the surviving
row count.

## Relationship to co-occurrence

After a high-count rare-letter placement is confirmed YES, it may still be
useful to inspect the other segments occurring in that bucket. For anchor
segment `a` and candidate `t`, that conditional count is:

```text
|R_a ∩ R_t|
```

For example, one could inspect the most frequent undecided segments among the
34,485 rows containing `wide roadway`.

This can provide context that helps the human judge a candidate. It does not
reveal hidden direct impact:

```text
|R_a ∩ R_t| <= result_count(t)
```

Global result-count ranking will always see at least as many rows for `t` as
an anchor-specific view. The conditional view is useful only if organizing the
candidate around a constrained placement makes its badness easier to notice.

## Letters with multiple copies

The simple partition does not apply directly to `k` or `w` in the current S7
working bag. A row may contain more than one segment carrying the selected
letter, so individual containing-segment buckets overlap.

It is possible to partition rows by the complete multiset of segments that
place all copies of the letter. That placement signature is likely to fragment
quickly and is harder to review. It should not be implemented before the
unique-letter report demonstrates practical value.

## Suggested experiment

Keep incremental global review as the baseline:

1. filter rows containing hard-NO segments;
2. exclude confirmed and previously reviewed segments;
3. review the next 50 segments in descending result-count order;
4. complete the classifications; and
5. recompute.

Use the letter-placement report alongside that loop. If a diversification
experiment is desired, compare:

- 50 globally highest-result-count unreviewed segments; with
- 40 globally highest-result-count segments plus 10 large unresolved
  placement buckets from a selected once-occurring letter.

Measure each policy by:

- hard NOs found per 50 reviews;
- unique rows eliminated per batch;
- useful YES segments found; and
- additional rare-letter placement coverage reviewed.

Because badness is human-determinable, no offline co-occurrence calculation
can establish which policy is better. The comparison requires actual review
outcomes.
