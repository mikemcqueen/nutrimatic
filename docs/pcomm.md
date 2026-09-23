# `pcomm`

`pcomm` is like `comm` for `word,word` pair files. `a,b` and `b,a` count as the
same pair. The inputs do not need to be sorted.

```text
pcomm [--tabs] [-123] FILE1 FILE2
```

Column 1 holds pairs found only in FILE1, column 2 pairs found only in FILE2,
and column 3 pairs found in both. `-1`, `-2`, and `-3` suppress those columns,
as in `comm`. Output is left justified unless `--tabs` is given. Either input
may be `-` for standard input, but not both.

## Output

The output does not depend on which input is streamed, on argument order
beyond which file is FILE1, or on the flags. The flags choose which columns
are printed, not which rows or their order. So `pcomm -1 A B` prints exactly
the column 2 and 3 rows of `pcomm A B`, in the same order.

- **Shared pairs.** A pair found in both files is printed once in column 3,
  with its first spelling in FILE1.
- **Repeats.** All other lines are printed as given, repeats included, as
  `comm` does. That includes a file holding both `a,b` and `b,a`. To remove
  repeats, dedupe the input first.
- **Order.** FILE1's rows (columns 1 and 3) come first in FILE1 order. FILE2-only
  rows follow in FILE2 order.

## Which input is held

- **Standard input** is always streamed.
- **Otherwise** the smaller file is held in memory and the larger one is
  streamed.
- **Pipes and `<(...)`.** If either input is not a regular file, its size is
  unknown, so FILE1 is held.

## How the order is kept

- **FILE1 streamed.** One pass. Its rows are printed as they are read, then the
  held FILE2's unmatched rows follow.
- **FILE2 streamed.** The first pass marks matched pairs and records one bit per
  FILE2 line: whether it is FILE2-only. Then the held FILE1 rows are printed,
  and a second pass over FILE2 prints the marked lines. The second pass does no
  lookups.
- **FILE2 is standard input or a pipe.** It cannot be read twice, so its
  FILE2-only lines are kept in memory until the end. That costs memory for
  those lines only, not for all of FILE2.

FILE2 is read twice, or its lines kept, only when column 2 is shown together
with column 1 or 3. Otherwise it is read once.

## Memory

- **Held file.** A regular held file is mmapped, and the set points into the
  mapping instead of copying each line. Anything else is read into one buffer.
- **Per line.** Each held line costs one 16-byte row, plus one hash-table node
  for each distinct unordered pair.
- **Lookups.** Hashing ignores word order, so each lookup is a single probe with
  no allocation.

Design notes and timings are in `findings/pcomm-streaming.md`.
