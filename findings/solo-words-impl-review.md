# `--solo-words` implementation review

Reviewed the implementations at:

- `/home/mike/code/nutrimatic`: `0f3416c`, with the implementation in
  `811888e` and `c7aee22`;
- `/home/mike/code/nutrimatic-2`: `9c0e458`, with the implementation in
  `ce904ed` and `9c0e458`.

The scoring core is correct in both implementations under each one's own CLI
contract. Both make per-segment solo bonuses optimistic during phase 2 and the
phase-3 Cartesian-product walk, then apply an exact whole-answer assignment
before retaining or publishing a result. I found no case where either current
implementation can prune a result that its exact solo score should retain.

The two implementations are nevertheless not interchangeable. The main
checkout is the better base because it implements the settled 16-unique-word
contract, has a genuinely score-inert fast path, and takes more care over the
floating-point upper-bound invariant. `nutrimatic-2` has useful phase-1 probe
optimizations and a useful direct `DfsTopN` regression test that are worth
adapting rather than taking its implementation wholesale.

## Findings

### High: `nutrimatic-2` implements a different public contract

The main checkout accepts at most 16 unique words and rejects duplicates,
including duplicates supplied by repeating the option
(`source/dfs-cli-args.cpp:144-179`). This agrees with the settled decisions in
`plans/solo-words.md:37-61` and
`findings/solo-words-plan-synthesis.md:39-45`.

`nutrimatic-2` instead accepts at most eight occurrences, preserves duplicates,
and defines repetition as extra assignment capacity
(`../nutrimatic-2/source/dfs-cli-args.cpp:38-68`,
`../nutrimatic-2/source/dfs-solo.h:18-24`). Its CLI tests deliberately require
`--solo-words ab,ab` to pay two segments
(`../nutrimatic-2/source/test-dfs-cli.sh:271-290`).

This is observable behavior, not just representation. A command with nine
unique partners succeeds in main and fails in `nutrimatic-2`; a duplicate fails
in main and changes the score in `nutrimatic-2`. Do not replace main with the
sibling implementation unless this product decision is explicitly reopened.

### Medium: `nutrimatic-2` has no score-inert fast path

Main constructs `DfsSoloWords` only when at least one effective bonus is
nonzero and otherwise passes `NULL` through extraction and output
(`source/dfs-anagrams.cpp:307-339`, `source/query-index.cpp:347-391`). The
recorded score-inert run therefore performs no context or profile work and was
within noise of baseline (`findings/solo-words-performance.md:8-32`).

`nutrimatic-2` resolves the supplied words unconditionally, prints a `solo
words:` diagnostic, and passes a non-null context through every single-word
emission (`../nutrimatic-2/source/dfs-cli-args.cpp:83-93`,
`../nutrimatic-2/source/dfs-anagrams.cpp:306-328`,
`../nutrimatic-2/source/query-index.cpp:353-385`). Thus `--solo-words ab` with
no effective bonus preserves stdout scores but still changes stderr and pays
the index-probing cost. That conflicts with the main plan's explicit
score-inert gate (`plans/solo-words.md:229-239`).

Recommendation: retain main's conditional construction. If code is shared
between the implementations, make an empty/inert context skip both resolution
and the diagnostic, rather than relying only on zero-valued score terms.

### Low: main repeats the capacity literal instead of defining one invariant

Main hard-codes `16` in parsing, constructor validation, graph sizing, and the
augmentation loop (`source/dfs-cli-args.cpp:166-175`,
`source/dfs-solo-words.cpp:65-84`, `source/dfs-solo-words.cpp:217-240`). The
sibling defines `DFS_MAX_SOLO_WORDS` once and uses it for parsing, masks, help,
and matching (`../nutrimatic-2/source/dfs-solo.h:18-24`).

Recommendation: add a shared `inline constexpr size_t DFS_MAX_SOLO_WORDS = 16`
to main's solo-word header and replace all capacity literals. This is a small
maintainability fix; no current behavior is wrong.

### Low: main lacks the sibling's direct phase-3 correction regression

Main has good pure matching tests and end-to-end CLI checks, including bounded
top-N, scarcity, rerouting, and `query-index --score` round trips. It does not,
however, directly construct a class list whose optimistically best spelling is
not the exactly best spelling and exercise `DfsTopN::emit()` on that case.

`nutrimatic-2` does exactly that in
`../nutrimatic-2/source/test-dfs-output.cpp:626-701`, and also checks that an
uncontested correction leaves the queued score bit-identical
(`../nutrimatic-2/source/test-dfs-output.cpp:703-727`). This is a sharper unit
guard for the boundary between optimistic traversal and exact retention than a
shell-level top-1 comparison alone.

Recommendation: port the shape of this test to main, adapted to main's
out-of-line profile context and unique-word policy. Do not port the duplicate
capacity assertion.

## Design comparison

| Area | Main checkout | `nutrimatic-2` | Assessment |
|---|---|---|---|
| Public capacity | 16 unique words; duplicates rejected | 8 occurrences; duplicates add capacity | Main matches the settled contract |
| Packed member | 16 bytes; three category bits in `score_flags` | 16 bytes; exact two-byte masks overlay phrase pair state | Both preserve the hot record size |
| Exact masks | Sorted, out-of-line table keyed by candidate text | Stored directly in each single-word member | Sibling has cheaper phase-3 access, but only by limiting capacity to 8 |
| Inert path | No context, probes, profile table, or solo diagnostic | Resolves and probes whenever words are supplied | Main is preferable |
| Index probing | Up to two continuation walks per candidate/solo edge | Child-character screens reject most walks first | Sibling has the better active phase-1 probe structure |
| Exact assignment | Integer-cost augmenting flow over up to 16 partners | Subset DP over at most 256 states | Both are exact; each fits its capacity choice |
| Numeric correction | Long-double exact value, conversion directed downward | Double DP and `min(0, exact - optimistic)` | Main more explicitly preserves the machine upper bound |
| Phase-3 integration | Binary-search exact profiles for eligible members | Reads masks directly from member views | Sibling is simpler/faster in active phase 3 |
| Query behavior | Exact sequence correction; three-way row merge by effective flags | Same | Equivalent under shared inputs and contract |

## Correctness analysis

Main's matcher computes, for every reachable cardinality, the maximum number of
pair-tier edges using integer residual costs, then selects the best
`K * word_bonus + H[K] * pair_bonus`
(`source/dfs-solo-words.cpp:204-278`). Its correction is derived from the same
per-member upper terms used by the pending queue, is forced non-positive, and
is rounded downward when converted to `double`
(`source/dfs-solo-words.cpp:281-315`). `DfsTopN` adds that correction to the
queued upper score before offering the spelling
(`source/dfs-output.cpp:153-193`). This is the strongest implementation of the
pruning invariant.

The sibling's subset DP also computes the exact maximum-weight assignment and
allows a segment to remain unmatched
(`../nutrimatic-2/source/dfs-solo.cpp:176-235`). Its phase-3 queue remains
optimistic and the exact correction is applied only to the spelling offered to
the result heap (`../nutrimatic-2/source/dfs-output.cpp:145-205`). With the CLI's
non-negative-bonus restriction, this is also admissible. The difference is
mainly numerical conservatism and supported capacity, not a discovered wrong
answer.

Both implementations correctly:

- restrict solo matching to single-word segments;
- treat either aggregate index order as a word-tier edge;
- treat a `--pairs` hit as both word-tier and pair-tier;
- enforce one use per available solo partner through exact assignment;
- keep member ordering and phase-2 bounds optimistic;
- correct `query-index --score` sequences and round-trip DFS result scores; and
- extend query-index's three-way merge with effective word/pair predicates.

## Performance recommendations

Keep main's side-table representation and 16-word contract, but consider
adapting two local probe techniques from `nutrimatic-2`:

1. Cache each solo word's possible first continuation characters and the union
   of solo first characters during setup
   (`../nutrimatic-2/source/dfs-solo.cpp:39-86`).
2. Reuse one pair-key string and one children buffer, and use the cached
   character sets to avoid continuation walks that cannot succeed
   (`../nutrimatic-2/source/dfs-solo.cpp:99-153`).

Main currently constructs a fresh pair key and performs both directional
continuation probes inside the candidate/solo loop
(`source/dfs-solo-words.cpp:98-134`). The sibling's screens should reduce active
phase-1 index decoding without requiring its eight-bit packed-mask design or
duplicate semantics.

Do not claim a speed win from the existing timing notes alone. Main's published
measurements use different bags and capacities from the timing recorded in the
sibling commit, so they are not an apples-to-apples comparison. If active
phase-1 cost matters after the simple key reuse, benchmark both probe strategies
on the same checkout, bag, partner lists, build mode, and quiet host process
table before taking the more involved child-set cache.

## Recommendation

Use the main checkout implementation as the canonical version. No scoring fix
is required based on this review. Make the shared capacity constant and add the
direct phase-3 unit regression. Treat the sibling's probe screening and scratch
reuse as targeted performance ideas, gated by a same-workload benchmark; do not
adopt its eight-word/duplicate-capacity contract or unconditional inert-mode
work.

## Validation performed

Main checkout:

```text
meson test -C build dfs-solo-words dfs-output dfs-cli query-index-cli
4/4 passed
```

`nutrimatic-2` (its tree is read-only to this session, so Meson could not write
its log files):

```text
./build/test-dfs-solo
./build/test-dfs-output
bash source/test-dfs-cli.sh build/dfs-anagrams build/make-dfs-test-index \
  build/query-index
source/test-query-index.sh build/query-index build/make-dfs-test-index
all passed
```
