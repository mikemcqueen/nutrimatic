# Restore bag-free exact scoring in `query-index --score`

## Context

`--score` exists to answer one question: what score would `dfs-anagrams` give
this exact sequence of index entries? The original design,
`plans/archive/query-score.md`, was explicit about how:

> Keep the scoring formula in one shared module. `query-index` should look up
> counts and submit score terms; it should not reimplement restart or word-bonus
> arithmetic.
>
> **Do not build `DfsClassList` or run phase 1/phase 2 in this mode.** Exact
> lookup is proportional to the total input text length and requires no letter
> bag, class list, cache, or worker threads.

Commit 7611a68 ("Align query-index scoring with DFS") reached for code reuse and
took the class list along with the arithmetic. It now synthesizes a letter bag
from the concatenated entries, runs a full phase-1 extraction over that bag, and
uses the result only to look up each supplied entry's `(class_index,
member_index)` so it can call `dfs_build_spelling`. The enumeration is pure
overhead: the caller already knows which entries it wants.

Four consequences:

1. **Wasted work.** A full trie walk with `min_word_len = 1` and no word cap — a
   *wider* extraction than `dfs-anagrams` itself normally runs — to score k
   known entries.
2. **Per-line reload in stdin mode.** `prepare_dfs_class_list` sits inside the
   loop (`source/query-index.cpp:722`, loop opens `:695`), so every stdin line
   re-reads the dictionary and every pair file and re-runs phase 1.
3. **A 128-letter cap.** `clean_letters` appends and never clears
   (`source/dfs-cli-args.cpp:227-237`), so `query-index.cpp:776-779` accumulates
   every entry's letters into one bag and `check_bag_length` rejects long
   sequences the arithmetic handles fine.
4. **Rejection inputs gate a calculator.** Score mode loads dictionaries and NO
   files it has no use for, and `collect_workflow_exclude_pair_files`
   (`query-index.cpp:290`) *validates* those paths during argument parsing.
   `push_optional_pair_file` (`dfs-cli-args.cpp:169-183`) stats each one and
   hard-fails on a path that cannot be inspected or is not a regular file, so a
   classified-NO path that is a directory or a dangling symlink kills `--score`
   before a single entry is looked up.

The goal is the original one: `dfs-anagrams` and `query-index` share the scoring
*formula*, and `query-index` submits terms to it without enumerating anything.

## Why no class list is needed

`dfs_build_spelling` computes `representative + Σ(chosen − member0) +
correction` (`source/dfs-output.cpp:129-143`, `:159-178`). The member-0 terms
cancel algebraically, so the value reduces to `Σ member_upper_log_score(count,
multi_word, flags)`, plus `(k−1)` segment boundaries, plus the solo correction.
The class path is a calling convention, not a scoring input. Every term is
reachable directly:

| Term | Direct source |
|---|---|
| `count` | `IndexReader::aggregate_entry_count` (the trailing-space aggregate node) |
| `multi_word` | an interior space in the entry text |
| pair `score_flags` | the `pairs` / `weighted_pairs` maps, keyed on entry text |
| solo edges | `DfsSoloWords::resolve()` — the non-registering twin of `register_profile` |
| boundary penalty | `DfsScoreModel::append_log_score`, needs only `reader.count()` and k |
| descending BEST | `DfsBestBonusPolicy::descending(entry count)` |
| solo correction | `dfs_exact_result_matching`, a pure graph routine |

`resolve()` is required rather than merely preferable: `lookup()` asserts
`frozen`, and the only production `freeze()` is inside `DfsClassList`
construction (`source/dfs-class-list.cpp:584`), so a bag-free path that called
`lookup()` would abort.

## Scoring contract

**Mathematical equivalence with `dfs-anagrams`, at `%#.7g` display parity — not
bitwise identity.**

Phase 3 computes a representative followed by member deltas
(`dfs-output.cpp:159-178`); the bag-free path accumulates the selected members
directly. The two groupings are algebraically equal but can round differently,
so this change does **not** preserve identical floating-point operations.

The existing regression tests check exactly the stated contract:
`test-dfs-cli.sh:878-887` and `:628-649` compare the formatted column as a
string, and `assert_close` in `test-query-index.sh` allows 6e-4 relative error.
Keep both as they are.

Bitwise identity is unreachable without converting phase 3 to the same direct
accumulation, which is ruled out by the decision to leave `DfsTopN::emit` alone.
That is a deliberate trade, not an oversight.

## Decisions taken

- **`--score` scores what it is given.** It no longer refuses entries excluded
  by a dictionary, a NO-pairs file, or `-m`. `--dict`, `-m`, `-x` and `-u` are
  *already* `score_incompatible`, so this filtering only ever arrived through
  workflow mode.
- **Score mode touches no rejection input at all** — no dictionary, no
  exclusions, and no validation of exclusion paths. It reads only positive
  evidence.
- **A missing entry fails in sequence mode.** Any supplied entry absent from the
  index is an error; the synthetic count-1 fallback is dropped from score mode.
  Stdin mode keeps scoring 0 for a pair absent in both orientations.
- **The accumulation loop lives in `query-index`.** It calls the shared
  `DfsScoreModel` primitives and `dfs_exact_result_matching`; no new shared API.
- **Phase 3 stays untouched.** `dfs_build_spelling` and its hot parallel caller
  (`DfsTopN::emit`, run per popped candidate on every search worker thread) are
  not modified. The scoring formula still lives in one place; what query-index
  owns is marshalling.

## Implementation

### 1. Split the bag-free preparation out of phase 1

`source/dfs-class-list-build.{h,cpp}`:

```cpp
bool prepare_dfs_scoring_inputs(
    IndexReader* reader, DfsCommonArgs const& args, bool score_mode,
    size_t exact_segments, DfsPreparedClassList* out);
```

It loads the positive pair inputs only — legacy `--pairs` and the weighted
tiers — then builds `model` and `solo_words`. No dictionary, no exclusions, no
`exclude_pair_files` parameter. With `score_mode`, both pair loads take the
symmetric, minimum-independent path; the legacy `--pairs` load needs that branch
too (`load_pair_file` rather than `load_extraction_pair_file`), not just
`load_weighted_pair_files`.

Neither rejection input has a consumer once the class list is gone:
`exclude_pairs` is read only by `warn_dictionary_drops`
(`dfs-class-list-build.cpp:120`) and `DfsClassList`
(`dfs-class-list.cpp:296,346-348`), and the dictionary only by `DfsClassList`'s
`dictionary_filter`. `warn_dictionary_drops` also takes `letters`, which score
mode no longer has.

`prepare_dfs_class_list` becomes the wrapper, keeping today's sequence:

```
load_dictionary
prepare_dfs_scoring_inputs(..., /*score_mode=*/false, ...)
load_exclude_pair_files
admit_best_words / warn_dictionary_drops
DfsClassList + phase-1 diagnostics
```

Add `external_rows` / `best_rows` to `DfsPreparedClassList`. They are currently
function locals (`dfs-class-list-build.cpp:98-99`) consumed by
`admit_best_words` and `warn_dictionary_drops`, so the split cannot compile
without promoting them. `classes` stays null in score mode.

The only reordering is that `model` and `solo_words` are built before the
exclusion load rather than after. That is unobservable: `DfsScoreModel` and the
`DfsSoloWords` constructor emit nothing, and every phase-1 diagnostic sits at
the tail of the wrapper (`dfs-class-list-build.cpp:147-158`). `admit_best_words`
still mutates the dictionary before `DfsClassList` reads it, and neither the
model nor the solo-word context reads the dictionary.

`dfs-anagrams`, `rerank-anagrams` and ordinary `query-index` listing keep
calling `prepare_dfs_class_list` and are unchanged.

### 2. Rebuild the `--score` path

`source/query-index.cpp` — the pre-7611a68 `print_sequence_score` is the proven
template; recover it with `git show 7611a68^:source/query-index.cpp`.

- Guard `collect_workflow_exclude_pair_files` (`:290`) with `!out->score`, so
  score mode neither collects nor validates exclusion paths, and stop threading
  `args.exclude_pair_files` through either score path. `out->score` is already
  set by the option loop (`:248-251`), well before `:290`.
- Delete the letter-bag synthesis and `check_bag_length` from both score paths;
  `validate_literal_entry` already enforces the character set, and removing the
  bag removes the 128-letter cap.
- Delete `prepare_dfs_class_list`, `dfs_index_members`, and the class/member
  lookup in `build_sequence_score`. Both helpers keep their `rerank-anagrams`
  callers (`rerank-anagrams.cpp:296,332`), so nothing is orphaned.
- Gather terms per entry: `aggregate_entry_count` (**error if absent — no
  synthetic fallback**), `find(' ')` for `multi_word`, `weighted_pairs` then
  `pairs` for flags, and `dfs_solo_score_flags(solo_words->resolve(...))` for
  single-word entries.
- Accumulate with `member_upper_log_score` + `append_log_score`, add the
  `dfs_exact_result_matching` correction, print via `model->displayed_score`.
- Keep `exact_segments = score_entries.size()` so descending BEST is unchanged.

The solo flags are load-bearing and easy to re-break silently:
`dfs_exact_result_matching`'s correction is *relative* to
`solo_local_upper_log_bonus`, which `member_upper_log_score` has already added.
Omitting the flags makes the correction subtract a term that was never added.

Two error messages survive: `index has no entry "%s"` and stdin's `index has
neither entry "%s" nor "%s"`. `duplicate phase-1 spelling` and `dfs-anagrams
phase 1 excludes entry` both go away with phase 1.

### 3. Fix stdin streaming

`score_stdin_values` calls `prepare_dfs_scoring_inputs` **once**, before the
loop, with `exact_segments = 1`. Each line then only gathers terms and scores.

The existing both-orientations index probe stays and becomes the natural
missing-entry guard: absent in both → score 0. Orientation choice is the one
with the higher `member_upper_log_score`, ties to written order — now
*necessary* rather than merely equivalent to today's "lower `member_index`",
since no `member_index` exists to compare. Use `model->displayed_score` here
too, replacing the raw `exp()` at `:740`, so all three score printers agree by
construction.

### 4. Help text

In `usage()`, scope the phase-1 claims to listing mode: the `--pairs` lines
*"eligible listed pairs absent from the index are admitted with corpus count 1"*
and *"dictionary, bag, -m, -x, and workflow NO rules still apply"*, and the
`--wfroot` line about excluding `no.pairs`, must no longer read as covering
`--score`. Extend the `--score` sentence to say it scores the sequence as given,
applies no dictionary or exclusion filtering, and requires every entry to be in
the index. Two further lines mention `--score` and need re-checking: the
descending-BEST-exponent line (`:110-116`) and the solo-word "once per row or
`--score` sequence" line (`:136`).

### 5. Rationale note

Add `findings/query-index-score-needs-no-phase-1.md` recording the member-0
cancellation, the solo-flag/correction coupling above, and why `--score` must
use the aggregate trailing-space count rather than the exact residual
(`exact_entry_count` has no production caller; phase 1 emits `Choice::count` at
`dfs-class-list.cpp:447`). Keep the `.cpp` bodies bare.

## Files

- `source/dfs-class-list-build.{h,cpp}` — the split, plus the two new row fields
- `source/query-index.cpp` — both score paths, the exclusion guard, help text
- `source/dfs-cli-args.{h,cpp}` — no signature change; `score_mode` now reaches
  `load_weighted_pair_files`, whose symmetric branch is dead today (its sole
  call site hardcodes `false` at `dfs-class-list-build.cpp:106`) even though the
  help text already promises it
- `source/test-query-index.sh` — two flipped tests plus two smoke tests
- `findings/query-index-score-needs-no-phase-1.md` — new

## Behavior changes

- A workflow NO exclusion no longer blocks `--score`.
  `test-query-index.sh:578-583` asserts the old behavior and **flips**: the
  sequence now prints a score. The adjacent assertion that ordinary listing
  still honors the exclusion stays as is. That fixture already writes a real
  `no.pairs`, so extend the flipped case with one line that also makes the NO
  path uninspectable — a directory in place of the file — proving the collection
  guard as well as the contents path. Ordinary listing must still fail on that
  same directory, which is what keeps the guard honest.
- An asserted-but-unindexed entry no longer scores 1 in sequence mode; it fails.
  `test-query-index.sh:420-424` **flips** from `assert_close ... 1` to an
  `expect_score_failure` with `index has no entry "cd ab"`. The adjacent
  `grep -q '^1 cd ab$'` at `:418` covers ordinary *listing*, where phase-1
  synthesis is unchanged, and stays.
- A classified-NO or target-NO file that is unreadable, malformed, or present as
  something other than a regular file no longer fails `--score`. Score mode
  never inspects those paths, let alone parses them. Ordinary listing and
  `dfs-anagrams` are unaffected.
- Sequences over 128 letters now score instead of erroring.
- `--score` pair loading becomes symmetric and minimum-independent, matching the
  documented contract.

Every other scored entry in the suite was audited against the synthetic index —
`ab,cd,uv,wx`, `ab,ba`, `wx,xy`, `2345 1`, `gh ij`, `f`, and all six `--score`
sites in `test-dfs-cli.sh` — and all are indexed. `all.stdout` is generated with
no pair files (`test-dfs-cli.sh:54-55`), so the round-trip loop synthesizes
nothing. The two above are the only tests that change.

## Verification

```bash
source ./setup.sh
source ~/code/nutrimatic/.env/bin/activate
conan build .
source build/dep-info/conanbuild.sh
meson test -C build query-index-cli dfs-cli rerank-anagrams dfs-search dfs-output \
  --print-errorlogs
```

`conan build .` does not run tests; `meson test` is its own step.

The parity check is `test-dfs-cli.sh:878-887`, which pastes every
`dfs-anagrams` result line back through `--score` and requires a matching score
column. It must pass **unchanged**, as the regression test for the scoring
contract above. The solo round-trips at `:792`, `:812`, `:827` and `:844` cover
the `resolve()`-for-`lookup()` substitution, and `:628-649` covers descending
BEST across four cumulative exponents.

If a formatted score ever does shift, the cause is the regrouping described in
the scoring contract landing on a rounding boundary, not a scoring bug —
diagnose it by comparing log-space values before touching the formula.

Tests to add (smoke only, per CLAUDE.md):

1. A sequence exceeding 128 letters scores instead of failing.
2. In stdin mode with a `--pairs` file, the loader's "read N pairs" diagnostic
   appears exactly once across many input lines — proving files load once, not
   per line.

Then confirm end-to-end against the real index, checking for competing runs
first since this touches timing:

```bash
pgrep -x query-index; pgrep -x dfs-anagrams
dfs-anagrams -i "$IDX" <letters> -n 5 -g 2 | while read -r s e; do
  [[ "$(query-index -i "$IDX" "$e" --score | awk '{print $1}')" == "$s" ]] ||
    echo "MISMATCH $e"
done
time query-index -i "$IDX" '<long sequence>' --score
```

The last command should drop from a full trie walk to effectively instant.

## Why not the codex variant

`plans/query-index-score-codex.md` reaches the same diagnosis but is rejected on
the two points that matter. It converts phase 3 to direct accumulation,
rewriting `DfsTopN::emit` for a cold caller's benefit; and it keeps dictionary
and workflow-NO rejection in score mode ("dictionary and workflow-NO exclusions
still reject requested entries"), which is the opposite of the intent here.

Three of its contributions are folded in above: the synthetic count-1 rule this
plan originally omitted (resolved in the other direction — sequence mode now
fails instead), its insistence that every input load at most once per
invocation, and its honesty that the equivalence is mathematical rather than
bitwise.
