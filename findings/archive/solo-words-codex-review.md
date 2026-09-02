# Review of `plans/solo-words-codex.md`

The architecture is fundamentally sound, but the plan should be revised before
implementation. The main risk is numeric pruning correctness, not the matching
model itself.

## Findings

### 1. High: phase-3 upper-bound comparisons are not floating-point safe

The plan requires direct comparisons between tuple upper bounds and the exact
result floor (`plans/solo-words-codex.md:90-99`, `227-239`). Once exact scoring
is recomputed as base score plus matching bonus, it uses a different addition
order from the phase-2 upper bound. Although the upper bound is mathematically
admissible, floating-point regrouping can put it one or more ulps below the
exact score.

The existing phase-3 cutoffs are direct `<=` comparisons in
`source/dfs-output.cpp` around lines 138, 153, 172, and 210. The rounding
padding mentioned by the plan protects projected and length bounds, but does
not automatically protect these phase-3 comparisons.

Require a conservative comparison helper or upward-rounded tuple bounds at
every phase-3 cutoff, with an error envelope comparable to the existing search
bound treatment. Add a one-ulp boundary test to `test-dfs-output.cpp`.

### 2. Medium: maximum-cardinality matching conflicts with “may pair”

The outcome says a member “may pair” (`plans/solo-words-codex.md:9`), while the
scoring contract forces the maximum possible number of assignments (lines
39-45). With a negative word bonus, an eligible segment therefore must accept a
penalty; it cannot remain unmatched.

That is defensible, but it should be confirmed and stated as “every eligible
segment is matched whenever capacity permits.” Otherwise the conventional
interpretation is optional maximum-weight matching, which would decline
negative edges. The CLI help should mention the penalty behavior because it is
observable.

### 3. Medium: “complete index entry” contradicts aggregate lookup semantics

The scoring contract says the two-word phrase must exist as a “complete index
entry” (lines 30-32), but lines 60-61 deliberately choose aggregate
trailing-space existence. Aggregate lookup can accept `"solo member"` solely
because a longer `"solo member tail"` path exists; `IndexReader::exact_entry_count()`
is what distinguishes an exact entry.

Aggregate behavior is consistent with current phase-1 extraction and
`query-index --score`, so it may be the right choice. The contract and help
should call it an “aggregate phrase prefix accepted by phase 1,” rather than a
complete entry. Otherwise an implementer or user will reasonably infer exact
residual existence.

### 4. Medium: floating-point maximum-cost flow is unnecessarily fragile

At fixed matching cardinality `K`, every matching scores:

```text
W * K + P * known_pair_edge_count
```

Thus `W` never affects assignment selection. The secondary optimization only
needs to maximize the number of known-pair edges when `P > 0`, minimize it when
`P < 0`, and ignore it when `P == 0`.

Instead of double-valued path costs as proposed around lines 140-148, use
integer edge costs such as `0/1`, then calculate `W*K + P*H` once afterward.
This avoids floating comparisons inside residual-path selection. The plan
should also name the path algorithm; naive Dijkstra is invalid with negative
residual edges.

### 5. Medium: tests omit the unusual negative-bonus semantics

The matcher tests listed around lines 303-310 cover scarcity and rerouting, but
not:

- negative `W` still forcing maximum cardinality;
- negative `P` preferring ordinary edges and potentially rerouting to minimize
  known-pair use; or
- a profile with both edge kinds choosing `max(W, W + P)` for a one-row query.

These should be compact unit cases because the maximum-cardinality rule was
introduced specifically for zero and negative bonuses.

### 6. Medium: ordinary `query-index` merging lacks an explicit smoke test

The generalized category merge at lines 256-276 is a substantial independent
implementation, but the listed assertions mainly exercise DFS and
`query-index --score`. A round-trip score test does not exercise partitioning,
per-category partial sorts, k-way merging, or the `-n` cutoff.

Add one ordinary `query-index -n` case spanning plain, ordinary-edge,
known-edge, both-edge, and phrase categories. It should compare bounded output
with the prefix of unlimited output.

### 7. Low: verification omits a test that the plan modifies

Phase 2 includes `source/test-dfs-output.cpp` (line 342), but the final command
runs only the two CLI suites (line 391). It should include at least:

```bash
meson test -C build dfs-solo-words dfs-output dfs-cli query-index-cli
```

The focused matcher test should be mandatory, not conditional on whether a
convenient interface happens to emerge. The plan already calls for separating
pure assignment from index traversal precisely to create that seam.

## Choices that look correct

- The independent per-member maximum is a valid mathematical upper bound
  despite reusing solo capacity.
- Reordering members by base plus local upper bound is sufficient to keep tuple
  descendants non-improving.
- Exact matching must be performed after concrete spelling expansion.
- Two packed profile-category bits are sufficient; exact profiles can remain
  in a frozen text-keyed table.
- The continuation lookup strategy correctly avoids retraversing extracted
  candidate words.
- The proposed query categories have constant one-row bonuses, including the
  “both” category, whose bonus is `max(W, W + P)`.
