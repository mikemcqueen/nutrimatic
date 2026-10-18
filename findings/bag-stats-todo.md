# bag-stats: To Do

## pgui ss: review

From the `/code-review medium` pass on the working tree that replaces pgui
bag-stats' "pb"/"wb" toggles with an "ss:" field and counts a last p in
"big_dict". Line numbers refer to that tree.

* PAIRS rows are loaded for every search (`bag-stats.cpp:43`). `run()` parses
  every PAIRS row into `final_bags`/`final_rows` even when the search has no
  pb step and doesn't end in pf. The new default `wb,p` is one of those, so
  every rerun on a seed50m3 input parses millions of rows and copies them
  only to throw them away. The CLI's `load_bag_stats_inputs` loads PAIRS bags
  only for pb steps or a last pf, and keeps the rows only when they're
  listed. Do the same in pgui: guard the loop with `search.pairs > 0 ||
  search.pair_filter`, and push rows only when they'll be listed.

* A bare `p` in ss: gives an empty column with no error
  (`bag-stats.cpp:160`). `apply_fields()` accepts it, but without steps
  `list_bag_stats` goes to `count_remainder` with no word bags. Every
  completion then has empty `word_bags`, so every one is skipped. The result
  mapping would also fall through to `loaded.final_rows`, which only means
  something for pf. The header's "without steps, each PAIRS row that fits"
  is true only of `pf`. Either reject `p` in `apply_fields()` or decide what
  the GUI shows for it.

* Rejected ss: text stays in the field (`bag-stats.cpp:156`). When
  `parse_search` fails or a search has more than one pb or wb step,
  `search_field` keeps the rejected text and the previous search keeps
  running. The only sign is a message on stderr. Reset `search_field` to
  `search_text(search)` on rejection, the way `apply_length` corrects the
  length fields.

* big_dict reloads rerun pf searches (`bag-stats.h:31`).
  `uses_big_dictionary` is always true for BagStats, but `run()` reads
  `big_dictionary` only when the search ends in p. With `pf` or `wb,pf`, a
  reload of words.filtered reruns a search whose output can't change.
  `Command::uses_big_dictionary()` is a static constexpr flag, so either make
  it a per-instance member or accept the reruns.

* ex reruns p searches (`bag-stats.cpp:127`). `options.exact` now matters
  only for a last pf (`FinalPairBags`), but toggling ex still returns
  changed, so `wb,p` reruns everything (PAIRS load, DICT word bags,
  big_dict counter, walk) and gets the same output. Return changed only when
  `search.pair_filter` is set, or disable ex when the search ends in p.

* The `bag-stats.h` comment describes `"pb:"` and `"wb:"` fields, but
  `render_options()` draws `pb` and `wb` without colons.

## From docs/todo

* Route `load_pair_bags()` through `Pfilter` / `PfilterOptions` for every
  mode, so that pf mode (and the rest) get their row filtering from pfilter.
  Future pfilter options (-x, -y, -i) would then reach bag-stats as
  `PfilterOptions` fields.

* pfilter can probably also use a shared pair bag loader (bag-stats'
  `load_pair_bags()`) instead of reading PAIRS rows itself.

* Port the SIMD code from bag-stats to pfilter's `Pfilter::keep()`.

* Record --no-nopairs in the dump header (bag-stats 6), so that
  bag-stats-sort doesn't re-apply no.pairs to a dump made without them.

* Consolidate the sort flags (--wb, --wbwp, --tp, --mp, --wbwp-min,
  --wbfrac-min) into one --sort flag,flag,flag, in bag-stats,
  bag-stats-cuda and bag-stats-sort.
