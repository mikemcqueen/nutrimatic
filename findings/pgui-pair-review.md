# Reviewing Pairs in pgui

## Idea

pgui columns often hold good candidate pair sets. Today, recording verdicts on
them means going through `wf review p2`, reviewing a note in Evernote, and
running `wf complete p2`. Review mode records the verdicts from inside pgui:

- Shift+R, pressed while a pfilter column has focus, opens a review of the
  pairs that column shows.
- Each pair gets a checkbox. Checked means YES, unchecked means NO, and every
  row starts unchecked.
- Space toggles the checkbox on the focused row. Selection plays no part.
- SUBMIT records the verdicts for pgui's current sentence. The modal then
  closes, and the column updates to drop the newly classified NO pairs.

Implemented in `source/gui/review.cpp`, opened from `Window::open_review()`.

## Parallel track: `wf classify pairs`

The workflow (`../words/findings/classify-pairs.md`) adds

```
wf classify pairs -s SENTENCE --yes YES-FILE --no NO-FILE
```

It checks both files for conflicts before changing either classified set, so
it records both verdicts or neither. pgui's SUBMIT depends on it. Two calls to
`wf classify yes|no` could leave a review half applied.

## Entering review mode

Shift+R does nothing, but says why on stderr, unless all of these hold:

- The focused column's command is pfilter. Other commands' rows need not be
  pairs, and `wf` refuses the whole submission on a line that isn't
  `word,word`.
- A sentence is selected. Without `-s`, `wf classify pairs` would record
  global verdicts.
- `$WF` is set. `setup.sh` exports it as the path to `../words/wf`.
- The column isn't pending. A rerun in flight, or one waiting to start, would
  replace the rows being reviewed.

## Rows

- The review takes exactly the rows the column shows, after its regex filter
  and judged-bad hiding. No other filtering is applied.
- pfilter has already dropped the global NO pairs and the sentence's NO
  pairs, so no shown row can contradict an existing NO.
- In cv mode each row ends in its ratio, e.g. ` 0.42`. `Column` already cuts
  each line at its first space into the item and a value, so the review
  submits the item, `left,right`, and shows the value beside it as the column
  does.

## Modal

- Use an `ImGui::BeginPopupModal` overlay rather than putting the column's own
  list into a review state. The column stays live: changes upstream call
  `set_items()` and `set_hidden()` on it (`column.cpp`). Reviewing in place
  would need guards on every one of those paths.
- The modal blocks input to other windows, so the column-level `Shortcut`s
  (Left/Right and Shift+R in `ListBox::render()`) stop firing with no extra code.
- Entering review should change as little as possible on screen: the rest
  of the window dims, but the pairs stay where they were. So the modal's
  list sits on the column's list box, and its status line on the column's
  item-count line just above. `ListBox::render()` records its frame's screen
  rect each frame and the modal is placed on it every frame, so it follows
  the list when the window resizes.
- The modal has no title bar, padding, or border, takes the main window's
  background, and its list takes a focused ListBox's colors. It dims the
  rest of the window with black at 38% rather than StyleColorsDark()'s light
  gray, which brightened the window around the modal instead of dimming it.
  Its rows have the ListBox's row height, so each pair lands where it was,
  shifted right by the checkbox, when the ListBox was scrolled to the top.
- The modal's list always opens at its first pair, with keyboard focus
  there, however the ListBox was scrolled or whatever it had selected.

## Freezing the pairs

`SharedLines` is `std::shared_ptr<Lines const>`, and `set_items()` swaps the
pointer rather than changing the lines. Holding the pointer freezes the pairs
with no copy. The column keeps updating underneath without affecting the
review.

## Checkbox rendering

- Keep one focusable item per row: the `Selectable`. A real `ImGui::Checkbox`
  beside it would put two nav targets on each row, and Up/Down would start
  landing on checkboxes.
- Draw the box in the Selectable's rect with `GetWindowDrawList()->AddRect`
  and `ImGui::RenderCheckMark`. That comes from `imgui_internal.h`, which
  `list-box.cpp` already includes.
- Pass the checked state as the Selectable's `selected` argument so YES rows
  are highlighted across the full row. The nav cursor shows focus and the
  highlight shows the checked state.

## Space toggles

- Space is ImGui's activate key, and an activated `Selectable` returns `true`
  just as a clicked one does. In review mode, a `true` return means toggle.
- The review list must not pass `ImGuiSelectableFlags_SelectOnNav`. With it,
  arrow movement also returns `true`, which is why `ListBox::render()` checks
  `NavJustMovedToId`. Without it, only a click, Space or Enter returns `true`.

## Rows already classified YES

pgui always shows YES pairs, so the review list will contain pairs that are
already classified YES, both sentence and global. Left unchecked, one of those
would be submitted as NO, and `wf` refuses a sentence NO that contradicts a
sentence or global YES.

- Show those rows checked and locked. There is no un-classify.
- Leave them out of both files.
- `app_state().classified_pairs` (`ClassifiedPairCache`) already has the YES
  sets.

## SUBMIT and CANCEL

- Both are buttons on a final row inside the list, after the last pair. With
  the `ImGuiListClipper`, that's row `count`, so `clipper.Begin(count + 1)`.
- The only way to submit is to reach the end, which prevents accidental early
  submission. End or the scrollbar still allows a deliberate jump.
- Down from the last pair lands on SUBMIT, and Left/Right moves between the
  two buttons.
- Space or Enter on a button activates it, as with any item.

## Confirmations

- SUBMIT and CANCEL each open a YES/NO confirmation. It's a second
  `BeginPopupModal`, opened from inside the review modal; ImGui supports
  stacked modals.
- Esc in the review opens the CANCEL confirmation. That lets you bail out
  without scrolling to the bottom.
- Esc inside a confirmation means NO: back to the list.
- Focus starts on NO in both confirmations.
- Skip the CANCEL confirmation when nothing is checked.
- The SUBMIT confirmation shows a summary, e.g. "37 YES / 363 NO".
- Claim Esc explicitly with `Shortcut(ImGuiKey_Escape)` at each level rather
  than relying on ImGui's built-in Esc handling for popups, which has changed
  between versions.

## Running wf

- After SUBMIT is confirmed, write the YES and NO pair lists to temp files and
  run `$WF -d "$WFROOT" classify pairs -s SENTENCE --yes YES-FILE --no
  NO-FILE`.
- `$WF` is `../words/wf` itself, not a symlink, so its `dirname "$0"` finds
  the venv. It doesn't change directory, and the temp file paths are
  absolute. `wf` takes its root from `-d` before `$WFROOT`
  (`workflow/wf.py:main`).
- Start it with `posix_spawn`, with no shell, and read stdout and stderr
  through one pipe.
- Run it off the UI thread, like column jobs. `wf` starts a venv Python.
- While it runs, the modal is locked: SUBMIT and CANCEL are greyed out, Esc
  does nothing, checkboxes don't toggle, and the modal shows "submitting…".
- On a non-zero exit, show the output in the modal and keep the checks. `wf`
  writes clear conflict messages.
- `wf` runs `pcomm` from PATH, which `setup.sh` gives `build/`.
- Remove the temp files afterwards.

## Updating columns afterwards

On success only, close the modal and bump a classified version. Don't do
either when SUBMIT is confirmed, because pfilter could rerun before the files
are written. No polling of the classified files.

- `ClassifiedPairCache` (`source/classified.h`) reloads a file when its
  modification time or size changes, but only when `get()` is called. The
  only caller is a pfilter run (`pair-filter.cpp`). Nothing reruns a column
  when the file changes.
- A column reruns when its `Column::Key` changes (`column.h`). Add
  `classified_version` to `AppState`, and include it in the `Key` only for
  commands that read classified pairs, declared by something like
  `Command::uses_classified()`. In practice that's pfilter.
- Bumping `app_state().generation` instead would rerun every column, including
  expensive commands that don't read classified pairs.
- Columns downstream of a pfilter column rerun through the `source_version`
  chain, because `set_items()` bumps `version_`.
- In the reviewed column, the new NO pairs disappear and the YES pairs stay.
  `set_items()` keeps the selection when the selected pair survives.

