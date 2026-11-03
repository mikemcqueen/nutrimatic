# `top-segments`

## How it works

`top-segments` reads the complete supplied result set before producing any
output. Each nonempty input row must have one of two forms:

```text
score segment[,segment ...]
word,word[,word ...]
```

A row starting with a digit is the first form. A row starting with a letter
and holding a comma is the second, a pairs-file line: it is read as one
segment whose words are its comma-separated fields joined by spaces, so
`cat,dog` counts as the segment `cat dog`. Any other row is an error.

The score is parsed but does not affect the result. Row-level filters are
applied first, followed by per-segment ignores. By default, each remaining
segment occurrence increments its entry in an
`unordered_map<string, SegmentStats>`. Repeating a segment twice on one result
row therefore adds two to its occurrence count. Multiple input files contribute
to the same aggregate counts.

After all input has been read, the eligible entries are partially sorted. In
the ordinary mode, the ordering is:

1. occurrence count, descending;
2. segment text, ascending, to break ties.

`-n N` limits only the number of rows printed; it does not stop input scanning
or counting early. The default limit is 1000, and `-n 0` prints every entry.

In short, the ordinary mode counts every eligible segment occurrence across
the complete supplied result set, then prints the top `N` by descending count
with lexical tie-breaking.

The alternative modes modify this behavior:

- `--by-length` makes non-space character length the primary sort key.
- `--segments N[,N...]` counts only each row's Nth segments, 1-based; a row
  with fewer segments contributes only those it has. Row-level filters still
  look at every segment. The default, 0, counts every segment. The selection
  applies before every other mode below.
- `--solo-words` and `--pairs` select which complete segments are emitted.
- `--all-words` and `--pair-words` project selected segments into individual
  words before output.
- `-u LETTERS` takes the letter bag from the first input row, removes
  `LETTERS`, and prints only entries whose letters fit in what is left. It
  hides rows at print time only; counts and `--elim` columns are unchanged.
- `--elim` also tracks how many distinct surviving result rows contain each
  candidate and sorts primarily by decision impact rather than occurrence
  count. Duplicate occurrences on one row still increase `COUNT`, but increase
  `REJECT` only once.
