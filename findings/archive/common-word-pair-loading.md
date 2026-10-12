# Sharing Word-or-Pair Loading Across Tools

## Question

Can every tool that reads word or `word,word` pair lines share one loading
path?

## Answer

For the word/pair file loaders, yes, and most of it is shared already. What
remains is three copies of the same line loop. Three other readers handle
words or pairs in a different format and shouldn't be forced onto the shared
parser. Not pursued for now.

## Already shared

Every word/pair file loader ends in `parse_pair_row()` → `clean_word()`
(`dfs-cli-args.cpp`), so cleanup and parse errors are the same everywhere:

| Entry point | Users |
|---|---|
| `load_pair_file()`, `load_pair_file_ignoring_single_words()`, extraction, weighted and reject loaders (all via `load_pair_rows_stream()`) | dfs-anagrams, rerank-anagrams, segment-stats, `classified.cpp`, `pair-exclusions` (top-, first-, filter- and common-segments) |
| `read_rows()`, `print_kept_rows()` (`row-input.cpp`) | pfilter, bag-filter |
| `read_lines()` then `parse_pair_row()` directly | bag-subset, which drops lines that can't fit the bag before parsing |

## Duplicated: three copies of the same getline loop

| Loop | Where | What it adds |
|---|---|---|
| `read_lines()` | `row-input.cpp` | nothing; the generic loop |
| `load_pair_rows_stream()` | `dfs-cli-args.cpp` | hyphen policy (skip `-` lines or reject them) and single-word policy (allow, ignore and count, or reject) before parsing |
| `load_dictionary()` | `dfs-cli-args.cpp` | words only; `-` lines skipped silently |

## Proposed consolidation

Rebuild `load_pair_rows_stream()` and `load_dictionary()` on `read_lines()`,
so every word/pair file goes through `read_lines()`, then the caller's
policy, then `parse_pair_row()`. Put the hyphen and single-word policies in a
small options struct shared by `read_rows()` and `load_pair_rows()`.

- **Contained:** everything involved is in `dfs_class_list_lib`.
- **Visible difference:** the open and read failure messages are worded
  differently today (`error: can't open %s "%s"` vs
  `%s: can't open "%s": …`), so one wording would replace the other.

## Not worth folding in

These read words or pairs but not in the shared format. At most they could
use `read_lines()` for the loop itself:

- **pcomm:** memory-maps regular files and compares raw pair text for
  speed, with no cleanup. The shared parser would slow it down and change
  what it compares.
- **query-index score input** (`parse_score_values()`): any number of
  comma-separated words per line, trimmed and checked with
  `is_literal_word()` rather than cleaned.
- **freqsort:** reads a dictionary but needs the original case to spot
  proper nouns, which `clean_word()` would drop.

## Naming

"Row" in this codebase usually means a dfs-anagrams result row
(`segment-rows`). `row-input`, `DfsPairRow` and `parse_pair_row()` also use
it for a word/pair line. `row-input` also holds the generic
`read_input_file()`, which the segment tools use to read result rows. A
split into `input-file` (`read_input_file()`, `read_lines()`) and a
word/pair input file was considered. The name is left alone for now.
