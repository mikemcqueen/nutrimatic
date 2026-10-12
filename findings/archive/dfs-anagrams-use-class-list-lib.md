# Moving find-anagrams to the dfs_class_list_lib Helpers

## Question

find-anagrams has its own copies of the argument helpers that
`dfs-cli-args.cpp` in `dfs_class_list_lib` also provides. (dfs-anagrams
already gets them from the library.) Would switching find-anagrams to the
library's helpers change its behavior?

## Answer

No, as long as find-anagrams takes only the helpers below and not the rest of
the dfs-anagrams option handling. Its output, diagnostics, exit codes and
search speed all stay the same.

## Side-by-side comparison

| find-anagrams code | Library equivalent | Result |
|---|---|---|
| `clean_letters` | `clean_letters` | Same code, same error messages. |
| `subtract_letters` | `subtract_letters` | Same logic and same sorted leftover letters. Error text is byte-identical. |
| `parse_count` | `parse_count` | Same code, including the `INT_MAX` limit and the message. |
| `parse_count64` | `parse_count64` | Same code, including the `ERANGE` check. |
| The block in `parse_args` that lowers the default `-m` for short bags, then rejects an explicit `-m` that's too big | `finalize_min_word_length` | Same checks and the same error message. |
| `DEFAULT_MIN_WORD_LEN = 4` | `DFS_DEFAULT_MIN_WORD_LEN = 4` | Same value. |

## What must not be adopted

These would change find-anagrams' behavior:

- **`check_bag_length`** would start rejecting bags longer than
  `DFS_MAX_BAG_LETTERS`. find-anagrams has no such limit. Its only limit is
  `AnagramFilter`'s "anagram too long" check.
- **`dfs_diagnostic_letter_bag`** prints the letter-bag line in a different
  format from find-anagrams' `# N letters "…", words of M+, at most K words`
  line.
- **`DFS_COMMON_LONG_OPTIONS` / `dfs_parse_common_option`** would bring in
  dfs-anagrams' option set (`-x`, `-n`, `--dict`, `--wf` and more).
  find-anagrams' `-m`, `-u`, `-p` and `-c` and its usage text stay its own.

## Linking and speed

- The build change is adding `dfs_class_list_lib` to find-anagrams'
  `link_with` and `thread_dep` to its dependencies, the same as the other
  tools use.
- No symbol is defined in both `libdfs-class-list.a` and `libsearch.a`.
  `dfs-cli-args.h` compiles cleanly into find-anagrams with `-Werror`.
- Link-time optimization drops everything unused: the trial binary was
  74,216 bytes against 74,088 for the current one.
- `AnagramFilter`, `SearchDriver` and `IndexReader` are untouched, so
  inlining in the hot path is unaffected.

## How it was checked

A trial find-anagrams was built outside the repo. It removed the local
helpers, included `dfs-cli-args.h`, called `finalize_min_word_length`, and
linked `libdfs-class-list.a`.

- **Argument handling:** 19 cases matched the current binary exactly, both
  output and exit code. They covered bad characters, `-u` removing a letter
  that isn't there or removing everything, `-m` values that are bad,
  negative, too large or don't fit in the bag, the default `-m` being lowered
  for a short bag, `-p 0`, bad or too-large `-p`, an unknown option, an extra
  argument, and a missing index file.
- **Searches on `$IDX`** (`wiki-merged.2.index`):
  - For `listen`, `astronomer`, `dormitory -m 0`, `conversation -u con` and
    `schoolmaster -m 5`, the first 3,000 lines of output were identical.
  - A full `astronomers -m 4` run gave byte-identical output (64,967 lines)
    and the same progress lines.
- **Timing:** about 5 seconds for both on `astronomers -m 4`, and the gap
  between them was within run-to-run noise. No other search processes were
  running.

## One thing that would be lost

find-anagrams' copy of `clean_letters` has a comment explaining why it
rejects spaces left in the bag. A space would be counted into
`AnagramFilter`'s radix but could never be used up, because
`has_transition()` handles `' '` before it looks at `value[]`. So the search
would silently never finish an anagram. The library version rejects spaces
the same way; this note keeps the reason.
