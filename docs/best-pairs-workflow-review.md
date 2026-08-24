# Recommended changes to the BEST PAIRS workflow

This is the actionable version of the review. Apply the mechanical corrections
first, then resolve the four decisions below one at a time. After that, the
workflow document can be rewritten as a sequence of commands that actually run.

## 1. Make the corrections that do not require a decision

- Replace "top 10-15%" and "top 15%" with "P1 YES results whose directional
  probability is in `[0.85, 1.0]`." The probability band is not a percentile.
- Describe `--pairs` as a bonus list, not an allowlist. Removing an entry removes
  its pair bonus, but the entry can still appear and still gets `--word-bonus`.
- Replace "all possible pairs" with "two-word index entries constructible from
  the letter bag and surviving the `-m`, `-x`, and dictionary filters."
- Replace `split -n N` with `split -l 1000000` if the intent is chunks of at
  most one million lines.
- Define the top 1000 as "the 1000 most frequent multi-word segments among the
  retained `-n` DFS results."
- Add setup for the Nutrimatic working directory, `IDX`, `source ./setup.sh`,
  workflow root, sentence, segment count, and output directories.
- Fix the spelling errors and remove the obsolete hand-written note loop.

## 2. Document the complete P1 lifecycle

The draft currently jumps from generating pairs to extracting P1 YES results.
Add the actual lifecycle:

1. Generate the candidate-pair file.
2. Subtract `.wf/p1/done/p1_done.pairs`.
3. Split large remaining input into manageable files if needed.
4. For each file, run `wf submit p1`, `wf eval p1`, run `evalpair`, place its
   JSONL result in the bundle, and run `wf complete p1`.
5. Extract the sentence-wide 85/15 seed list without queueing it:

   ```sh
   NUT=$PWD
   WFROOT=/home/mike/code/words/final

   ../words/wf -d "$WFROOT" extract p1 yes \
     --pairs "$NUT/idx/idx.2.s2.m4" \
     --pm .85 --pr .15 \
     -o "$NUT/results/s2/pairs.s2.idx2.m4.x2.85.15.p1.yes"
   ```

Absolute paths are intentional because the `../words/wf` wrapper changes its
working directory. `wf eval p1` prepares a bundle; it does not run `evalpair`.

## 3. Decision 1: establish the artifact-naming contract — resolved

Naming is the prerequisite for the remaining workflow decisions. Filenames
currently serve three purposes: they identify the contents to a person, carry
enough dimensions to distinguish runs, and let the workflow tool recognize an
artifact's semantic type. We should define that contract before deciding what
P2 accepts.

Recommendation: make each filename a canonical content key, not an ever-growing
history of commands. Put the dimensions that determine or interpret the file's
contents in a fixed order, followed by a semantic classifier and kind where
applicable:

```text
<content-key>.<classifier>.<kind>
```

This matches the current `words/workflow/names.py` model. It does not imply that
every command-line argument belongs in the key. BEST PAIRS still needs an agreed
retention rule before choosing the vocabulary and field order.

Resolution: use canonical content keys. Standalone names should identify and
distinguish their contents without accumulating the entire derivation chain.
Lifecycle and operation history belong to directory placement and the workflow,
not to an ever-growing filename or a required sidecar manifest.

### Next naming decision: what a content key must retain

Recommendation: make the key semantically identifying, not a complete encoding
of the command line. Retain fields used to find, distinguish, or correctly
interpret artifacts in this workflow; omit performance controls and incidental
mechanics. The workflow document records the exact reproducible commands.

Applied to the draft:

- Retain the sentence and artifact kind throughout.
- Retain segment count on DFS-derived and BEST PAIRS artifacts, because BEST
  PAIRS is explicitly segment-count-specific.
- Retain selection boundaries while they distinguish candidate sets: the P1
  probability band, DFS retained-result count, and P2 candidate cutoff.
- Retain scoring choices only when they distinguish experiments. In this
  workflow, omit `wb1` and `pb1`: word bonus 1 is the stable workflow choice and
  pair bonus 1 is the default. Add an explicit field later if either varies.
- Omit execution-only settings such as search-thread count, progress factor,
  cache size, preprocessing threads, projection depth, and verbosity.
- Retain `idx2`, `m`, and `x`: they distinguish the index generation and the
  candidate-entry constraints. Omit the dictionary because it is a stable
  project-wide input for this workflow rather than an experiment dimension.

Resolution: filenames encode semantic identity only. Exact commands carry full
reproducibility. Compact positional fields are acceptable where their position
has one established meaning: use `85.15` for the P1 probability band and
`1000000` for the DFS retained-result count rather than `pm85.pr15.n1000000`.
Keep the index generation as a tagged field, normally `idx2` going forward.

The current canonical DFS example is therefore:

```text
dfs.s2.idx2.m4.x2.g4.85.15.1000000
```

Once this choice is made, define the grammar and representative names for every
artifact in the recipe before changing either repository's commands.

## 4. Decision 2: name and support the P2-candidate artifact — resolved

The output of `top-segments` cannot honestly be named `*.p1.yes`: some segments
may not have come from the P1 YES seed list. But `wf eval p2` currently accepts
only that name shape.

The current transitions are:

- `wf submit p1 FILE` appends `.pairs` to the queued copy if needed.
- `wf complete p1` creates `*.p1.yes` and moves it unchanged into `p2/queued`.
- `wf filter` likewise creates `*.p1.yes` directly in `p2/queued`.
- Explicit `wf submit p2 FILE` currently appends `.yes` if needed, but `wf eval
  p2` accepts only `*.p1.yes`. Thus a submitted `foo.yes` cannot actually enter
  evaluation.

Resolution: preserve automatically advanced `*.p1.yes` names unchanged. For any
other explicitly submitted P2 input, append `.pairs` to the workflow-managed
queued copy unless it already has that suffix, consistently with `wf submit p1`.
`wf eval p2` should accept both canonical queue input types. No `.pairs` suffix
is appended to a `*.p1.yes` artifact.

For an unclassified P2 input, the input name before one terminal `.pairs` suffix
is the bundle name. Submission is idempotent with respect to the suffix:

```text
NAME        -> p2/queued/NAME.pairs -> bundle NAME
NAME.pairs  -> p2/queued/NAME.pairs -> bundle NAME
```

This contract also applies to a future workflow primitive that produces a
queue-ready `.pairs` file; submission must never create `.pairs.pairs`.

This does not require manually redirected `top-segments` output to end in
`.pairs`. Today its hand-crafted name can continue to end in the `head -n`
cutoff, such as `.1000`; `wf submit p2` turns the queued copy into `.1000.pairs`.
A future top-segments workflow primitive should own both selection and output
naming and can render the queue-ready name directly.

This requires canonical unclassified-pairs support in P2, at the cost of a small
change in the `words` repository. It avoids false `*.p1.yes` provenance and the
brittle manual note loop.

## 5. Decision 3: treat unchecked P2 entries as soft NO — resolved for now

Current scope: checked means confirmed YES; unchecked means soft NO. A true
three-state control remains a possible future improvement, but Evernote does not
provide a native tri-state checkbox and the two-checkbox experiment is deferred.

Keep the existing done-set behavior. P2 completion merges the whole evaluated
batch, including unchecked entries, into `p2_done.pairs`. Later P2 evaluations
filter that set by default.

For BEST PAIRS top-1000 reviews, recommend `wf eval p2 --no-filter`, but do not
require it. The batches are small enough to reconsider soft NOs, and review
quality matters more than avoiding every repeated item. Be explicit that
`--no-filter` restores all previously completed pairs present in the submitted
file, including prior confirmed YES entries as well as soft NOs.

Soft NO does not imply P1 auto-NO provenance. Completion therefore need not
extract unchecked entries into a `.p2.no` artifact or advance them to P3. If an
unchecked list is useful later, expose it through an on-demand extraction flow
rather than making it a completion side effect.

## 6. Decision 4: exclude dominating hard-NO segments — resolved in principle

The draft proposes removing frequent bad segments from the P1 seed file and
rerunning DFS. The intended effect is literal exclusion: a bad segment that
dominates the top million anagrams makes the frequency sample itself unreliable,
so no retained candidate should contain that segment.

Current `--pairs` behavior cannot provide this. It is a bonus-membership list;
removing a row only removes the pair bonus. The index entry can still appear and
still receives `--word-bonus 1`.

Required direction:

1. Remove the segment from the positive `--pairs` list.
2. Register it separately as a workflow-classified hard NO, distinct from the
   unchecked soft NOs produced by ordinary P2 review.
3. Supply the hard-NO set to DFS through an exact exclusion mechanism so matching
   index entries are not admitted to any candidate result.
4. Rerun DFS and top-segment collection until the retained million results no
   longer have a known-bad segment consuming a substantial share of the sample.

The Nutrimatic addition is a shared `--exclude-pairs FILE|WORKFLOW-DIR` option
using the same two-word, comma-separated representation as `--pairs`:

- `dfs-anagrams` omits matching index entries during phase-1 extraction, so no
  returned anagram can contain an excluded pair.
- `query-index --score` rejects an exact sequence containing a matching entry
  with a clear diagnostic and nonzero status. It should not print a numeric zero:
  the corpus score still exists, but workflow policy has declared the sequence
  inadmissible.

Path resolution is:

```text
regular file  -> load that file directly
directory     -> load DIR/.wf/classified/no/no.pairs
```

A supplied directory must contain a `.wf` subdirectory; otherwise fail with a
diagnostic that identifies the `--exclude-pairs` directory and the missing
workflow metadata. If `.wf` exists but the resolved
`classified/no/no.pairs` file does not, fail with a missing-file diagnostic;
explicitly supplying `--exclude-pairs` never means an empty exclusion set.
Passing the workflow root is therefore the normal recipe:

```sh
--exclude-pairs "$WFROOT"
```

The resolved workflow-side interface is:

```text
wf classify no FILE
.wf/classified/no/no.pairs
```

The command normalizes and unions explicit hard-NO pairs into the aggregate;
the refinement DFS run would pass the workflow root to `--exclude-pairs`, which
resolves that aggregate. This uses
the existing but currently unused `classified/no` layout and does not treat
unchecked P2 soft NOs as hard classifications.

In this workflow `-x 2` keeps candidate segments to at most two words; a broader
future segment exclusion format would need to account for more than two words.

## 7. Concrete names and parameters — resolved

Use the following concrete names and field order. Treat DFS `-n` and the
top-segment cutoff as empirical parameters, not fixed truths.

```text
pairs.s2.idx2.m4.x2.85.15.p1.yes
dfs.s2.idx2.m4.x2.g4.85.15.1000000
top.s2.m4.g4.1000.pairs
top.s2.m4.g4.1000.p2.yes
dfs.s2.idx2.m4.x2.g4.top1000.1000000
```

The first name is the P1 seed. `pairs` identifies its contents, while `85.15`
is the P1 probability band and `.p1.yes` preserves its workflow provenance and
kind. The second is the provisional DFS output scored with that seed.

The final name is the DFS output scored with confirmed BEST PAIRS. `top1000`
identifies the pair-bonus set as the confirmed output derived from the
top-1000 candidate review; it distinguishes this output from the provisional
DFS run without copying that run's full derivation chain.

For P2 manual review, the resolved bundle name deliberately drops most upstream
DFS context:

```text
top.s2.m4.g4.1000             # P2 bundle name
top.s2.m4.g4.1000.pairs       # queued P2 input artifact
p2/eval/top.s2.m4.g4.1000/    # in-flight bundle directory
top.s2.m4.g4.1000.p2.yes      # confirmed BEST PAIRS artifact
```

The bundle name identifies the review batch by sentence, minimum word length,
DFS segment count, and candidate cutoff. The source DFS filename and the
documented command retain the probability band, retained-result count, index
generation, and other generation context.

### Archive collisions

The compact name can intentionally collide when a later run produces a different
top-1000 set for the same sentence, `m`, and `g`. Preserving both historical
inputs is not currently a requirement; an iteration suffix can remain a future
escape hatch rather than a mandatory field.

Current P1 behavior is late failure: `wf submit p1` checks only `p1/queued`,
`wf eval p1` checks only the in-flight bundle, and `wf complete p1` eventually
refuses to overwrite the existing `p1/done/in` file unless completion uses
`-f`. By contrast, `wf filter` currently checks P2 queued, in-flight, and
`p2/done/in` before publishing.

Resolution: keep archived-state overwrite authority at `complete`, the command
that performs the replacement. Do not require `-f` at evaluation and then again
at completion; the flag is not preserved across commands. Improve `complete` by
preflighting all of its archive destinations before retrieval, extraction,
done-set merging, or other mutations, then fail immediately on a collision
unless that single `complete` invocation uses `-f`. This policy may require
removing or relaxing `wf filter`'s current `done/in` precheck so it does not
reintroduce the same two-flag problem.

## 8. Rewrite the workflow after the decisions

The final document should be a runnable recipe with this shape:

1. Set up paths and parameters.
2. Generate and complete P1 candidate batches.
3. Extract the sentence-wide P1 probability-band seed list.
4. Run fixed-segment-count DFS with that seed list.
5. Rank the frequent multi-word segments; tune and rerun if needed.
6. Submit the chosen candidate increment to neutral P2 review.
7. Complete P2 and obtain confirmed BEST PAIRS.
8. Run final DFS using confirmed BEST PAIRS as the bonus list.
9. Repeat for other segment counts; completed-P2 filtering handles overlap.

## Current-tool facts behind these recommendations

- `wf extract p1 yes` is the current non-queueing P1 extraction command.
- `wf eval p2` accepts only `*.p1.yes`, while `wf submit p2` accepts the broader
  `*.yes`; this is the neutral-input gap.
- `wf eval p2` subtracts `p2_done.pairs`, so prior-review filtering exists.
- `wf complete p2` discovers note chunks and archives confirmed pairs; the
  manual three-note loop should not duplicate it.
- `--pairs` and `--word-bonus` are independent bonuses.
- `top-segments --pairs` ranks multi-word segments by frequency.

The behavioral review was checked against the `../words` CLI at commit
`687b8ab`. Its content-key naming architecture was rechecked at commit `cee1f56`.
No tests were run, and no implementation files were changed for this review.
