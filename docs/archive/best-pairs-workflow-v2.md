# BEST PAIRS workflow, version 2

BEST PAIRS are the manually confirmed, semantically meaningful two-word index
entries used to improve anagram ranking for one input sentence and one exact
segment count. They are passed to `dfs-anagrams --pairs` as a bonus list.
`--pairs` is not an allowlist: an entry absent from the file may still appear in
an anagram and still receives `--word-bonus`.

Scores are comparable only among results with the same segment count, so this
workflow produces a separate BEST PAIRS artifact for every relevant segment
count.

The workflow has four main stages:

1. Automatically classify candidate two-word entries in P1 and extract the P1
   YES results whose directional probability is in `[0.85, 1.0]`.
2. Use those entries as pair bonuses in a provisional fixed-segment-count DFS
   run.
3. Manually review the most frequent multi-word segments in the retained DFS
   results. The confirmed entries are BEST PAIRS.
4. Run DFS again with BEST PAIRS as the pair-bonus list.

The candidate population is not "all possible pairs." It is the set of
two-word index entries constructible from the sentence's letter bag and
surviving the `-m`, `-x`, and dictionary filters.

## Tooling prerequisites

This recipe assumes the workflow changes resolved in the review have been
implemented:

- `wf submit p2 FILE` queues an unclassified input as `FILE.pairs`, unless the
  name already ends in `.pairs`; automatically advanced `*.p1.yes` inputs keep
  their names.
- `wf eval p2` accepts both `*.pairs` and `*.p1.yes` queue inputs.
- `wf classify no FILE` normalizes and unions explicit hard-NO pairs into
  `.wf/classified/no/no.pairs`.
- `dfs-anagrams` accepts repeated `--exclude-pairs FILE|WORKFLOW-DIR` options,
  combines their exclusions, and allows at most one workflow directory.
- `complete` preflights all archive destinations and requires `-f` on that
  invocation when an archived artifact would be replaced.

The current workflow without these changes cannot honestly submit a neutral
top-segment list to P2 or exclude a dominating bad segment from DFS. Removing a
bad entry from `--pairs` merely removes its pair bonus.

## Setup

The examples use sentence S2, the letter set `u-thisandthat`, minimum word
length 4, and exactly four DFS segments. Change `SENTENCE`, `LETTER_SET`,
`DFS_LETTERS`, `MIN_WORD_LENGTH`, and `SEGMENTS` for another run.

```sh
cd ~/code/nutrimatic
source ~/code/nutrimatic/.env/bin/activate
source build/dep-info/conanbuild.sh
source ./setup.sh

export IDX=~/code/nutrimatic/idx/wiki-merged.5.index

NUT=$PWD
WORDS=/home/mike/code/words
WF=$WORDS/wf
WFROOT=$WORDS/final
DICT=$NUT/tmp/words.big

SENTENCE=s2
LETTERS=$S2
LETTER_SET=u-thisandthat
DFS_LETTERS=("$LETTERS" -u thisandthat)   # u- form
# DFS_LETTERS=(thisandthat)               # o- form
MIN_WORD_LENGTH=4
MAX_EXTRACT_WORDS=2
SEGMENTS=4

P1_MIN=.85
P1_RANGE=.15
DFS_RESULTS=1000000
TOP_COUNT=1000

RESULT_DIR=$NUT/results/$SENTENCE
mkdir -p "$RESULT_DIR"
```

`DFS_RESULTS` and `TOP_COUNT` are empirical parameters, not fixed truths. A
longer sentence or a sample dominated by a few common entries may require more
retained DFS results or another candidate increment.

### The letter set

A full sentence has too many letters for `dfs-anagrams` and `top-segments` to
produce a useful candidate sample, so in practice stages 3 through 7 run
against a **subset** of `$LETTERS`, and an entire run -- every refinement, every
review round -- belongs to one such subset.

`dfs-anagrams` takes that working bag two ways, and both are used:

| Form | Meaning | Invocation |
| --- | --- | --- |
| `o-LETTERS` | use **only** these letters | positional letters = `LETTERS` |
| `u-LETTERS` | the sentence **less** these letters | positional `$LETTERS`, plus `-u LETTERS` |

Both exist for legibility, not for expressiveness: they describe the same bag
from opposite ends, and which one reads better depends on whether more letters
were kept or removed. `LETTERS` is whatever string is comprehensible -- the
words run together -- and it is passed to `dfs-anagrams` verbatim.

`LETTER_SET` is that label as it appears in artifact names. `DFS_LETTERS` is
the argv fragment, an array because the two forms differ in argument count,
which keeps stages 3, 5, and 7 to one substitution -- `"${DFS_LETTERS[@]}"` in
place of `"$LETTERS"` -- and free of a branch.

Stages 1 and 2 are unaffected. A restricted bag's candidate set is a subset of
the full bag's, so the sentence-wide P1 seed is a valid superset for every
letter set at one `MIN_WORD_LENGTH`: one P1 cycle, the expensive stage, serves
them all.

## Artifact names

Filenames are canonical content keys. They retain dimensions needed to find,
distinguish, or interpret their contents; they do not encode the complete
command history.

| Artifact | Canonical example |
| --- | --- |
| Candidate P1 input | `idx/idx.2.s2.m4` |
| P1 85/15 seed list | `pairs.s2.idx2.m4.x2.85.15.p1.yes` |
| Provisional DFS output | `dfs.s2.idx2.m4.x2.g4.85.15.1000000.u-thisandthat` |
| P2 candidate input | `top.s2.m4.g4.u-thisandthat.1000.pairs` |
| Confirmed BEST PAIRS | `top.s2.m4.g4.u-thisandthat.1000.p2.yes` |
| Final DFS output | `dfs.s2.idx2.m4.x2.g4.top1000.1000000.u-thisandthat` |

The sentence and artifact kind persist throughout. DFS-derived and BEST PAIRS
artifacts retain the exact segment count and the letter set they were searched
under -- trailing on the DFS names, which are read by eye and matched by
nothing, and ahead of the cutoff on the P2 bundle, whose name must stay a true
prefix of every artifact derived from it. Selection boundaries remain while
they distinguish candidate sets: the P1 probability band, retained DFS result
count, and P2 candidate cutoff.

`idx2`, `m`, and `x` distinguish index generation and candidate-entry
constraints. The filenames omit stable scoring choices (`--word-bonus 1` and
the default pair bonus 1), the project-wide dictionary, and execution-only
settings such as thread and cache controls. The commands below are the
reproducibility record for those details.

Set the paths used by the recipe:

```sh
CANDIDATES=$NUT/idx/idx.2.$SENTENCE.m$MIN_WORD_LENGTH
REMAINING=$CANDIDATES.remain
P1_SEED=$RESULT_DIR/pairs.$SENTENCE.idx2.m$MIN_WORD_LENGTH.x$MAX_EXTRACT_WORDS.85.15.p1.yes
PROVISIONAL_DFS=$RESULT_DIR/dfs.$SENTENCE.idx2.m$MIN_WORD_LENGTH.x$MAX_EXTRACT_WORDS.g$SEGMENTS.85.15.$DFS_RESULTS.$LETTER_SET
TOP_BUNDLE=top.$SENTENCE.m$MIN_WORD_LENGTH.g$SEGMENTS.$LETTER_SET.$TOP_COUNT
TOP_PAIRS=$RESULT_DIR/$TOP_BUNDLE.pairs
BEST_PAIRS=$WFROOT/.wf/p2/done/out/$TOP_BUNDLE.p2.yes
FINAL_DFS=$RESULT_DIR/dfs.$SENTENCE.idx2.m$MIN_WORD_LENGTH.x$MAX_EXTRACT_WORDS.g$SEGMENTS.top$TOP_COUNT.$DFS_RESULTS.$LETTER_SET
```

## 1. Generate and complete P1 candidate batches

Generate the sorted, unique candidate-pair file:

```sh
build/query-index "$IDX" "$LETTERS" \
  -m "$MIN_WORD_LENGTH" \
  -x "$MAX_EXTRACT_WORDS" \
  --dict "$DICT" \
  -n 0 \
  --csv |
  sort -u > "$CANDIDATES"
```

Subtract pairs already completed in P1. Both inputs to `comm` must be sorted;
the workflow done-set and the command above satisfy that requirement.

```sh
comm -23 \
  "$CANDIDATES" \
  "$WFROOT/.wf/p1/done/p1_done.pairs" \
  > "$REMAINING"
```

If the remaining file is substantially larger than one million lines, split
it into files of at most one million lines. Choose an empty output directory so
stale chunks cannot be submitted accidentally.

```sh
P1_CHUNKS=$RESULT_DIR/p1-chunks.$SENTENCE.m$MIN_WORD_LENGTH
mkdir -p "$P1_CHUNKS"
split -l 1000000 "$REMAINING" "$P1_CHUNKS/chunk."
```

For each chunk, run the complete P1 lifecycle. `wf eval p1` prepares a bundle;
it does not run `evalpair`.

```sh
INPUT=$P1_CHUNKS/chunk.aa
BUNDLE=$(basename "$INPUT")

"$WF" -d "$WFROOT" submit p1 "$INPUT"
"$WF" -d "$WFROOT" eval p1 "$BUNDLE"
```

Run the established `evalpair` command for the selected model and prompt using
the pairs file reported by `wf eval p1`. Pass `--save` and set its results
directory to the in-flight bundle so the resulting JSONL lands here:

```text
$WFROOT/.wf/p1/eval/$BUNDLE/
```

The deployment-specific host, model, prompt, concurrency, and authentication
arguments are intentionally not part of this workflow's artifact identity.
After `evalpair` has completed the entire chunk and its JSONL is in the bundle,
complete P1:

```sh
"$WF" -d "$WFROOT" complete p1 "$BUNDLE"
```

Repeat submission, evaluation, `evalpair`, and completion for every chunk. If
the input does not need splitting, submit `$REMAINING` directly and use its
basename as the bundle name.

## 2. Extract the sentence-wide P1 seed list

Extract P1 YES results whose directional probability lies in the 85/15 band,
without queueing the result into P2:

```sh
"$WF" -d "$WFROOT" extract p1 yes \
  --pairs "$CANDIDATES" \
  --pm "$P1_MIN" \
  --pr "$P1_RANGE" \
  -o "$P1_SEED"
```

The band covers directional probability `[0.85, 1.0]`; it is not the top 15
percent of results. Absolute paths are intentional because the `wf` wrapper
changes its working directory.

## 3. Run provisional fixed-segment-count DFS

Use the P1 seed as a pair-bonus list:

```sh
build/dfs-anagrams "$IDX" "${DFS_LETTERS[@]}" \
  -m "$MIN_WORD_LENGTH" \
  -S 20 \
  -p 10000000 \
  -n "$DFS_RESULTS" \
  --word-bonus 1 \
  --dict "$DICT" \
  --pairs "$P1_SEED" \
  -x "$MAX_EXTRACT_WORDS" \
  -g "$SEGMENTS" \
  > "$PROVISIONAL_DFS"
```

The exact `-g` value is essential. Repeat the provisional, review, and final
stages independently for every relevant segment count, and for every letter
set.

## 4. Rank P2 candidates

Select the `TOP_COUNT` most frequent multi-word segments among the retained
`DFS_RESULTS` results:

```sh
build/top-segments "$PROVISIONAL_DFS" --pairs |
  head -n "$TOP_COUNT" > "$TOP_PAIRS"
```

`top-segments --pairs` emits multi-word segments as comma-separated pairs. The
top 1000 is therefore a frequency ranking within the retained DFS sample, not
a global ranking over every possible anagram.

Inspect the distribution before submitting it. If the tail still contains
many plausible candidates, increase `TOP_COUNT`. If common poor entries crowd
out the useful sample, use the hard-NO refinement loop below and rerun DFS
before selecting the P2 batch.

## 5. Exclude dominating hard-NO segments

An unchecked P2 entry is a soft NO, not a hard exclusion. Use this loop only
when a clearly bad segment consumes enough of the retained DFS sample to make
the frequency ranking unreliable.

Put explicitly rejected pairs in a separate file, one comma-separated pair per
line, then classify them:

```sh
HARD_NO_INPUT=$RESULT_DIR/hard-no.$SENTENCE.m$MIN_WORD_LENGTH.g$SEGMENTS.pairs
"$WF" -d "$WFROOT" classify no "$HARD_NO_INPUT"
```

The aggregate is:

```text
$WFROOT/.wf/classified/no/no.pairs
```

Remove those entries from the positive seed used for the next iteration and
also pass the workflow root as the exact exclusion source:

```sh
REFINED_SEED=$RESULT_DIR/pairs.$SENTENCE.idx2.m$MIN_WORD_LENGTH.x$MAX_EXTRACT_WORDS.85.15.hard-no-filtered

comm -23 \
  "$P1_SEED" \
  "$WFROOT/.wf/classified/no/no.pairs" \
  > "$REFINED_SEED"

build/dfs-anagrams "$IDX" "${DFS_LETTERS[@]}" \
  -m "$MIN_WORD_LENGTH" \
  -S 20 \
  -p 10000000 \
  -n "$DFS_RESULTS" \
  --word-bonus 1 \
  --dict "$DICT" \
  --pairs "$REFINED_SEED" \
  --exclude-pairs "$WFROOT" \
  -x "$MAX_EXTRACT_WORDS" \
  -g "$SEGMENTS" \
  > "$PROVISIONAL_DFS"
```

Passing a workflow root resolves
`$WFROOT/.wf/classified/no/no.pairs`. Supplying `--exclude-pairs` is an explicit
request: a missing `.wf` directory or missing aggregate must be an error, not
an empty exclusion set.

Rerun `top-segments` after each refinement. Stop when known-bad entries no
longer consume a substantial part of the retained sample. Historical
iterations need not be preserved; add an explicit iteration suffix if they do
need to coexist.

## 6. Submit the candidate list to P2

Submit the neutral candidate artifact. It must not be called `*.p1.yes`,
because some frequent segments may not have been members of the P1 seed list.

```sh
"$WF" -d "$WFROOT" submit p2 "$TOP_PAIRS"
"$WF" -d "$WFROOT" eval p2 --no-filter "$TOP_BUNDLE"
```

`wf submit p2` treats a terminal `.pairs` suffix idempotently and creates this
queue/bundle transition:

```text
top.s2.m4.g4.u-thisandthat.1000.pairs
  -> .wf/p2/queued/top.s2.m4.g4.u-thisandthat.1000.pairs
  -> .wf/p2/eval/top.s2.m4.g4.u-thisandthat.1000/
```

`--no-filter` is recommended for these small, high-value review batches, but it
is not required. It restores every previously completed pair present in the
submitted file, including prior confirmed YES entries and prior unchecked soft
NOs. Without it, P2's accumulated done-set avoids repeat review across segment
counts and candidate increments.

Manually review the notes created by `wf eval p2`. A checked entry is confirmed
YES. An unchecked entry is a soft NO: it joins the P2 done-set but is not given
P1 auto-NO provenance, placed in the hard-NO aggregate, or automatically
advanced to P3.

When the review is complete, let the workflow retrieve and parse the note
parts:

```sh
"$WF" -d "$WFROOT" complete p2 "$TOP_BUNDLE"
```

Do not reproduce this step with a hand-written note retrieval loop. The
confirmed BEST PAIRS artifact is:

```text
$WFROOT/.wf/p2/done/out/top.s2.m4.g4.u-thisandthat.1000.p2.yes
```

## 7. Run final DFS with BEST PAIRS

Use the manually confirmed BEST PAIRS as the pair-bonus list. If a hard-NO
aggregate exists, continue enforcing it in the final run.

```sh
EXCLUDE_ARGS=()
if test -f "$WFROOT/.wf/classified/no/no.pairs"; then
  EXCLUDE_ARGS=(--exclude-pairs "$WFROOT")
fi

build/dfs-anagrams "$IDX" "${DFS_LETTERS[@]}" \
  -m "$MIN_WORD_LENGTH" \
  -S 20 \
  -p 10000000 \
  -n "$DFS_RESULTS" \
  --word-bonus 1 \
  --dict "$DICT" \
  --pairs "$BEST_PAIRS" \
  "${EXCLUDE_ARGS[@]}" \
  -x "$MAX_EXTRACT_WORDS" \
  -g "$SEGMENTS" \
  > "$FINAL_DFS"
```

The result is the final ranked anagram sample for this sentence, letter set,
and segment count. Repeat stages 3 through 7 for other segment counts and
other letter sets. P2's completed-pair
filter normally handles overlap between their top-segment lists; use
`--no-filter` when deliberately reconsidering prior decisions.

## Operational notes

- A later run with the same sentence, `m`, `g`, and cutoff may collide with an
  archived P2 batch. Preserving both histories is not currently required. Use
  `wf complete -f` only when deliberately replacing the archived artifacts;
  `complete` is the operation that owns overwrite authority.
- Before collecting accurate timing data, check the host process table for
  other `query-index` and `dfs-anagrams` processes. Concurrent sessions can
  invalidate the measurement.
- `-x 2` limits this workflow to two-word candidate segments. A future
  exclusion format for longer segments will require a separate contract.
