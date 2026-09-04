# High-impact segment analysis and review workflow

Date: 2026-09-01

This is a design finding only. It proposes calculation tools, persistent
workflow files, and `wf best` commands for impact-ranked segment review. None
of the proposed commands or workflow behavior are implemented by this file.

## Terms used in this document

- A **result row** is one complete result emitted by `dfs-anagrams`.
- A **result set** is the result file currently being analyzed. The examples
  assume that it contains up to one million result rows.
- A **segment** is a word pair that the existing segment tools extract from a
  result row, such as `away,team`. Each segment is counted at most once per
  row.
- A segment classification is **NO** when that segment is known to be bad,
  **YES** when it is known to be plausible, and **undecided** when it has not
  been classified. A **hard NO** is used as an exclusion rule: every result
  row containing it is rejected.
- A row **survives** when it contains no current hard-NO segment. The
  **survivor set** is the set of all such rows in the result set.
- The **result count** of a segment is the number of surviving rows containing
  it. Its **coverage percentage** is that count divided by the total number of
  surviving rows.
- A **coverage threshold** is the minimum result count or coverage percentage
  required for a segment to be considered at a particular review stage.
- A **review candidate** is an undecided segment being considered for human
  review. The **review frontier** is the current ordered candidate list, and a
  **batch** is the small prefix selected for one review round. A published,
  unreviewed frontier **owns the target**, meaning that another command must
  not replace that frontier until it is reviewed or explicitly discarded.
- The **anchor** is the segment whose effects are being ranked or explained.
  The **intersection** of the anchor and another segment is the set of
  surviving rows containing both.
- **Marginal impact** counts only rows not already covered by earlier
  candidates in the same hypothetical batch.
- A file or calculation is **fresh** when its recorded inputs and policy still
  match the live ones; it is **stale** when one of them has changed. A
  **digest** is a content hash used to make that comparison. A **content
  clock** is any recorded value, usually a digest, that changes when the
  relevant content changes.
- A **lane** is one branch of workflow scheduling. Later sections call the
  `dfs.best` branch `refine` and the `dfs.seed` branch `reseed`.
- The analysis **source** identifies the result set: `seed` means `dfs.seed`,
  and `best` means `dfs.best`.

## Goal

The ultimate goal is to find the correct anagram in a million-row
`dfs-anagrams` result set, or eliminate nearly all of that result set, then use
the accumulated hard-NO segment classifications to generate another million
results and repeat.

The basic frequency workflow already works:

1. generate a million results;
2. review frequent segments;
3. classify bad segments NO;
4. reject rows containing those segments;
5. regenerate.

The proposed improvement is to select review candidates by their likely
effect on the current survivor set and to make the entire inner loop
resumable without the operator remembering which calculation, review, or
generation step comes next.

The operating loop becomes:

```text
generate million
      |
      v
analyze surviving results
      |
      v
review small impact-ranked batch
      |
      v
complete classifications
      |
      v
re-analyze the same million
      |
      v
review another batch
      |
      v
eventually deepen threshold or generate next million
```

The important distinction is that an existing DFS result can be:

- stale for generating the next search because new hard-NO classifications
  exist; and
- still useful as a result set that can be filtered and analyzed.

The analysis loop should continue using the same million-row result set until
its marginal review yield is exhausted. Only then should status recommend
another expensive DFS generation.

## Interpretation of the current `select-segment` output

The current result hierarchy performs successive intersections. For example:

```text
know kind (443)
  away,team (387)
    snow,white (78)
```

means:

- 443 surviving rows contain `know kind`;
- 387 contain both `know kind` and `away team`; and
- 78 contain all three displayed segments.

This is useful for exposing the group around a selected segment. It is not a
decision tree: sibling branches overlap, and combinations can appear in more
than one order. Counts from different branches therefore cannot be summed.

It also lacks the other segment's result count across the complete survivor
set. An intersection of 387 does not say whether removing `know kind` would
nearly eradicate `away team` or merely subtract 387 from tens of thousands of
its occurrences.

The useful part should become a flat report that shows the complete result
count and both conditional percentages. Output that repeats combinations in
different orders should not be the primary impact calculation.

## Calculation model

The notation used below is:

- `U`: the survivor set, after applying all current hard-NO exclusions;
- `R_s`: the rows in `U` that contain segment `s`;
- `result_count(s) = |R_s|`: the number of rows in `R_s`; the vertical bars
  mean "number of rows in this set";
- `R_s ∩ R_t`: the intersection of those sets, meaning the surviving rows
  that contain both segment `s` and segment `t`;
- `|R_s ∩ R_t|`: the number of rows in that intersection; and
- `remaining_result_count(s,t) = result_count(t) - |R_s ∩ R_t|`: the result
  count that segment `t` would retain if every row containing anchor `s` were
  removed.

There are two relevant segment sets. The **candidate set** contains undecided
segments at or above `--min-coverage`; its members can be selected for the
current review batch. The **effect set** contains undecided segments at or
above `--fallout-coverage`; it is the set over which second-order effects are
reported. Usually the fallout coverage threshold is at or below the candidate
coverage threshold.

For every candidate `s`, calculate:

- **Potential direct impact:** `result_count(s)` rows. This is the primary
  payoff if `s` is classified NO.
- **Marginal impact:** rows in `R_s` not already covered by earlier candidates
  in a proposed review batch.
- **Combined result-count reduction:** add `|R_s ∩ R_t|` for every other
  segment `t` in the effect set. In mathematical shorthand this is "sum over
  `t` of `|R_s ∩ R_t|`." A row containing several such segments contributes
  once for each, so this number double-counts rows and is secondary.
- **Segments extinguished:** the number of segments `t` in the effect set for
  which `remaining_result_count(s,t) == 0`.
- **Threshold fallout:** the number of segments `t` that currently meet the
  next coverage threshold but for which `remaining_result_count(s,t)` is
  below that threshold.

Potential direct impact is the immediate effect if the candidate is
classified NO. Segments extinguished and threshold fallout are the genuine
second-order effects: they do not eliminate additional rows, but they reduce
how much future segment review is required.

If review costs are roughly equal and most candidates fail the human test,
descending result count is already a strong default ordering. In the more
general case, the ideal value is approximately:

```text
P(segment is NO) * marginal impact / review cost
```

Here `P(segment is NO)` means the estimated probability that human review
will classify this particular segment NO. The calculation tool does not know
that probability. It should therefore expose separate objective measurements
rather than manufacture one opaque weighted score.

### Heavy-hitter bound

Here a **heavy hitter** is simply a segment with a high result count.

The number of segments above a fixed coverage percentage is inherently
limited. If each result contains an average of `m` extracted segments and `p`
is a coverage threshold expressed as a fraction of the survivor set, then no
more than `m / p` distinct segments can each occur in at least that fraction
of the rows.

At a 5% threshold this is at most `20m`, meaning 20 times `m`. If a result has
five extracted segments on average, at most 100 segments can meet the
threshold. This makes a high-coverage-first review substantially smaller than
a fixed 1,000-segment frontier.

## Proposed Nutrimatic tool: `segment-impact`
Add one C++ executable alongside `top-segments` and `filter-segments`. It
should parse the same result format and use the same pair-filter behavior:
ignored segments contribute no candidate or effect counts, while a rejected
segment discards its complete result row.

Proposed commands:

```text
segment-impact rank [OPTIONS] RESULT-FILE
segment-impact explain -s SEGMENT [OPTIONS] RESULT-FILE
```

Example:

```text
segment-impact rank --wf --yes \
    --min-coverage 5% \
    --fallout-coverage 1% \
    --batch-size 50 \
    dfs.best
```

### `rank`

`rank` should make two sequential passes over the result file:

1. reject every row containing a hard-NO segment;
2. count surviving result rows containing each undecided pair segment;
3. select the candidate set at or above `--min-coverage` and the effect set at
   or above `--fallout-coverage`;
4. compute each candidate's intersection with each segment in the effect
   set;
5. calculate direct and second-order effects; and
6. select a small review batch.

Result counts are per row. The parser should defensively deduplicate a segment
within a row before incrementing its result count.

Both sets exclude classified YES and NO segments. Hard-NO segments reject
complete rows; confirmed-YES segments remain allowed in rows but are ignored
when calculating review candidates and their second-order effects.

A stable tab-separated-value (TSV) report should contain at least:

```text
selected
rank
segment
result_count
coverage_percent
batch_marginal_impact
combined_result_count_reduction
segments_extinguished
segments_below_fallout_threshold
```

Default ranking should remain descending `result_count`. The second-order
columns explain additional value and can support explicit alternative
orderings after their usefulness is established. They should not initially be
collapsed into one magic score.

`batch_marginal_impact` makes an explicit hypothetical assumption: earlier
selected candidates in the batch become NO. Because verdicts are not known
until human review, batches should remain small and the report should be
regenerated after each completed batch.

### `explain`

`explain` should replace the recursive hierarchy with a flat conditional
report:

```text
segment       result count  intersection  % anchor  % segment  remaining
away,team            38210           387      87.4        1.0      37823
rear,window            133           133      30.0      100.0          0
```

For an anchor `s` and a displayed segment `t`, `result count` is
`result_count(t)`, `intersection` is `|R_s ∩ R_t|`, `% anchor` is
`|R_s ∩ R_t| / result_count(s)`, `% segment` is
`|R_s ∩ R_t| / result_count(t)`, and `remaining` is
`remaining_result_count(s,t)`. Percentages are multiplied by 100 for display.

The two conditional percentages answer different questions:

- `intersection / anchor result count`: what characterizes the anchor's group
  of rows?
- `intersection / segment result count`: how much damage would rejecting the
  anchor do to this other segment?

An optional `--examples N` should print representative complete DFS result
rows. This preserves the useful contextual discovery performed by
`select-segment` without treating overlapping intersections as a hierarchy.

## One durable review frontier

Do not add an `impact.pairs` review frontier beside `top.segments`. The BEST
workflow should retain one shared frontier and at most one review that is
queued or being evaluated.

Proposed per-target artifacts:

```text
top.segments                 selected review batch; existing contract
.top.segments.gen            existing marker for source and frontier generation
impact.tsv                   complete current ranking report
impact.meta.json             inputs, settings, counts, and freshness data
impact-rounds/
  <review-bundle>.json       historical round measurements
```

`impact.tsv` is diagnostic data. Only `top.segments` is submitted to the
existing P2 review path.

### Analysis metadata

`impact.meta.json` should record:

```text
schema version
source: seed or best
resolved DFS result path
DFS file size and modification time, plus an optional content digest
hard-NO content digest
confirmed-YES content digest
surviving result count
coverage threshold
fallout coverage threshold
batch size
frontier digest
generation time
```

The metadata makes all freshness predicates explicit and preserves the
operator's selected policy across sessions.

Publication should use sibling temporary files. Publish the report, metadata,
and `top.segments`, then publish the generation marker last. If interrupted
before the marker, status detects an incomplete or inconsistent analysis and
offers the exact recovery command.

The `.top.segments.gen` marker should continue recording its source as exactly
`seed` or `best`; the more detailed record of analysis inputs belongs in
`impact.meta.json`.

## Proposed workflow commands

Add two `wf best` actions in the Words workflow repository:

```text
wf best analyze SENTENCE ... --source seed|best
wf best inspect SENTENCE ... [SEGMENT]
```

### `wf best analyze`

`analyze` should:

1. resolve `dfs.seed` or `dfs.best`;
2. run `segment-impact rank`;
3. atomically publish `impact.tsv`, metadata, and `top.segments`;
4. retain the coverage ladder, meaning the configured sequence of thresholds,
   and the batch size; and
5. report the resulting survivor count and review frontier size.

The first invocation may accept explicit policy such as:

```text
--coverage-levels 5%,2%,1%,0.5%
--batch-size 50
--fallout-coverage 1%
```

Subsequent status output should render exact commands using the stored policy,
so the operator does not need to remember these values.

### `wf best inspect`

`inspect` is read-only:

- without a segment, summarize the current report and policy;
- with a segment, display its conditional `segment-impact explain` report;
- optionally show representative result rows.

### Existing actions

The human-facing review actions remain:

```text
wf best review ...
wf best notes ...
wf best complete ...
```

Eventually, `wf best prepare` should run DFS followed by `analyze`, just as it
currently runs DFS followed by `top-segments`. During initial validation,
impact analysis can be an explicit strategy so the current frequency frontier
remains available.

`complete` should not start an expensive DFS or silently run the next
analysis. It should finish the review, update classifications and
`best.pairs`, then let status identify the next action.

## Resumable state machine

`wf best status` should derive the following states in this order.

### 1. Review queued

Print the exact `wf eval p2 ...` recovery command.

### 2. Review evaluating

Print `wf best complete ...` and the notes location.

### 3. Unsubmitted current frontier

Print `wf best review ...`. The other seed/best lane is waiting because the
current frontier owns the target.

### 4. Completed frontier and changed classifications

The old million remains the current analysis result set. Print:

```text
next: wf best analyze ... --source best
reason: completed review changed the hard-NO or YES set
```

The previous frontier is already covered by its review round and must not be
submitted again merely because the label clocks moved.

### 5. DFS exists but analysis is missing or incomplete

Resume with `analyze`, not another DFS generation.

This covers interruption after a DFS was published and interruption during
analysis publication.

### 6. Current analysis has review candidates

Print `wf best review ...`.

### 7. No candidates remain at the current coverage threshold

Present an operator choice:

```text
choose next:
  deepen:     wf best analyze ... --source best --min-coverage 2%
  regenerate: wf best prepare ... --source best
  reseed:     wf best prepare ... --source seed
```

These are scheduling alternatives, not instructions to run all three.

### 8. No further configured coverage threshold

Recommend regeneration while preserving `inspect` as an optional diagnostic.

## Desired status display

A useful resumed-session display would look like:

```text
s7/u-vindiesel/m4/g4:
  result set: dfs.best, generation 6, 1,000,000 rows
  surviving: 184,219 (18.4%)
  impact: 2% coverage threshold, batch 4 of this result set
  progress:
    reviewed: 146 segments
    classified NO: 119
    classified YES: 27
    unique rows eliminated: 612,403
    last batch yield: 3,841 rows per NO

  refine loop:
    next: wf best analyze s7 -u vindiesel -g 4 --source best
    reason: review r4 added 18 hard-NO segments

  reseed loop:
    waiting for the current best-derived result set
```

After analysis:

```text
  review frontier:
    37 candidates
    potential direct impact: 3,742-29,118 rows
    distinct rows covered by batch: 91,402
  next: wf best review s7 -u vindiesel -g 4
```

This answers:

- Which million-row result set is active?
- How many rows survive?
- Which threshold and review batch are active?
- Was the result set re-analyzed after the last review?
- Is there an unfinished review?
- What exact command comes next?
- Is it time to deepen the threshold or regenerate?

## Durable progress history

Each submitted review should have a manifest keyed to the existing review
bundle name. Record:

- DFS result-set path, file size, modification time, and optional digest;
- analysis and frontier digest;
- coverage threshold and batch size;
- survivor count before review;
- submitted candidates and their predicted effects;
- YES and NO verdict counts;
- survivor count after the next analysis;
- actual unique rows eliminated; and
- actual elimination yield.

Per-round JSON files are preferable to one append-only log. Each can be
atomically replaced and inspected independently, and one interrupted update
cannot corrupt the complete history.

The next `analyze` can fill in a completed round's after-measurements. Until
then, status should say that the completed round is awaiting measurement.

Historical manifests support progress reporting, but they are not an
imperative cursor. The current step should still be derived from live
artifacts, review locations, generation markers, and freshness predicates.

## Status architecture

The previously proposed two-lane BEST status model remains appropriate:

- `refine` uses `dfs.best`;
- `reseed` uses `dfs.seed`;
- both share one `top.segments`, one possible active review, and one target
  review sequence;
- main workflow actions render before maintenance notices;
- labels distinguish `next`, `after maintenance`, `waiting`, and `caught up`.

Impact analysis adds a cheap inner loop within the source that owns the
current result set. A changed NO set should make analysis stale without making
the result set unusable. Search freshness and result-set usability must be
separate predicates.

The status report should distinguish:

- **main workflow:** analyze, review, complete, deepen, or generate;
- **progress:** survivor counts and measured review yield; and
- **maintenance:** stale `best.pairs`, damaged markers, dangling links, or
  incomplete historical measurements.

Maintenance facts must not hide the main next action.

## Shared-frontier safety prerequisite

Before status can act as a safe scheduler, frontier ownership must be
complete. The current BEST preflight prevents replacing `top.segments` while
a top review is queued or evaluating. It does not protect an unsubmitted
frontier for which status says review is needed.

The required rule is:

> From successful frontier publication until its review is completed or
> explicitly discarded, that frontier owns the target.

Every command that writes `top.segments` -- legacy generation, `analyze`, and
`prepare` -- must enforce the same predicate. Normal status must not advertise
another lane's frontier-writing command while an unreviewed frontier owns the
target.

## Hard and soft classifications

Hard-NO classifications are search constraints: a mistaken hard NO can
permanently exclude the correct answer. It may eventually be useful to retain
a separate soft-NO or down-ranking layer for judgments that are persuasive but
not safe enough for irreversible pruning.

Likewise, YES means plausible, not required. YES classifications should
preferentially rank results or provide pair bonuses; they should not restrict
the survivor set to answers containing every YES segment.

This suggests two complementary lanes of evaluation:

- **elimination:** review high-coverage likely-NO segments; and
- **discovery:** promote complete results containing several credible YES
  segments and no NO segments.

The initial `segment-impact` work should focus on elimination. Whole-result
promotion can be added independently without changing the impact contracts.

## Suggested implementation order

1. Implement `segment-impact rank` and `explain`, then validate the calculated
   result counts and intersections against small fixtures and a current
   million-row result.
2. Add `wf best analyze` with `impact.tsv`, metadata, atomic publication, and
   the existing `top.segments` review path.
3. Add status states for analysis stale, analysis interrupted, and review
   batch ready. Complete the unsubmitted-frontier ownership guard at the same
   time.
4. Add per-round manifests and yield/progress reporting.
5. Integrate impact analysis into `prepare` and add decisions between the
   configured coverage thresholds.

This sequence produces a useful review frontier after step 2 without making
the full status redesign a prerequisite for validating the calculation model.
