# Plan: review a freqsort column's dictionary words in pgui

## Outcome

A freqsort column whose `src:` is "dict" can be reviewed the way a pfilter
column's pairs are, with the same key, the same modal list of checkboxes,
and the same SUBMIT/CANCEL confirmations. Each row is one dictionary word:
checked keeps it, unchecked removes it. Words already reviewed in an
earlier `wf remove words` round are shown checked and locked, since `wf`
would ignore them. Every unlocked word, kept or removed, is recorded as
reviewed, so a kept word comes back locked next time. A confirmed SUBMIT
writes the unchecked words to `pgui.sN.words`, N being the selected
sentence, and every unlocked word to `reviewed.words`, in a fresh `mkdtemp`
directory, and runs

```
$WF -d $WFROOT remove words <tmpdir>/pgui.sN.words --reviewed <tmpdir>/reviewed.words
```

on the worker thread pair review uses, so the round is archived as
`dict/removed/pgui.sN.words.removed.M` and
`dict/done/in/pgui.sN.words.reviewed.M`. On success pgui reloads its
dictionaries and the column reruns without the removed words. On failure
the output is shown and the checks are kept, as for pairs.

`remove words --reviewed` takes the whole reviewed set beside the removals
and archives it as the round's reviewed input, as `complete words` does.

## Opening a review

`Window::open_review` accepts a column when its command is reviewable as
today (pfilter: pairs), or when the command is FreqSort and the column's
source is `Dict` (words). The command trait becomes a review kind rather
than a bool:

```cpp
enum class ReviewKind { none, pairs, words };
```

in its own `review-kind.h`, so the command headers don't pull in `review.h`.
`reviewable` in `command.h` is replaced by
`ReviewKind review_kind(InputSource const& source) const`, defaulting to
`none`, so the command alone decides what its column can review:
`PairFilter` returns `pairs`, `FreqSort` returns `words` when
`std::holds_alternative<Dict>(source)` and `none` otherwise. `open_review`
passes `column.source_` and accepts any kind but `none`. Anything else prints `pgui: only pfilter columns, or freqsort columns on
dict, can be reviewed`.

A sentence is required for both kinds; the existing check stays, with its
message made kind-neutral: `pgui: select a sentence to review`. A word
removal applies to every sentence; the sentence names the round, as the sN
of its archive stem.

## Review

`Review` keeps its layout, confirmations, `Job`, `jthread`, `run_command`
and failure display unchanged. What differs by kind moves behind the kind
it is opened with:

- **Parsing rows.** Pairs parse with `parse_pair_row` as today. Words take
  each row's first whitespace-separated field (freqsort rows are
  `word  score`) and require it to match `[a-z]+`; any other row fails the
  open with `pgui: review: row N: not a word: TEXT`. The parsed word is
  kept per row, so submit sends words, not rows.
- **Locking.** Pairs lock the already-classified YES pairs as today. Words
  lock those in `.wf/dict/done/reviewed.words`, read once at open into a
  set; a missing file locks nothing, and an unreadable one fails the open.
  The path is a new `WORKFLOW_REVIEWED_WORDS_PATH` in `workflow-paths.h`.
  Every other row starts unchecked and editable; starting unchecked, so
  removal is the default, is intended.
- **Wording.** Pairs keep `S%d: %d YES / %d NO` and
  `Record %d YES / %d NO for S%d?`. Words show `S%d: %d keep / %d remove`
  and ask `Record %d keep / %d remove in the dictionary?`, the status line
  counting locked rows as keep and the question leaving them out.
- **Submitting.** Pairs run `classify_pairs` as today. Words collect the
  unlocked words and, of those, the unchecked ones; when every row is
  locked, SUBMIT closes the review as `cancelled` at once, with no
  confirmation and nothing run. Otherwise `remove_words` writes them to
  `reviewed.words` and `pgui.sN.words` in a `mkdtemp` directory, runs the
  command above, and removes the directory.

`Review::open` takes the kind; the `$WF` and `$WFROOT` checks apply to
both. Only pair reviews load `ClassifiedPairCache`; only word reviews read
`reviewed.words`.

## After submit

`Review::Result::submitted` already reaches `Window`. For a pair review it
bumps `classified_version` as today. For a word review it calls
`app_state().refresh_dictionaries()`, which reloads whichever of
`.wf/dict/words.filtered` and `.wf/dict/dict.filtered` `wf` rewrote. The
column's `Key` already includes the D: dictionary's version when the source
is `Dict` (`dictionary_file_version`), so the column reruns with no `Key`
change.

The comments describing `reviewable` in `command.h`, and the class
comments in `review.h` and `window.h`, are rewritten for the two kinds.

## Tests

None beyond building; the review is interactive.
