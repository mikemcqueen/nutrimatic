# Choosing Which pcomm Input to Stream

## Goal

pcomm's output should not depend on which input it streams. The flags
choose which columns are printed, not which rows or their order. So
`pcomm -1 A B` prints exactly the column 2 and 3 rows of `pcomm A B`, in the
same order.

## Rule

`main()` picks the streamed input:

1. If either input is `-`, stream standard input.
2. Otherwise, stream the larger file and hold the smaller one in the set.
   If either input is not a regular file (a pipe or `<(...)`), its size is
   unknown, so FILE1 goes in the set.

An earlier rule let a single `-1` or `-2` choose the streamed file. That could
put the larger file in the set, and it changed duplicate handling and row order
depending on the flags. It was removed.

## Output

- A pair found in both files is printed once in column 3, with its first
  spelling in FILE1.
- All other lines are printed as given, repeats included, as `comm` does.
  This includes `a,b` and `b,a` both appearing in one file.
- FILE1's rows (columns 1 and 3) come first in FILE1 order. FILE2-only rows
  follow in FILE2 order.

### Why repeats are kept

The held side can remove repeats for free, but the streamed side cannot. It
would need a second set of every distinct pair it had printed, which can hold
as much as the set the streaming was meant to avoid. Keeping repeats on both
sides costs nothing on the streamed side. If you want repeats removed, dedupe
the input first.

### How the order is kept

- **FILE1 streamed.** One pass. Its rows are printed as they are read, then
  the set's unmatched FILE2 rows follow.
- **FILE2 streamed.** The first pass marks matched pairs and records one bit
  per FILE2 line: whether that line is FILE2-only. Then the held FILE1 rows
  are printed. A second pass maps the regular file again and prints the
  lines whose bit is set. It does no hashing, so it costs about as much as
  splitting the lines. When FILE2 is standard input or a pipe, its FILE2-only
  lines are kept in a buffer during the first pass instead. That costs memory
  only for those lines, not for all of FILE2.

The second pass or buffer is needed only when column 2 is shown together with
column 1 or 3. Otherwise FILE2 is read once.

## Set layout

- **Held text.** A regular held file is mapped, and the set stores
  `string_view`s into the mapping. Anything else is read into a single
  string. No line is copied into its own allocation.
- **Index.** `PairMap` maps each pair's first spelling to a matched flag. It
  has one node per unordered pair.
- **Rows.** `rows` has one entry per held line, repeats included. Each entry
  is 16 bytes: where the line starts, and a pointer to its index node. Both
  spellings of a pair have the same length, so the key's length gives the
  line's length. A matched pair is printed at the row that starts where the
  node's key does, which is its first occurrence.

## Hashing Either Word Order

`PairHash` splits a line at the comma, puts the two words in a fixed order,
and combines their hashes. `PairEqual` checks lengths first, then compares
the whole line, then the swapped words. So `a,b` and `b,a` are one key.

Each lookup is a single probe with no allocation. The earlier code built the
reversed string for every line. For most pair lines that was a heap
allocation. A miss, which every exclusive row is, also needed a second hash
and a second lookup.

## Measurements

Built with `-O2`, output to `/dev/null`, warm page cache, no competing
query-index or dfs-anagrams runs.

| Case | Before | After |
|---|---|---|
| `-23 idx.2.s2.m4 p1_done.pairs` (60 MB vs 584 MB) | 47 s, 3.9 GB | 13 s, 0.9 GB |
| default `idx.2.s2.m4 idx.2.s1.m4` (60 MB vs 183 MB) | 7.1–7.4 s, 500 MB | 6.6 s, 520 MB |
| single pass, same file held both times | 7.8 s | 8.0 s |

- **`-23` row.** The old flag rule held the 584 MB file. The `-23` output is
  byte-identical to the old output.
- **Default-mode memory.** Peak memory counts the held file's mapped pages,
  60 MB here. Heap use is lower than before.
- **Single-pass row.** Hashing both word orders ran at about the same speed
  as building the reversed string and probing twice. It was not faster, as
  had been expected.

## Unchanged

- Passing `-` as both inputs is still an error.
