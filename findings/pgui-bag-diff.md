# pgui: showing only the pairs a changed u: adds

## Question

> ok just exploring an idea at this point. in pgui i specify a pairs file, a
> letter bag, and a "used" letter bag to be subtracted from the letter bag.
> and typically i use a lot of pfilter columns to show pairs. at some point i
> may exhaust the pairs that can be generated from the combined l: - u: bag,
> and i'll want to change the u: bag, to, typically, contain fewer letters.
> the resulting l: - u: letter bag will contain more letters, and thus more
> pairs. but those resulting pairs in this case will be a strict superset of
> the pairs previously displayed (for pfilter columns). what i'm thinking of,
> is a way to show only the "new" pairs, filtering out all the pairs (perhaps
> with the optionally-selectable exception of classified-YES pairs) that were
> generated with the old letterbag. so, for example, i could imagine adding a
> window-level u_new: field. if it is populated, and the letters are different
> than what is in u:, then we could potentially add additional src: filters,
> such as "seed85m4_diff" that is the pairs generated with u_new: that aren't
> generated using u:. note that the "strict superset" mentioned before will
> not always be true if the u_new letter bag isn't a strict subset of u:, and
> i'd want to support this case too. so the "strict superset" thing may not
> even really be relevant, that was just an idea i had while i was typing.
> but do your analysis and let me know if you think it is important. i think
> i have all the tools available to achieve this, for example
> letter_bag_diff.sh gives me the numbers on the diff between different bags
> without actually giving me the pairs. and, to add to this, maybe there could
> be a checkbox next to the src: dropdown, something like [YES], if it's
> checked, included all classified YES pairs that intersect the two pair sets,
> even though they wouldn't normally be displaybe because of the "diff" rule.
> let me know if you have any questions about what my intent is or edge cases
> I havne't considered, and let me know what additional tooling is required
> here or any gotchas i may not have considered.

## Findings

Sources read: `source/gui/column.h`, `column.cpp` (`used_letters()`,
`output_used_letters()`), `app-state.h` (`seed_bands`, `seed_view()`),
`pair-filter.h`, `pfilter-impl.h`, `pfilter.cpp`, and `letter-pair-diff.sh`
(assumed to be the script meant by `letter_bag_diff.sh`, which doesn't exist).

### Short answer

The idea works, and there's no need to build two pair lists and subtract one
from the other. Letters only reach the pairs at the pfilter step: the seed
views behind src: don't depend on letters. And "fits in a bag" only ever gets
easier as the bag grows. So "new" can be tested one pair at a time:

> show P if P fits `L − u_new` **and** P does **not** fit `L − u_old`

The first half is what pfilter already does. The second half is one more bag
check per pair.

That's why the "strict superset" idea doesn't matter much. The test is
correct whether or not u_new is a subset of u:. When it isn't, the pairs that
fit only the old bag simply aren't shown. Being a subset only tells you there
are no lost pairs. The one case to flag is u_new ⊇ u:: the bag shrinks, so
the "new" list is always empty, and the window should say so.

### Issues and gotchas

1. **A src: entry like `seed85m4_diff` is the wrong place for this.** Seed
   views (`AppState::seed_view()`) are cached by (band, dictionary) and don't
   know about letters. A "new pairs" view depends on l:, u:, u_new:, and for
   later columns on the pairs selected before it. It would break that cache,
   double the src: dropdown (5 bands to 10), and still be wrong for chained
   columns (see issue 2). Better: an option on the pfilter command or the
   column, as a toggle plus the [YES] checkbox.

2. **Chained columns are the real catch.** Column 2 runs with column 1's
   selected pair taken out of the bag (`output_used_letters()`). What's new is
   the whole combination of pairs, not each pair alone. If column 1's
   selection is new, everything in column 2 is new too. If column 2 reads
   column 1's output and column 1 has already dropped the old pairs, column 2
   loses "new selection + old pair" combinations, which are new. So:
   - The test for column k should be `chain_used + u_old + P ⊄ L`, where
     `chain_used` is everything selected before this column plus the column's
     own u:. Writing it as one combined check also avoids subtracting letters
     that aren't there when `L − u_old − sel` would go negative.
   - The new-only filter should **hide** pairs from the list rather than drop
     them from `output()`, the way the judged-bad filter and the cv range
     (`display_hides()`) already hide them. Then later columns still read the
     full list. Save and review already work on the shown pairs, so they'd
     follow.

3. **One of the two bags has to drive pfilter.** If u: keeps the old letters
   and u_new: is a separate field, every column must quietly run with u_new:
   whenever it's set; otherwise the new pairs are never generated and the
   list is always empty. Possibly cleaner: edit u: as now, and a separate
   field ("was:" or "u_old:") holds the previous letters to compare against.
   Either works; it affects how the status line reads. Avoid saving the old
   u: automatically, because fiddling with u: would keep resetting what's
   being compared against.

4. **Which "YES"?** Every seed85 pair already has a phase 1 YES score of at
   least 0.85. [YES] presumably means pairs classified YES in the classified
   files, globally or for this sentence: the same lookup pfilter's
   `-y`/`drop_yes` uses, already loaded in pgui as
   `app_state().classified_pairs`. Needs confirming.

5. **"ex" (exact) mode.** A pair that uses all of the new bag can't use all of
   the old one unless the bags are equal, so with ex every pair counts as new.
   The old-bag check should always be a plain fit check, never exact.

6. **Old letters that don't fit in l:.** If u_old has letters l: doesn't have,
   the old bag isn't valid. Treating that as "nothing fit before" (everything
   is new) is reasonable, but it should be a deliberate choice, maybe with a
   warning.

7. **"Previously displayed" vs. "could have been displayed".** The bag test
   answers the second question. In column 2, a pair actually seen earlier
   under a different column-1 selection can still show as new. Getting
   "actually seen" exactly would mean keeping a persistent file of seen
   pairs. That's possible, but heavier and depends on history. The bag test
   is the recommendation.

### Tooling needed

1. **pfilter:** an option like `--not-fit BAG` (a `std::optional<LetterBag>`
   in `PfilterOptions`) that drops pairs fitting BAG, plus tests in
   `test-pfilter.sh`. Then `seed-pairs … | pfilter -l NEW --not-fit OLD`
   gives the actual new pairs from the command line, and
   `letter-pair-diff.sh` could become a single pass instead of two runs
   compared in awk.
2. **pgui window:** the comparison field (u_new: or u_old:), compared against
   u: so it does nothing when the two match, and a warning when the bag
   shrinks.
3. **pgui column:** a new-only toggle and the [YES] checkbox. The hide check
   combines this column's used letters (with u: swapped for the old letters)
   with P and tests against L. Hidden YES pairs are let back through when
   [YES] is checked.
4. **Status line:** probably "N new / M total", to show how much the change
   in u: opened up.

### Open questions

1. Does "new" in chained columns mean the combination is new (issue 2)?
2. Which field holds the new u: and which the baseline (issue 3)?
3. Does [YES] mean classified YES, not phase 1 YES (issue 4)?

## Decisions

Built as described in `plans/pgui-bag-diff.md`.

- Open question 1: yes, the combination is what's new. Each column checks
  its chain's baseline used letters plus the pair against L, and hides
  rather than drops, so later columns read every pair.
- Open question 2: u_new: is a separate window field that drives every
  column's run while "new" is checked; u: is the baseline.
- Open question 3: [YES] means classified YES, globally or for the
  sentence, from `app_state().classified_pairs`.
- Tooling item 1: pfilter gets `-U NEW_USED` (with `-l L -u USED`) instead
  of `--not-fit BAG`, so no pre-subtracted bags are needed.
  `letter-pair-diff.sh` gains a `-l -u -U` form but stays two passes: its
  WORDS column needs the old pairs' words, which a single `-U` pass drops.
- Tooling item 4: the column status line's existing "N of M" gains
  ", K old", counting the pairs hidden as old.
