# Plan: always preserve the P1 seed-pair signal

## Status

Design only. No implementation in either repository has been authorized or
started.

## Outcome

The P1 `.85` seed remains the baseline pair-bonus source for every BEST search.
`dfs.best` refines that baseline instead of replacing it with the much smaller
confirmed-YES set.

The recommended scoring policy is:

| Search | Pair source | Effective pair bonus |
|---|---|---:|
| `dfs.seed` | sentence P1 `.85` seed | `1.00` |
| `dfs.best` | sentence P1 `.85` seed | `1.00` |
| `dfs.best` | global `classified/yes` | `1.05` |
| `dfs.best` | target-local `best.pairs` | `1.10` |

For a pair present in more than one positive source, use the highest applicable
bonus. Do not add the source bonuses together. Global classified NO and
target-local NO remain exclusions and win over every positive source.

The numbers are initial calibration values, not truths encoded by the P1
classifier. The `.85` in the seed name is the classifier's inclusion threshold;
it is unrelated to the `dfs-anagrams` pair-bonus magnitude.

## Why this is needed

The current searches are asymmetric:

- `dfs.seed` passes the sentence's seed file directly to `--pairs`.
- `dfs.best` constructs its `--pairs` set from global `classified/yes` plus an
  optional target `best.pairs`, subtracts global and target-local NO, and then
  filters to pairs spellable by the target bag.
- Consequently, a P1 seed pair loses its pair bonus in `dfs.best` unless it has
  separately reached `classified/yes` or was placed in `best.pairs`.

That contradicts the intended objective. Passing the `.85` classifier was the
reason the pair became a search seed; later human evidence should refine that
signal, not erase it.

An equal-weight union fixes the lost-seed defect but makes the refinement weak.
The positive sets overlap heavily. In the live workflow data inspected on
2026-09-12, the three sentence seeds held 98,250, 26,396, and 34,142 pairs,
while global `classified/yes` held 113. All 56 entries across the four concrete
target `best.pairs` files were already in their sentence seed, and 55 were also
in global YES. Equal union membership therefore makes almost every promoted
pair indistinguishable from an ordinary seed pair.

Equal additive bonuses are worse. Nutrimatic applies a pair bonus as
`1,000,000 ^ bonus`. Adding a full `1.0` for every overlapping source gives a
pair another millionfold advantage at each tier. The proposed `0.05` steps are
deliberately modest:

```text
bonus 1.00  -> baseline seed preference
bonus 1.05  -> about 2x the weight of a seed-only pair
bonus 1.10  -> about 4x seed-only and 2x classified-YES
```

These ratios compare otherwise equal matching segments. Each selected segment
still contributes its corpus count and the existing multi-word bonus, and a
result containing multiple matching segments earns each segment's score.

## Meaning of the three positive sources

### P1 seed

The seed is broad automatic evidence: the pair's directional P1 probability is
in `[0.85, 1.0]`. It is the foundation of both searches and receives the
existing strong `1.00` pair bonus.

### Global classified YES

This is a human-confirmed semantic verdict, normally folded by P2 completion.
It is stronger evidence than the automatic P1 threshold and should win a close
ranking contest without overwhelming corpus evidence. A pair need not have
appeared in this sentence's seed to receive this tier.

### Target-local `best.pairs`

Current P2 completion does not derive `best.pairs`; confirmed pairs go into the
global YES aggregate. `best.pairs` is therefore an optional, hand-maintained
override today.

Giving it the strongest tier is correct only if its contract is explicit:
membership means "promote this pair for this target," not merely "this YES was
discovered while working on this target." If target-local provenance is all it
is meant to record, it should receive the classified-YES tier instead of an
additional promotion.

The recommended plan adopts the explicit target-promotion meaning.

## Effective-set semantics

For `dfs.best`, calculate:

```text
positive pairs = seed union classified/yes union optional best.pairs

effective_bonus(pair) = max(
    1.00 when pair is in seed,
    1.05 when pair is in classified/yes,
    1.10 when pair is in best.pairs
)

excluded pairs = classified/no union optional target/no.pairs

weighted search input =
    positive pairs
    minus excluded pairs
    filtered to pairs spellable by the target letter bag
```

Pair orientation continues to be one identity. Loading either `a,b` or `b,a`
must apply the effective maximum to both search orientations. Duplicate and
reversed appearances must not stack bonuses.

`dfs.seed` remains the reproducible P1-only baseline. It uses the seed at
`1.00`, plus the same global and target-local exclusions, but does not apply the
later promotion tiers. This preserves a meaningful distinction between the
reseed and refine lanes:

```text
dfs.seed = automatic baseline
dfs.best = automatic baseline plus human and target-local promotion
```

## Nutrimatic changes

Nutrimatic currently represents pair membership as one bit and applies one
global `--pair-bonus` magnitude. It cannot express the policy above by passing
several files: `--pairs` is not repeatable as a weighted source, and duplicate
entries collapse into a set.

Add a weighted pair-input form while preserving the existing interface:

- Keep `--pairs FILE --pair-bonus N` unchanged for existing callers.
- Add one weighted manifest input, tentatively `--pair-weights FILE`, mutually
  exclusive with `--pairs` and `--pair-bonus`.
- Give the manifest an exact, locale-independent format such as:

  ```text
  1.00<TAB>seed,pair
  1.05<TAB>human,confirmed
  1.10<TAB>target,promotion
  ```

- Require a finite, non-negative bonus and an otherwise valid pair entry on
  every line. Malformed input is fatal and names the file and line.
- Normalize both orientations and retain the maximum bonus for duplicates.
- Keep exclusion parsing strict and unchanged; weighted positive input must not
  broaden any exclusion or rejection file format.

The option name and concrete line spelling should be confirmed during the
Nutrimatic implementation review. The required semantic contract is one
weighted manifest, maximum precedence, and backward-compatible unweighted
`--pairs` behavior.

Thread the effective per-entry bonus through every scoring-dependent path:

- extraction and pair lookup;
- member score ordering inside each anagram class;
- phase-2 upper bounds;
- phase-3 spelling expansion and displayed scores;
- `query-index --score` and ordinary query-index ranking.

Do not add a `double` to every packed member without measuring the memory cost.
`DfsPackedMember` is intentionally 16 bytes. Prefer a compact bonus/tier
identifier with the actual magnitudes owned by the loaded score model, or an
equivalent representation that preserves the packed size and correct ordering.

The important invariant from the existing bonus implementation remains:
member ordering, search bounds, expansion deltas, and displayed scores must all
use the same effective pair bonus. A partial scoring implementation is wrong.

## Words workflow changes

In `/home/mike/code/words/workflow/best/`:

1. Keep `dfs.seed`'s direct seed input and existing exclusions unchanged.
2. Include `target.seed()` as the mandatory base source when building the
   `dfs.best` input.
3. Merge seed, global YES, and optional `best.pairs` by pair identity and retain
   the maximum configured bonus.
4. Apply global and target-local NO before publishing the weighted input.
5. Preserve target-bag filtering so unrelated global pairs are not loaded into
   the DFS process.
6. Pass the weighted manifest to the new Nutrimatic option.
7. Keep the refusal when no positive pair is spellable by the target, but name
   all three possible positive sources in its diagnostic.
8. Update help and dry-run output to describe the baseline seed and the two
   promotion tiers.

The workflow constants should initially own `1.00`, `1.05`, and `1.10`; do not
add operator-facing tuning flags until corpus comparison shows a real need.
Stable defaults keep searches and status reproducible.

## Freshness and receipts

The current `dfs.best.pairs` receipt records only the sorted set of pairs used
by the successful search. That becomes insufficient as soon as weights exist:
moving an already-present pair from `1.00` to `1.05` changes ranking without
changing set membership.

Make the successful-search receipt weight-aware:

- Record the normalized pair identity and effective bonus used by the run.
- Publish the receipt only after the DFS result and `dfs.best` symlink have
  been published, preserving the current interruption-recovery ordering.
- Recompute the same weighted, excluded, bag-filtered manifest in
  `Inputs.usable_pairs` and compare it byte-for-byte with the receipt.
- A changed seed, new YES classification, edited `best.pairs`, retracted or new
  exclusion, or changed tier value must make `dfs.best` stale exactly when it
  changes the effective target-specific manifest.
- Changes to global YES that are unspellable by this target must remain no-ops
  for its freshness.

The receipt may retain the name `dfs.best.pairs` if its weighted line format is
documented and no consumer expects an ordinary pair set. Otherwise introduce a
separate metadata/manifest artifact and leave `dfs.best.pairs` as a plain
human-readable projection. Decide this from a live consumer search before
implementation.

## Delivery phases

### Phase 1: weighted scoring in Nutrimatic

- Implement the weighted positive-input model end to end.
- Preserve legacy `--pairs` behavior byte-for-byte.
- Add focused smoke coverage for three same-letter-class spellings at distinct
  weights, reversed/duplicate maximum precedence, malformed weights, and
  exclusion winning over a positive weight.
- Include query-index in the focused checks because it shares the loader and
  score model.
- Review the complete diff before committing.

Proposed commit subject:

```text
Support weighted pair bonus entries
```

### Phase 2: always seed BEST searches in Words

- Build and pass the target-specific weighted manifest.
- Publish and compare the weighted receipt.
- Update status, dry-run diagnostics, help, and focused BEST workflow tests.
- Verify both `gen dfs.best` and `prepare --source best` use identical weighted
  inputs.
- Verify `dfs.seed` remains the P1-only baseline.
- Review the complete diff before committing.

Proposed commit subject:

```text
Preserve seed bonuses in BEST searches
```

Land the Nutrimatic commit before the Words commit. Do not leave the workflow
calling an option unavailable in the built Nutrimatic checkout.

## Minimal validation

Follow the repository instructions and use the latest Terra model for all test
selection, test changes, and execution.

Nutrimatic:

```bash
source ~/code/nutrimatic/.env/bin/activate
conan build .
source build/dep-info/conanbuild.sh
meson test -C build dfs-cli query-index-cli --print-errorlogs
```

Use the actual focused test names present at implementation time. Add only
smoke cases needed to protect score ordering and CLI behavior.

Words:

```bash
source ./setup.sh
python -m unittest tests.test_workflow_best tests.test_workflow_best_rows
```

Run the smallest applicable BEST test selection first; widen only for failures
or shared-code risk. No timing run is required for the scoring semantics, but
before any corpus comparison involving `dfs-anagrams` or `query-index`, check
both processes in the host process table as required by `AGENTS.md`.

## Calibration check after implementation

Before declaring the weights settled, run a bounded comparison on one existing
target using the same index, dictionary, bag, segment count, and result cutoff:

1. Current seed-only `1.00` scoring.
2. Equal union at `1.00`.
3. Recommended maximum tiers `1.00`, `1.05`, `1.10`.

Compare at least:

- how many retained results contain a seed, YES, or target-best pair;
- how far promoted pairs move in the retained ranking;
- whether a small set of promoted pairs monopolizes the cutoff;
- whether `top-segments` still exposes useful unclassified candidates.

If each tier is too weak or strong, tune the increment while retaining maximum
precedence and the seed baseline. Do not change the P1 `.85` extraction
threshold as a proxy for DFS score calibration.

## Explicitly out of scope

- Re-running P1 or changing its `.85/.15` extraction band.
- Treating `--pairs` as an allowlist.
- Changing global or target-local NO semantics.
- Automatically deriving `best.pairs` from target review provenance.
- Adding workflow CLI flags for bonus tuning before the bounded comparison.
- Changing the single `top.segments` review frontier or coupling DFS generation
  to review completion.
