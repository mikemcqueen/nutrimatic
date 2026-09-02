Plan written to plans/solo-words-claude.md.

Shape of it, given your three answers:

- Tier 1 (word bonus): a single-word segment W earns it when "S W" or "W S" is an index entry or is in --pairs, for some solo word S. Tier 2 (pair bonus, additive) needs --pairs. So pair_mask ⊆ word_mask, which is what keeps query-index's three-group partition/merge at three groups instead of four.
- Once-per-answer is handled by splitting the score in two: the search side (member ordering, phase-2 bound, phase-3 deltas, pending queue) uses the optimistic bonus where every segment assumes it gets its best partner; DfsTopN::emit() then applies a never-positive correction from an exact max-weight matching and stores that. Optimistic ≥ exact is what keeps member 0 the class best and every prune admissible. Doing it the other way round — exact scores in the pending queue — would break the expansion's early breaks, since a child's correction can be smaller than its parent's.
- 16 solo words, two uint16_t masks. IntermediateMember absorbs them in existing padding; DfsPackedMember goes 16 → 24 bytes, a 50% member-arena increase on every search. The escape hatch (a parallel mask array allocated only with --solo-words) is recorded as a lever, not done preemptively.

Two things I decided that you didn't ask about, both flagged in the Decisions section:

1. --solo-words rejects a negative --word-bonus/--pair-bonus (exit 2). "Best available tier" is only an upper bound while the magnitudes are non-negative; a negative one would break both the phase-2 bound and query-index's constant-bonus-per-group assumption. Existing options stay unrestricted without --solo-words.
2. No new score term. Solo bonuses reuse multi_word_log_bonus_ and pair_log_bonus_, so there's no new DFS_*_BASE and no DfsAnagramSearch signature change — segment_log_score() just gains two defaulted mask parameters, and all eight existing call sites compile untouched.

Two phases: the probe/matching module with its own unit test, then everything else in one commit (masks, ordering, both CLIs) — partial landing would desync member order from the score model, the failure plans/pair-bonus.md records.

The synthetic fixture needs no new entries — ab/cd/dc/ab cd covers the index arm, and a --pairs file supplies arbitrary edges for the assignment test, so the existing absolute-score assertions keep their corpus total.