# `select-segment`

`select-segment` analyzes an existing `dfs-anagrams` result set around one
chosen pair. It does not select a workflow item, classify a pair, or modify
workflow state.

## Synopsis

```text
select-segment -s SEGMENT [--top | --first] [-r FILE]... [-i FILE]...
               [-n N] RESULT-FILE
```

`SEGMENT` is accepted as one lowercase word or as two lowercase words
separated by one space or comma. In practice, the selected segment must be a
two-word pair because candidate generation always uses `--pairs`.

## Processing

### 1. Determine the available letters

The basename of `RESULT-FILE` must contain exactly one `sN` component and
exactly one letter-set component:

- `u-LETTERS` means `LETTERS` have already been used. The script reads the
  sentence's letters from `$WFROOT/.wf/best/sN/letters` and subtracts the
  named letters.
- `o-LETTERS` means only `LETTERS` remain, so the script uses them directly.

Letter subtraction treats letters as a multiset, including repeated letters.
The command fails if a `u-LETTERS` bag is not a subset of the sentence's
letters.

`WFROOT` must be set, nonempty, and name a directory.

### 2. Filter the DFS result rows

The script creates a temporary result file by running the equivalent of:

```text
filter-segments --wf [--reject FILE ...] RESULT-FILE
```

This rejects an entire result row if any of its segments:

- is in the workflow's globally classified-NO pairs;
- is in the inferred target's `no.pairs`;
- is in any file supplied with `-r` or `--reject`; or
- contains a word absent from the workflow dictionary.

The workflow target is inferred by `filter-segments` from the result path or
filename.

### 3. Generate candidate pairs

The script runs either `top-segments` or `first-segments` on the filtered
temporary file, always with `--pairs`:

```text
top-segments --pairs [-n N] [--ignore FILE ...] FILTERED-RESULTS
```

`--top` is the default and ranks pairs by their frequency in the surviving
rows. `--first` instead preserves first-encounter order. With no `-n`, the
selector's default limit is 1,000 pairs.

Each `-i` or `--ignore` file prevents its pairs from becoming candidates. It
does not reject rows containing those pairs. Workflow YES pairs are not
ignored automatically.

The requested `--segment` must occur in this generated candidate list. Thus a
pair which exists in the result set but falls below the candidate limit is
reported as not found.

### 4. Print the result-intersection hierarchy

The selected pair becomes the root. The script finds all surviving rows that
contain it as an exact segment and prints their count. It then recursively
considers the other generated candidates.

A candidate can be a child when:

- its letters can be made from the letter multiset remaining after all its
  ancestors have been consumed; and
- it occurs in at least one of the parent node's result rows.

For example:

```text
know kind (443)
  away,team (387)
    snow,white (78)
```

This means:

- 443 surviving rows contain `know kind`;
- 387 rows contain both `know kind` and `away team`; and
- 78 rows contain all three displayed segments.

Every node's count is therefore the intersection of all segments on its path
from the root.

This is not a decision tree. Sibling result sets may overlap, and the same
combination can appear elsewhere in a different order. Counts from sibling
branches must not be added together.

Nodes with zero matching rows are omitted. Candidate order comes from the
selected `top-segments` or `first-segments` ordering.

## Output and temporary files

The hierarchy and diagnostics are written to standard error. The command does
not produce ordinary output on standard output.

Filtering and each recursive intersection use temporary files. The script
removes them during normal completion and while unwinding from failures. It
does not create or update workflow classification files.

The recursion can be expensive: it invokes `grep` and creates a temporary
result file for every explored ordered candidate combination, until a branch
has no matching rows or no letter-compatible children.

## Current edge cases

- Although the argument parser accepts a single-word `--segment`, the
  generated candidates are always pairs. A single-word selection therefore
  normally fails with `selected segment not found`.
- Segment input is limited to lowercase ASCII letters, despite the diagnostic
  using the broader word "alphabetic."
- A space in the selected pair is normalized to a comma for comparison with
  selector output. When searching DFS rows, comma-form pairs are converted
  back to the space-separated representation used inside a result segment.
- Exact result matching uses segment boundaries, so selecting `foo bar` does
  not match a longer or partial segment containing those words.
- With `--top`, `-n 0` means unlimited output. With `--first`, `-n 0` produces
  no candidates.

The module also retains a compatibility-only `display_segment_hierarchy()`
function. The command-line path does not call it; it calls
`display_result_hierarchy()` and reports result-row intersections instead.
