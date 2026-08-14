# Where segment bonuses go wrong, and what a pair bonus would inherit

`--word-bonus N` is applied in two places that do not agree. Phase 2 accounts
for it per class; phase 3 rescores per member without it. The result is that
inside a class the bonus cancels exactly, and across classes it survives as a
constant inflation applied to every spelling of the class -- including the
single-word spellings it was never meant to favor.

A `--pair-bonus` over a supplied pair list is the same kind of term and would
inherit all of this. It would also break three assumptions the current code
makes that happen to hold only while there is exactly one bonus.

This is analysis, not a plan. Nothing here has been changed.

## The two accountings

One bonus magnitude, set once:

$$
B = N \cdot \log(10^6)
$$

`make_multi_word_log_bonus()` (`source/dfs-score.cpp:16-19`). The intended score
of a member $m$ is

$$
s(m) = \log \operatorname{count}(m) + B \cdot [\,m \text{ is multi-word}\,]
$$

which is exactly `DfsScoreModel::segment_log_score()`
(`source/dfs-score.cpp:29-34`).

**Phase 2** never sees individual members. It walks classes, and charges each
class its best member's score:

$$
\operatorname{best}(c) = \max_{m \in c} s(m)
$$

built in `source/dfs-search.cpp:206-225` and accumulated into
`representative_log_score` in `source/dfs-all-runner.cpp:66-76`. The `for` loop
that computes the max is guarded by `bonus_reorders`
(`source/dfs-search.cpp:208`): under a zero bonus, member 0 is already the best,
so the scan is skipped.

**Phase 3** takes that representative score and adjusts it for whichever member
of each class the spelling actually uses, in `spelling_log_score()`
(`source/dfs-output.cpp:67-81`):

```cpp
double score = representative_log_score;
for (size_t i = 0; i < class_indexes.size(); ++i) {
  if (member_indexes[i] == 0) continue;
  size_t const class_index = class_indexes[i];
  score +=
      log(double(classes.member(class_index, member_indexes[i]).count)) -
      log(double(classes.member(class_index, 0).count));
}
```

The base is in score space, against the *best* member. The delta is in raw count
space, against *member 0*. Those are the same member only when the bonus is
zero.

## The error, exactly

Members are sorted count-descending by `member_order()`
(`source/dfs-class-list.cpp:345-350`), so member 0 is the highest-count member.
Write $c_0 = \operatorname{count}(m_0)$ and let member $j$ be the one a spelling
selects from class $c$.

Actual contribution of class $c$, reading the loop above (the `continue` at
`source/dfs-output.cpp:74` makes $j=0$ contribute nothing beyond the base):

$$
a(j) = \begin{cases}
\operatorname{best}(c) & j = 0 \\
\operatorname{best}(c) + \log \operatorname{count}(m_j) - \log c_0 & j \neq 0
\end{cases}
$$

Intended contribution is $s(m_j)$. Subtracting, both cases collapse to one
expression:

$$
a(j) - s(m_j) = \underbrace{\bigl(\operatorname{best}(c) - \log c_0\bigr)}_{D(c)}
\;-\; B \cdot [\,m_j \text{ is multi-word}\,]
$$

$D(c) \ge 0$ is how far the bonus lifts the class's bound above its own top
count. Two consequences follow directly.

### Inside a class, the bonus cancels

Take a multi-word member $j$ and a single-word member $j'$ of the same class.
The error terms are $D(c) - B$ and $D(c)$. Their difference in *scored* order is

$$
a(j) - a(j') = \bigl(s(m_j) - s(m_{j'})\bigr) - B
= \log \operatorname{count}(m_j) - \log \operatorname{count}(m_{j'})
$$

The $B$ that `segment_log_score()` added is removed again. Phase 3 ranks the
members of a class by raw count, exactly as it would at `--word-bonus 0`. **The
bonus cannot promote a multi-word spelling over a single-word spelling of the
same letters** -- which is the thing it exists to do.

### Across classes, a constant inflation leaks

$D(c)$ does not depend on $j$, so every spelling drawn from class $c$ is
inflated by the same $D(c)$ regardless of which member it used. A class earns
that inflation by *containing* a rare multi-word member, and then spends it on
all of its members. A class whose best member is a rare two-word phrase lifts
its own top single word by $D(c)$ too.

So the bonus is not lost -- it is relocated from "this spelling is a phrase" to
"this spelling's class contains a phrase". That is a different and unwanted
statement.

### At `--word-bonus 0` everything is correct

$B = 0$ makes $\operatorname{best}(c) = \log c_0$, so $D(c) = 0$ and the error is
identically zero. The default path is exact, which is why this has gone
unnoticed: the delta form in `spelling_log_score()` is precisely right for the
unbonused model it was written under.

## Fixing the delta alone is not enough

The obvious repair -- give `DfsTopN` the `DfsScoreModel` and compute
$s(m_j) - \operatorname{best}(c)$ -- is correct arithmetic and breaks the
expansion.

The expansion walks member index tuples through a priority queue, and stops
early on two invariants, both stated at `source/dfs-output.cpp:143-146` and
`source/dfs-output.cpp:161-166`:

> Since pending is score ordered and descendants cannot improve on their parent,
> an authoritative cutoff ends this expansion.

A descendant increments one member index. Under raw-count deltas that always
lowers the score, because members are count-sorted -- the invariant holds by
construction. Under correct per-member scores it does not: a rarer multi-word
member at index 3 can outscore the single-word member at index 1 by $B$. Both
early breaks, and the `next.log_score > published` filter at
`source/dfs-output.cpp:202`, would then discard spellings that belong in the
output.

**The invariant the expansion actually needs is that member order is
score-descending, not count-descending.** Sort members by $s(m)$ in
`member_order()` and everything lines up at once: member 0 becomes the best
member, $D(c)$ becomes 0 by definition, the delta $s(m_j) - s(m_0)$ is
non-positive and monotone, and `bonus_reorders` in `source/dfs-search.cpp:208`
has nothing left to scan for.

The cost is that phase 1 would need the score model at construction time. It is
a CLI argument, so this is a plumbing question rather than an ordering problem.
Two things read member order expecting counts and would have to be revisited:
`DfsClassRecord::members` is documented as "highest count first"
(`source/dfs-class-list.h:54`), and query-index re-sorts survivors itself
(below).

## What a pair bonus adds on top

A `--pair-bonus M` over a loaded pair set, applied to entries whose whole text
is in the set, is a second additive term:

$$
s(m) = \log \operatorname{count}(m)
+ B \cdot [\,\text{multi-word}\,]
+ P \cdot [\,\text{in pair set}\,],
\qquad P = M \cdot \log(10^6)
$$

Everything in the sections above applies unchanged, with $D(c)$ now measuring
both lifts. The specific reason it matters more here than for `--word-bonus`:

**A known pair is almost never member 0.** The pair list selects for phrases
that are interesting, not phrases that are frequent, so a pair member is a rarer
spelling than its class's top single word essentially by construction. It is
therefore always in the $j \neq 0$ case, where the cancellation of the previous
section applies in full. The bonus would raise the class's phase-2 bound, cause
the class to be explored more, and then vanish from the score of the one
spelling it was meant to promote. **Observationally the flag would look like it
does nothing to dfs-anagrams' rankings.**

Three further places assume a single bonus:

1. **`bonus_reorders`** (`source/dfs-search.cpp:208`) tests
   `multi_word_log_bonus() != 0.0`. With a pair bonus the class-best scan must
   run when *either* term is nonzero. Left alone, `--word-bonus 0 --pair-bonus 1`
   silently keeps member 0 as the class bound and the pair bonus never reaches
   phase 2 at all.

2. **query-index's two-group merge** (`source/query-index.cpp:378-404`) rests on
   "a constant bonus is order-preserving within each group", partitioning by
   `is_phrase` into exactly two count-sorted runs and merging them by score. A
   pair bonus makes three groups -- single word, phrase, phrase-in-pair-set --
   and the two-way partition would emit them out of score order. The `-n` cutoff
   is applied per group before the merge, so this is not only a display
   reordering: it drops rows that belong in the top N.

3. **query-index's count fast path** (`source/query-index.cpp:378`, and the
   comment at `source/query-index.cpp:171-175`) switches on
   `word_bonus == 0.0` to print raw counts and sort by count. That test would
   have to become "no bonus of any kind is active", or a `--pair-bonus`-only run
   prints counts in count order and ignores the flag.

### Smaller mechanical points

- `DfsPackedMember` has a named `uint16_t reserved`
  (`source/dfs-class-list.h:41-47`) and `IntermediateMember` likewise
  (`source/dfs-class-list.cpp:29-36`), so a membership flag costs no bytes and
  both static asserts on record size hold.
- The lookup belongs in `DfsExtractor::emit()`
  (`source/dfs-class-list.cpp:252-286`), which already holds the full entry text
  with its trailing space. `emit()` is on the hottest path in phase 1, so the
  set probe wants to be gated on `word_count > 1` -- the overwhelming majority
  of emissions are single words that cannot match a two-word key.
- Stored text is not NUL-terminated. A probe against
  `std::unordered_set<std::string>` therefore constructs a temporary per lookup;
  a heterogeneous or `string_view`-keyed set avoids that.
- `same_member()` and `member_order()` (`source/dfs-class-list.cpp:345-356`) need
  no new field: the flag is a function of the text, so equal-text members always
  agree on it.
- query-index's `--score` mode derives multi-word-ness from the user's own text
  with `find(' ')` (`source/query-index.cpp:256-260`) and would need the same
  set probe to stay consistent with what dfs-anagrams computes for the same
  sequence.
- Repurposing `--pairs` from its `-x 2` shorthand leaves
  `dfs_finalize_common_args()` (`source/dfs-cli-args.cpp:246-256`) with nothing
  to reconcile and `DFS_PAIRS_LIMIT` (`source/dfs-cli-args.h:19`) unreferenced.

## Summary

| | `--word-bonus` today | with a `--pair-bonus` added |
|---|---|---|
| phase-2 class bound | correct | correct only if `bonus_reorders` widens |
| within-class member order | bonus cancels; ranked by count | same, and pair members are always the affected case |
| cross-class comparison | inflated by $D(c)$ per class | inflated by more |
| `--segments` output | inherits the spelling scores | same |
| query-index single entries | correct | wrong: two-group merge, count fast path |
| at bonus 0 | exact | exact |

The order that follows from this: member ordering by score, then the phase-3
delta against member 0, then a second bonus term. Adding the term first lands it
on the one path that cancels it.
