# Plan: new pairs after a change to u:

Background and analysis: `findings/pgui-bag-diff.md`.

## Outcome

Given letters L, a baseline used bag USED, and a new used bag NEW_USED, show
only the pairs that are new: those that fit `L − NEW_USED` and don't fit
`L − USED`. Pairs classified YES can optionally be let back through.

- pfilter does it on the command line with `-l L -u USED -U NEW_USED`, no
  pre-subtracted bags needed.
- pgui does it with a window-level u_new: field, applied to every pfilter
  column, hiding (not dropping) the pairs that aren't new.
- `letter-pair-diff.sh` gains a `-l BAG -u USED -U NEW_USED` form next to
  its current pre-subtracted `OLD-BAG NEW-BAG` form.

## Rule

A pair P is new when it does not fit `L − USED`.

A USED that doesn't fit L, or uses all of it, is an error, as `-u` is
without `-U`: in pfilter and `letter-pair-diff.sh` it stops the run, in pgui
it marks the field (see Validation). In pgui a column's USED also holds
earlier columns' selections, so it can stop fitting from clicks rather than
typing; that is not checked, and the column treats it as nothing fitting
before, so every pair is new.

The baseline check is always a plain fit check. `--exact`/ex and `--cv`/cv
apply to the new bag only.

With keep-yes, a pair classified YES (globally, and for the sentence when
given) counts as new even when it fits the baseline.

## pfilter library (`pfilter-impl.h`, `pfilter-impl.cpp`)

`PfilterOptions` gains:

```cpp
std::optional<LetterBag> old_bag;  // the baseline; none: every pair is new
bool keep_yes = false;             // YES pairs count as new
```

`bag` stays the bag pairs must fit, and with a baseline it is the new one.
The struct comment gains a sentence for each.

`Pfilter` gains:

```cpp
// Whether `row` is new: old_bag isn't given, or `row` doesn't fit it, or
// keep_yes and `row` is classified YES globally or for sentence.
bool is_new(DfsPairRow const& row) const;
```

`keep()` is unchanged and doesn't call `is_new()`. pfilter drops rows that
aren't new. pgui doesn't use `is_new()`; it hides rows with its own check
(see pgui).

`load()` loads the YES sets (global, and the sentence's when given) into a
separate `yes_` vector when `keep_yes` and `old_bag` are set. Its `add` lambda
takes the vector to push onto: the existing calls pass `rejected_`, the
keep-yes calls `yes_`.

The sentence's YES file may be missing, as many sentences have none; for
keep-yes a missing one is an empty set, with no message.
`classified_pair_file()`, `load_classified_pairs()` and
`ClassifiedPairCache::get()` gain `bool missing_ok = false`; with it, an
absent sentence file loads nothing, silently. `add` takes it too and passes
it to `get()`: the keep-yes sentence call passes `true`, every other call
`false`, so `-y -s N` still requires the file.

## pfilter CLI (`pfilter.cpp`)

- `-U, --u-new LETTERS`, repeatable like `-u`, accumulated the same way.
  Requires `-l`. When given, the bag is `L − NEW_USED` and `old_bag` is
  `L − USED` (USED may be empty).
- NEW_USED and USED each go through `make_letter_bag()` as `-u` does today:
  a letter not in L, or nothing left, is an error.
- When USED ⊆ NEW_USED (as letter counts, equal included), warn that no pair
  can be new: `L − NEW_USED` fits inside `L − USED`. With `--keep-yes` the
  warning reads "only YES pairs can be new: the -U bag fits inside the -u
  bag", since YES pairs still print.
- `--keep-yes`: sets `keep_yes`. Requires `-U`; can't be combined with `-y`.
- `keep_yes` counts like `drop_yes` in the two `-s` checks, since it loads
  the YES sets too: `-s` with `--no-nopairs` is allowed with `--keep-yes`
  (`pfilter.cpp:189`, message "-s goes with -y or --keep-yes when given
  --no-nopairs"), and `--keep-yes` without `-s` gets the "--sentence not
  supplied" alert (`pfilter.cpp:268`).
- Rows are printed when `keep(row) && is_new(row)`, with and without `--cv`.
- Help:
  - `-u`: "subtract letters already used from the -l letters; with -U, the
    baseline bag; may be repeated"
  - `-U, --u-new LETTERS`: "print only pairs that fit the -l letters less
    LETTERS but not the -l letters less the -u letters; may be repeated"
  - `--keep-yes`: "with -U, also print pairs classified YES (globally, and
    with -s N for sentence N) that fit the -u bag"
  - usage line: `[-l LETTERS [-u LETTERS]... [-U LETTERS]... [--keep-yes]
    [--exact]]`

## pgui

### Window (`window.cpp`, `app-state.h`)

The row gets, after u:, a u_new: clearable field, then a "new" checkbox and
a "YES" checkbox:

- `AppState` gains `std::string new_used_letters`, `bool new_only`,
  `bool new_keep_yes`, and `unsigned baseline_version`, commented "Goes up
  when which pairs count as old changes without any column's run changing:
  u: while new_only, or new_keep_yes."
- u_new: is stored on Enter with l: and u:. Whichever of u: and u_new: drives
  the runs bumps `generation` when it changes; u: as the baseline bumps
  `baseline_version` instead, and u_new: with "new" unchecked bumps nothing,
  since no run or hide reads it:

  | Change        | "new" unchecked  | "new" checked      |
  |---------------|------------------|--------------------|
  | l:            | `generation`     | `generation`       |
  | u:            | `generation`     | `baseline_version` |
  | u_new:        | nothing (stored) | `generation`       |
  | "new" toggled | `generation`     | `generation`       |
  | "YES" toggled | greyed out       | `baseline_version` |

  When one Enter changes fields in both columns, `generation` covers it.
- Toggling "new" applies the row as Enter does (l:, u:, u_new: stored from
  their fields), then bumps `generation`, so the runs always match the text
  shown.
- "YES" is greyed out unless "new" is checked.
- With "new" checked, u_new: drives the run: every column (all commands, not
  only pfilter) and every status line use u_new: where they used u: before.
  u: becomes the baseline. With "new" unchecked, u_new: is ignored. An empty
  u_new: with "new" checked means "nothing used".

### Validation of u: and u_new:

l: already gets a red border (`letters_valid`, `IM_COL32(255, 96, 96, 255)`,
1px frame border). u: has no such check today; its overflow only shows as
"over:" in column status lines and a pfilter run error.

- `AppState::validate_letters()` also sets `used_valid` and `new_used_valid`:
  whether `bag_letters()` of u: and of u_new: fit `bag_letters(actual_letters)`
  and leave at least one letter. Each that doesn't fit is diagnosed with
  `report("pgui", "u: letters don't fit within l:")` / `"u_new: letters don't
  fit within l:"`, and each that leaves nothing with `"u: uses all of l:"` /
  `"u_new: uses all of l:"`. Both are errors for both fields, by the Rule.
- The check reads only the typed u: and u_new: fields, not earlier columns'
  selections.
- With `letters_error` set, `actual_letters` is empty, so the u: and u_new:
  checks are skipped and both are valid: l:'s own border and message already
  name the fault.
- u: and u_new: get the same red border as l: when invalid. The red-border
  push/pop becomes a small helper in `window.cpp` used by all three.
- Neither blocks runs: as today, pfilter reports the error for the bag it
  can't make, and other commands show "over:".
- When "new" is checked and u: ⊆ u_new:, warn once on apply, as pfilter does,
  with the "only YES pairs" wording when "YES" is checked.

### Column (`column.cpp`, `column.h`)

- `Column::used_letters()` and `output_used_letters()` take
  `bool baseline = false` and pass it up the chain to the letter-source
  column's `output_used_letters()`. The chain head is
  `app_state().used_letters` when `baseline`; otherwise
  `app_state().new_used_letters` when `new_only`, else `used_letters` as now.
  The upstream selections and column u: go into both.
- The baseline remainder is computed next to `remaining_`, when the key or
  `baseline_version` changes: `remaining_letters(actual_letters,
  used_letters(true), ...)`'s `sorted`, kept as `old_remaining_sorted_`.
  `remaining_letters()` already leaves `sorted` empty when the letters don't
  fit, and it is empty when they use all of L. Both mean nothing fits the
  baseline and every pair is new, so no case needs its own handling and
  nothing is printed per update.
- `update_hidden()`: when `new_only`, the made command is pfilter, and
  `old_remaining_sorted_` isn't empty, hides each output pair that fits the
  baseline. No `Pfilter` is built:
  - The fit check is the judged-bad loop's: `LetterCounts` from the
    remainder, decremented per letter of the item. It moves into a small
    helper used by both checks.
  - With "YES" checked, a pair that fits stays shown when it is in the
    global or sentence YES set. The sets come from
    `app_state().classified_pairs` with `cache.get(..., "yes")`, and the row
    is parsed with `parse_pair_row()`, as `review.cpp` does. Only pairs that
    fit the baseline are parsed.
  - The selected sentence's YES file may be missing; the hide passes
    `missing_ok = true` (see pfilter library), so a missing one is an empty
    set, with no message, as in pfilter `--keep-yes`.
  - When either set is still null (an unreadable or malformed file, already
    diagnosed by `get()`), old pairs are hidden as if "YES" were unchecked.
  - The column's output is unchanged, so later columns reading it still see
    every pair.
  - Only pfilter columns hide. Other commands run on u_new: and show their
    full output: `pairs` lists every pair that fits `L − u_new`, old and new.
    The hide doesn't carry into columns that read a pfilter column's output:
    freqsort over a pfilter column with "new" checked shows its old pairs too.
- `update_hidden()` keeps the number of pairs it hid as old in
  `old_hidden_count_`. The status line's existing "N of M" (shown of output,
  `column.cpp:319`) gains ", K old" when it isn't 0, so the old pairs are told
  apart from those judged-bad and cv hide. K counts every pair that fits the
  baseline, also hidden or not by judged-bad or cv, so it changes only with
  u:, u_new: and "YES".
- `update_hidden()` reruns when `baseline_version` changes, tracked like
  `hidden_judged_version_`.
- A changed YES set reaches the hide only when the column reruns, which
  already happens when a Review of pairs is submitted in pgui
  (`classified_version` is in pfilter's key) and on Enter, which reloads
  changed files. No other trigger is added.
- Saving and review already work on the shown pairs, so they follow the hide.

## Scripts

`letter-pair-diff.sh` gains a second form; output is the same in both:

```
letter-pair-diff.sh -s N [--pm P] OLD-BAG NEW-BAG LETTERS...
letter-pair-diff.sh -s N [--pm P] -l BAG -u USED -U NEW_USED LETTERS...
```

- The option loop gains `-l`, `-u`, `-U`. With `-l`, `-u` and `-U` are
  required and every positional argument is LETTERS; without `-l`, `-u` and
  `-U` are a usage error and the positional form is unchanged.
- `pairs()` takes the used letters as a second argument and adds
  `-u "$2"` to its pfilter call. The bags form passes `""`, which pfilter
  treats as no used letters.
- Each pass runs into a variable (`old=$(pairs ...)`, `new=$(pairs ...)`)
  before awk, which reads them through `<(printf '%s\n' ...)`. A failing pass
  then stops the script under `set -e` instead of reaching awk as empty
  input. Both forms get this.
- USED goes through pfilter's `-u` like NEW_USED, so a USED that doesn't fit
  L or uses all of it is an error, as with `pfilter -U`.
- It stays two passes rather than one `-U` pass because the WORDS column
  needs the old pairs' words.
- Usage line and header comment describe both forms.

## Tests (`test-pfilter.sh`)

Smoke cases only:

- `-l L -u USED -U NEW_USED` over a pair that fits both bags and one that fits
  only `L − NEW_USED`: prints only the second.
- `--keep-yes` with a YES pair that fits the baseline: printed.
- `-u x -U xy`: warns, prints nothing.
- `--keep-yes` with `-y`, and `-U` without `-l`: rejected.

## Order

1. pfilter library and CLI, tests.
2. Scripts.
3. pgui window, validation, columns.
