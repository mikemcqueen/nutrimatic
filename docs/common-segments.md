# `common-segments`

## How it works

```text
common-segments RESULTS PAIRS --wf -t FULL-TARGET
```

`RESULTS` holds `dfs-anagrams` result rows, each of the form:

```text
score segment[,segment ...]
```

`PAIRS` holds `word,word` lines in `best.pairs` format. The score is parsed but
does not affect the result.

A row *holds* a pair when one of its comma-delimited segments is that pair's
two words in either order. This is a complete segment match, not a substring:
the row `7 tree homes,red fox` does not hold `tree,home`. Both orientations
count, because the pair file is loaded with both word orders, and `hobbit home`
and `home hobbit` are one pair rather than two.

Printed, one per line in ascending order, is every segment appearing on a row
that holds two or more distinct pairs, less the pairs that row is credited for:

- a row holding fewer than two pairs contributes nothing;
- a row holding exactly two contributes its non-pair segments only, since the
  one intersection it belongs to excludes both of its pairs;
- a row holding three or more contributes every segment, since each of its
  pairs lies in the intersection of two others.

Rows are processed and discarded as they are read, so nothing scales with the
size of `RESULTS`.

The row filters apply first, in `top-segments`' precedence — the workflow's
classified NO pairs, the selected target's `no.pairs`, `-r/--reject`,
`-a/--allow-pairs` and then the dictionary — and drop the whole row. Segments
in `-i/--ignore`, and with `-y/--yes` the workflow's classified YES pairs, are
left out of the output without dropping their row.

Output is pair-file format: multi-word segments are printed as `word,word` and
solo words bare, which is what `top-segments --pairs` emits, so the output
feeds back in as a pairs file. A segment is printed as the row spelled it, so a
pair appearing in both orientations across rows prints as both.

With `--results`, the rows themselves are printed instead: every row holding
two or more pairs, exactly as it was read, in the order the input gave them.
The row filters still apply, but the per-row rule above does not, since it
selects segments rather than rows, and neither do `-i/--ignore` and the
`-y/--yes` classified YES pairs.

With `--combos`, the counts behind those intersections are printed instead:
one line per combination of two pairs held by the same row, as

```text
COUNT PAIR1 PAIR2
```

where `COUNT` is the number of surviving rows holding both pairs. The columns
are padded to line up: `COUNT` is right aligned to the widest count printed,
and `PAIR1` is left aligned to the widest `PAIR1` printed, so `PAIR2` starts at
one column. Each pair is printed as the `PAIRS` file wrote it, in that file's
word order, whichever orientation the rows spelled, so one pair has one
spelling here. The two pairs of a line are in ascending order by that
spelling, and lines are printed by descending count, ties broken by `PAIR1`
and then `PAIR2`. As with `--results`, the row filters apply and `-i/--ignore`
and the `-y/--yes` classified YES pairs do not, since pairs are selected
rather than segments. This is the mode that holds the most state: one entry
per observed combination of two pairs.

With `--pairs`, those same intersections are counted one pair at a time, as

```text
COUNT PAIR
```

where `COUNT` is the number of surviving rows that held `PAIR` alongside at
least one other pair — the union of that pair's `--combos` rows, not the sum
of their counts, so a row holding three pairs counts once for each of the
three rather than twice. `COUNT` is right aligned to the widest count printed,
each pair is spelled as the `PAIRS` file wrote it, and lines are printed by
descending count, ties broken by that spelling. The state held is one entry
per pair rather than one per combination.

Unlike `--results` and `--combos`, this mode does honor `-i/--ignore` and the
`-y/--yes` classified YES pairs, as `top-segments --pairs` does, since it
emits one pair per line rather than rows or pairs of pairs. They are applied
when printing, not when counting: a suppressed pair still counts as the other
pair a row needs to hold, so hiding it leaves its neighbors' counts unchanged
rather than silently costing them rows.

`--results`, `--combos` and `--pairs` are mutually exclusive.

When no row held two or more pairs, nothing is printed, one line says so on
standard error, and the exit status is 0: an empty intersection is an answer.
