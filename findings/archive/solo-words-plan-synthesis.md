# Solo-words plan synthesis

## Recommendation

Use `plans/solo-words.md` as the only implementation plan. It combines the
Codex draft's 16-byte packed-member design with the Claude draft's safer
optimistic-score-plus-correction model, and replaces both drafts' assignment
algorithms with a small integer matching frontier.

## Why neither earlier plan is sufficient

The Claude plan has the clearer pruning invariant and matches the settled
index-or-`--pairs`, once-per-answer, and 16-word requirements. Its decisive
performance defect is unconditional growth of `DfsPackedMember` from 16 to 24
bytes: 50% more packed-member memory on every run, irregular cache-line
packing, and a cost even when `--solo-words` is absent.

The Codex plan correctly keeps the packed record at 16 bytes and moves exact
profiles out of line. It also correctly distinguishes search upper bounds from
retained exact scores. Its maximum-cardinality-first semantics are not the
natural maximum-score contract: a zero-value ordinary edge can displace a
positive pair edge, and negative edges can become mandatory. Recomputing the
exact score independently also permits floating regrouping to put an alleged
upper bound below the retained score by an ulp.

The canonical plan fixes both issues:

- category flags stay packed while exact masks live in a frozen side table;
- active solo bonuses are non-negative;
- matching maximizes total score, including the choice to leave an edge
  unused;
- the matcher derives an integer maximum-known-edge frontier for each
  cardinality, avoiding floating residual-path costs; and
- phase 3 stores `pending_upper + nonpositive_correction`, so the value tested
  for pruning is an upper bound in actual machine arithmetic.

## Policy-decision status

The owner has confirmed:

- negative word/pair bonuses are rejected when solo words are active; existing
  negative-bonus behavior remains available when `--solo-words` is absent.
- duplicate solo words are rejected for now, including duplicates introduced
  across repeated option occurrences; duplicates do not add capacity.
- each direct solo-word field must already be nonempty lowercase `a-z0-9`;
  uppercase, punctuation, and spaces are rejected rather than normalized.
- aggregate trailing-space presence defines an index-backed pair; an exact
  residual entry is not required. This preserves phase-1 and
  `query-index --score` semantics and the cheaper continuation probe.

All policy decisions identified during the plan review are resolved.

## Performance unknowns

Static inspection resolves the largest memory question: the hot packed member
must remain 16 bytes. Two dynamic costs remain unknowable before implementation
and measurement:

- directional index probes scale with extracted single words times supplied
  solo words; and
- optimistic reuse of a scarce partner can loosen phase-2/phase-3 bounds and
  multiply spelling expansion.

The canonical plan therefore makes quiet, alternating phase-1 measurements and
a representative phase-2/phase-3 expansion comparison completion gates. It
also requires edge/profile counters so a regression can be attributed rather
than guessed at.
