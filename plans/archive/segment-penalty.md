# Plan: add `-P` / `--segment-penalty P`

## Goal

Add the same explicit segment-penalty control to `dfs-anagrams` and
`query-index`:

```text
-P P
--segment-penalty P
```

For selected index entries \(w_1,\ldots,w_k\), score them as:

```text
product(count(w[i])) / (corpus_total * P)^(k - 1)
```

Equivalently:

```text
corpus_total * product(probability(w[i])) / P^(k - 1)
```

The first selected entry pays no segment penalty. Each subsequent selected
entry pays exactly one factor of `P` while retaining the existing
`1 / corpus_total` normalization. Spaces inside one indexed phrase do not
create another segment.

## User-visible decisions

- Default `P` to `1,000,000`, preserving current output and pruning behavior
  when the option is omitted.
- Accept finite `P >= 1`. `P=1` removes the explicit boundary penalty while
  preserving score monotonicity; values below 1 could make an appended segment
  improve the accumulated score and invalidate pruning assumptions.
- Keep `P` a floating-point value so experiments are not restricted to
  integer powers of ten.
- `query-index --score` uses `P` for comma-separated entries. Ordinary
  `query-index` ranking scores each result as one segment, so changing `P`
  cannot change that ranking. With `--require-completable`, pass `P` through
  the shared phase-2 search for consistency, although exact feasibility must
  remain score-independent.
- Keep `--word-bonus` independent of the selected `P`. One unit continues to
  multiply a multi-word indexed entry by `1,000,000`, matching current
  behavior:

  ```text
  multi_word_log_bonus = word_bonus * log(1,000,000)
  ```

  In particular, `--word-bonus` must still work at `P=1`. Tying its base to
  `P` would reproduce the problem identified in
  `ideas/rethinking-segment-penalty.md`, where every bonus becomes zero at
  `P=1`.

Changing the default to `P=1`, segment-count buckets, and changes to the
legacy `find-anagrams`/`SearchDriver` score model are out of scope. A default
change can be considered after comparing the explicit settings.

## Implementation

### 1. Express the shared score model in segment-penalty terms

Update `source/dfs-score.{h,cpp}`:

- replace `DFS_RESTART` with
  `DFS_DEFAULT_SEGMENT_PENALTY = 1e6`;
- give the fixed word-bonus base its own named constant (normally the same
  default value), so the two concepts cannot accidentally become coupled
  again;
- change `DfsScoreModel` to accept `segment_penalty`, `corpus_total`, and
  `word_bonus`;
- compute the between-segment log contribution directly as:

  ```text
  -log(segment_penalty) - log(corpus_total)
  ```

  rather than computing a reciprocal restart first;
- keep all accumulation in natural-log space and exponentiate only for
  display; and
- rename restart-oriented fields/accessors to append- or
  segment-boundary-oriented names.

The direct `-log(P)` formulation avoids avoidable reciprocal underflow and
makes the public formula visible in one authoritative place.

Update `source/dfs-search.{h,cpp}` so `DfsAnagramSearch` accepts a segment
penalty and uses the renamed shared boundary contribution everywhere it builds
class scores, score bounds, and DFS transitions. This is a semantic rename,
not an algorithm change: the existing bounds remain valid for every accepted
`P`.

Update DFS-family constructor call sites and tests to pass
`DFS_DEFAULT_SEGMENT_PENALTY` instead of spelling `1e-6`. Do not change the
unrelated restart representation used by `find-anagrams`, `SearchDriver`,
`find-expr`, `find-phone-words`, or the measurement-only `measure-f` tool.

### 2. Add shared validation and both CLI options

Add a small `parse_segment_penalty()` helper to
`source/dfs-cli-args.{h,cpp}`. It should use the existing finite-double parser
and then reject values below 1 with a direct diagnostic such as:

```text
error: --segment-penalty must be at least 1
```

In `source/dfs-anagrams.cpp`:

- add `double segment_penalty` to `Args`, defaulted to
  `DFS_DEFAULT_SEGMENT_PENALTY`;
- add short option `-P` and long option `--segment-penalty`;
- show the option, formula, valid lower bound, and default in `usage()`; and
- pass the parsed value into `DfsAnagramSearch`.

In `source/query-index.cpp`:

- add the same `Args` field, default, short option, long option, validation,
  and usage text;
- allow the option with `--score` rather than marking it score-incompatible;
- pass it to the exact-sequence `DfsScoreModel`;
- pass it to the ordinary single-entry ranking model; and
- pass it into the `DfsAnagramSearch` used by `--require-completable`.

Include `P` in the startup/phase-2 diagnostic lines where practical so saved
benchmark stderr identifies the score model without relying on the shell
history. Update the CLI diagnostic smoke assertions accordingly.

### 3. Preserve `--word-bonus` while decoupling it

Revise both programs' help text to describe the bonus as a fixed
`1,000,000^N` multiplier for each selected multi-word index entry, rather than
as a fraction of the active restart penalty.

Keep the existing class-level policy in `dfs-anagrams`: whether a class gets
the phrase bonus continues to be determined by its best member. Keep
`query-index`'s member-level policy. Resolving that pre-existing distinction is
not part of the segment-penalty switch.

### 4. Add focused smoke coverage

Extend `source/test-query-index.sh` using its synthetic index:

1. Confirm omitted `-P` and explicit `-P 1000000` produce the current score.
2. Confirm a one-entry `--score` result is unchanged at `P=1`, `100`, and
   `1000000`.
3. Confirm a two-entry score is divided by `corpus_total * P` once.
4. Confirm a three-entry score pays `P` twice.
5. Confirm a multi-word exact index entry remains one segment.
6. Confirm `--word-bonus 1` still supplies a million-fold boost at `P=1`.
7. Confirm ordinary one-entry ranking is unchanged by `P`.
8. Confirm `--require-completable` returns the same classes at two materially
   different penalties.
9. Reject `P=0`, a value below 1, a malformed value, and a non-finite value
   with exit status 2 and no result output.

Extend `source/test-dfs-cli.sh`:

1. Compare the default output with explicit `-P 1000000`.
2. Use a known multi-segment synthetic result to verify that `-P 1` changes
   its displayed score by the expected factor without losing the result.
3. Confirm a one-segment result is invariant.
4. Exercise both `-P` and `--segment-penalty`.
5. Add the same invalid-value checks.
6. Retain cached/uncached correctness at one non-default penalty if the
   existing small fixture keeps this cheap.

Update the representative score assertion in
`source/test-dfs-search.cpp` to use the penalty formula and named default.
Constructor-only mechanical changes in other DFS tests should use the named
default constant; do not broaden test coverage beyond the scoring cases above.

## Build and verification

Build according to the repository instructions:

```bash
source ~/code/nutrimatic/.env/bin/activate
conan build .
source build/dep-info/conanbuild.sh
```

Run the minimal Meson smoke set:

```bash
export IDX=~/code/nutrimatic/idx/wiki-merged.5.index
meson test -C build dfs-search dfs-cli query-index-cli --print-errorlogs
```

Also run `dfs-cli-differential` once at the default penalty to confirm that
the compatibility default has not changed the existing relationship with
`find-anagrams`.

Before committing, run `/review` as required by `AGENTS.md`, address any
findings, and rerun only the affected smoke tests.

## Post-implementation experiment

First validate the formula directly with `query-index --score` at
`P = 1`, `100`, `10000`, and `1000000`, including the examples from the design
note:

```text
therapist
the,artist
schoolmaster
the,classroom
the classroom
```

Then compare `dfs-anagrams` on fixed short, medium, and long bags with the same
index, `-n`, cache mode, projection depth, and thread counts at all four
penalties. Use `source ./setup.sh` for `S6` inputs and the required `IDX`
environment variable. Before every accurate timing run, check:

```bash
pgrep -af '(^|/)dfs-anagrams( |$)'
```

Record:

- the full command and penalty;
- retained output and last retained score (the top-N cutoff);
- nodes and solutions visited;
- successful bound transitions and projected-bound work;
- setup and search time; and
- subjective quality/noise in the retained results.

The current stdout joins selected entries with spaces, so it cannot reliably
distinguish an indexed phrase from separately selected entries. Do not infer
segment counts from spaces. If the experiment needs a retained-result
segment-count histogram, add temporary benchmark instrumentation that carries
`class_indexes.size()` through `DfsTopN`; keep that instrumentation out of the
production switch unless a separate user-facing diagnostic is requested.

## Acceptance criteria

- Both CLIs accept `-P P` and `--segment-penalty P`.
- Omitting the option is output-compatible with today's `1e-6` restart
  multiplier.
- For `k` selected entries, exactly `k - 1` factors of `1/P` and
  `1/corpus_total` are applied.
- `P=1` retains corpus normalization and does not disable `--word-bonus`.
- Indexed phrases remain a single segment.
- All accepted settings preserve search/bound correctness; changing cache
  mode or thread count does not change retained output.
- Invalid or pruning-unsafe penalties fail during argument parsing.
- `find-anagrams` and the legacy `SearchDriver` scoring path are unchanged.
