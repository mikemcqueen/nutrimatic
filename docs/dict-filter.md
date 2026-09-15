# `dict-filter`

## Why this tool exists

At first glance, `sort` plus `comm -23 MYFILE /usr/share/dict/words` should do
the same job. It doesn't fit the solo-words review workflow
(`plans/sketch-dict-filter.md`), for three reasons:

1. **Input lines carry counts.** `top-segments --solo-words` emits lines like
   `123 word`. `comm` compares whole lines, so a count-prefixed line never
   matches a bare dictionary word. `dict-filter` strips the count before
   lookup and prints the original line, count included.

2. **Counts differ between runs.** After a review pass removes words from the
   dictionary, the next `top-segments` run can report different counts for
   the same words. Filtering a new run against already-reviewed
   `good*.solo` / `bad*.solo` files therefore can't be done line-for-line.
   Since the counts are useful during classification, they are kept rather
   than stripped. `-d` accepts count-prefixed files as dictionaries and can be
   repeated to union them, so

   ```text
   dict-filter -v -d good.solo -d bad.solo top2.solo
   ```

   stands in for `comm -23`.

3. **No sorting.** `comm` requires both inputs sorted in the same collation,
   which destroys the ranked-by-count order of `top-segments` output.
   `dict-filter` uses a set lookup, so output preserves input order.

Two smaller conveniences:

- **Dictionary normalization.** Entries are lowercased, stripped of
  non-alphanumerics (`Aaron's` → `aarons`), and hyphenated entries are
  skipped. With `comm` this preprocessing would have to be done by hand.
- **Multi-word lines.** A line matches only if every space-separated word is
  in the dictionary, a holdover from the original `source/dict-filter.py`,
  which filtered segment output.
