# Plan: Phase 2 — node-ordered segments

Narrow implementation plan for the canonical-ordering constraint only. Context,
option analysis, and the `-x/--max-words` work live in
`plans/trie-node-ordered-permutations.md` and
`findings/reduce-permutations.md`; this document assumes those decisions and
does not re-argue them.

Phase 2 is independent of Phase 1 — neither blocks the other — though the
measurements are easier to read if Phase 1 lands first.

## Scope

Stop `SearchDriver` from exploring the same multiset of segments in more than
one order, by requiring segment trie-node IDs along a path to be non-decreasing.

A **segment** is the string consumed between two restarts: usually one word, but
a contiguous corpus phrase (`pen built` at `scale == 1.0`) is a single segment
spanning two words. Ordering is constrained *between* segments only.

## The invariant

> Along any path, the trie node IDs of successfully identified completed
> segments are non-decreasing.

Soundness: every multiset of segments has exactly one non-decreasing
arrangement, so no word set is lost — only its redundant orderings. `>=` rather
than `>` keeps repeated words legal.

This works because a trie node offset uniquely identifies the string from root
to that node, so it is a total order on segments. It does not need to be
lexical, and it isn't.

Two segments cannot be identified this way: **leaf nodes**, which all report
`choice.next == (off_t)-1`. Handling is specified below; the reasoning is in
`plans/trie-node-ordered-permutations.md`.

## Critical detail: there are TWO check sites, not one

This is the part most likely to be got wrong, because the obvious
implementation silently under-prunes by a factor of k.

The restart block at `search-driver.cpp:76-86` is where a segment ends *and
another begins*. But the **final** segment of a solution never restarts — the
result is emitted from the `is_accepting` branch at `search-driver.cpp:58-74`,
and for the anagram filter the terminal state has no outgoing transitions
(`find-anagrams.cpp:62`), so the restart push that follows it is dead.

Checking only at the restart therefore constrains segments 1..k-1 and leaves the
last one free. Surviving arrangements would be **k**, not 1: any element may be
last, with the rest sorted. For k=6 that is 720 → 6 instead of 720 → 1.

So the check must happen at both sites:

| Site | `search-driver.cpp` | Completed segment ID | On failure |
|---|---|---|---|
| Restart push | 76-86 | `next.choice.next` | skip the restart push |
| Accepting/emit | 58-74 | `next.choice.next` | skip the emit, fall through |

At both sites `next.choice.ch == ' '` and `next.choice.next` is the node reached
*after* consuming that trailing space — the completed segment's identity.

## Data structure changes

### `source/search.h`

Add to `Next`:

```c
IndexReader::Node last_seg;  // node ID of the last identified completed
                             // segment on this path; 0 = none yet
```

`0` is a safe "none" sentinel: `IndexWriter::write` assigns `out.pos` only after
writing at least one byte, so no real node has offset 0.

`Next` goes 40 → 48 bytes. Update the `static_assert` at `search.h:38`
accordingly (or drop it in favour of an explicit `sizeof(Next) == 48`).

Add a constructor parameter, defaulted off so `find-expr` is untouched:

```c
SearchDriver(const IndexReader*, const SearchFilter*,
             SearchFilter::State start, double restart,
             bool canonical_order = false);
```

Store it as a `bool canonical` member.

### `source/search-driver.cpp` — constructor

```c
seed.last_seg = 0;
```

### `source/search-driver.cpp` — `step()`

**Inheritance.** `new_next` is initialised once before the `tmp` loop
(lines 38-40). Add there:

```c
new_next.last_seg = next.last_seg;
```

Every character-transition child inherits the path's constraint unchanged. This
is also what makes the restart push correct by default — it must *not* be
treated as consuming a new space.

**Emit site**, inside `if (filter->is_accepting(...))` at line 58, before the
string reconstruction:

```c
if (canonical && next.choice.ch == ' ' &&
    next.choice.next != (off_t) -1 &&
    next.choice.next < next.last_seg)
  goto no_emit;   // or restructure so the branch is skipped
```

Guarding on `ch == ' '` matters: `is_accepting` can fire mid-word for other
filters, and only a space marks a segment boundary.

On failure, do **not** insert into `seen` — the canonical arrangement of the
same word set is a *different string* and must still be free to be emitted
later.

**Restart site**, at line 76:

```c
IndexReader::Node completed = next.choice.next;
bool identified = (completed != (off_t) -1);

if (canonical && identified && completed < next.last_seg)
  ;  // out of order: do not start a new segment here
else if (restart > 0.0 && next.choice.ch == ' ' &&
         next.choice.next != reader->root()) {
  ...existing body...
  new_next.last_seg = identified ? completed : next.last_seg;
  nexts.push(new_next);
}
```

Note `new_next.last_seg` is assigned *after* the existing body, overwriting the
inherited value — the restart is the one place where the constraint advances.

Suppressing the restart push does not kill the path: the space-child pushed in
the `tmp` loop still carries it forward contiguously. Only *starting a new
segment* out of order is forbidden.

## Leaf handling

When `completed == (off_t) -1` the segment cannot be identified. **Carry the
previous `last_seg` forward unchanged** rather than storing `-1`.

Storing `-1` would satisfy every subsequent comparison and abandon ordering for
the remainder of the path. Carrying forward keeps constraining against the last
*identifiable* segment, so non-leaf segments still collapse to one order with
leaves free to sit anywhere. Still sound — for any word set, the arrangement
with non-leaf segments in node order and leaves interleaved anywhere passes
every check.

## `source/find-anagrams.cpp`

- Add `-c/--canonical-order` (`OPTPARSE_NONE`) to `long_options`, a
  `bool canonical_order` to `Args` defaulting to `false`, a case in the switch,
  and the flag to `usage()`.
- Pass it as the new `SearchDriver` argument at line 241.

## Verification

**Soundness — the test that matters.** On 8- and 12-letter bags, every word
*set* found without the flag must still be found with it. Arrangements and
scores may differ; missing sets are a bug.

```bash
run() { ./build/find-anagrams idx/wiki-merged.5.index "$1" ${2:-} 2>/dev/null \
        | sed 's/^[^ ]* //' | tr ' ' '\n' | sort | tr '\n' ' ' ; }
# per line: strip score, sort words, compare the two sorted sets of sets
```

Do this per result line, not over the whole stream. Run both to completion on a
bag small enough to terminate (8 letters completes immediately per
`findings/anagram-perf.md`).

**Under-pruning check.** Count distinct word sets vs total results emitted. With
both check sites wired up the ratio should be ~1:1. If it lands near 1:k for
k-word results, the emit-site check is missing or misfiring — this is the
failure mode the two-check-site section exists to prevent.

**Regression.** `find-expr` output must be byte-identical with the flag off.
Run `test-expr`.

**Effect.** Peak RSS (`/usr/bin/time -v`) and `nexts.size()` at matched step
counts, with and without the flag, at 12/16/20 letters.

**Leaf frequency.** Count how often the restart site sees `-1`. It bounds the
achievable pruning and is the input to the parent plan's Phase 3 decision.

## Expected outcome

Per segmentation, k! orderings collapse to 1. The realised ratio will be lower
because:

- Leaf segments are unconstrained relative to each other.
- Dedup is per-segmentation: `{A,B,C}` as three segments and as `(A B)(C)` are
  different segment multisets and are not deduped against each other. This is
  intended — they score differently and the contiguous one is wanted.

## Risks and edge cases

- **The kept arrangement may not be the best-scoring one.** For pure-restart
  paths the score is order-independent (`pilot but when` and `but when pilot`
  tie exactly), so nothing is lost. The loss is confined to contiguous phrases:
  `pen built` scores 7 while `built pen` scores 2.1e-05. Hence the flag, and
  hence off by default.
- **A word set is never lost outright**, because the anagram filter accepts any
  arrangement and every single word is reachable from the root, so the
  non-decreasing arrangement always exists as a search path.
- **Repeated words**: `>=` admits them; `>` would silently drop every anagram
  using the same word twice.
- **`Next` grows 20%**, paid by `find-expr` too. Deferred to Phase 4 of the
  parent plan, which takes it back to ~32 bytes via `float` log-score and
  `int32` count.
- **Node identity is an emergent property**, not an enforced one: it holds
  because `IndexWriter` never coalesces identical subtrees. True today
  (streamed trie, offsets are file positions), asserted nowhere. Leave a comment
  at the point of use recording the dependency.

## Rejected alternatives

- **Deriving the previous segment ID from crumbs instead of storing it.** Saves
  the 8 bytes, but requires re-walking the trie from the root through the
  reconstructed characters at every restart, paying a `children()` decode per
  character. Trades the memory win for exactly the CPU the design was supposed
  to leave alone.
- **A side table mapping `Node` → dense `uint32` id.** Any injective mapping
  induces a valid total order, so it would be sound, and it would fit the
  ~6 bytes Phase 1's flattening frees. But it needs a hash map that grows with
  distinct segments seen — reintroducing the unbounded memory cost this whole
  approach exists to avoid.
