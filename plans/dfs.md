# Plan: `dfs-anagrams` — approach A (word extraction + collapsing DFS)

## Why this plan, and why now

`findings/ancc-inspiration-summary.md` §9 item 2 made the whole architecture wait
on one measurement: **F**, the cost of keeping contiguous corpus phrases in a
word-list search. §9's rule was F ≲ 1.5 → build A, F ≳ 2 → build B.
`findings/phrase-recovery-cost.md` measured it: **F = 1.12–1.80**, and its §3
shows even that overstates the real cost, because the classes phrases add are all
≥ `2*min_len` letters and so cannot be picked at the depths where a four-word
solution actually lives (4.75 letters/word at 19 letters). That selects **A**.

Approach A is the only design that gets anagram collapsing (the 306x, worth ~+4.9
letters — more than a fully tuned 20-core build at +3.5), and with phrases
recovered it is a strict superset of today's tool rather than a trade. This plan
builds it, **single-threaded**, with **no `--max-words` switch** (the word cap is
derived from `-m`, exactly as `find-anagrams` already does).

This document does not re-argue A vs B; `ancc-inspiration-summary.md` §8–§9 does
that. It is the construction plan.

## Settled decisions

Locked in with the user; not open for re-litigation here:

1. **New binary, `dfs-anagrams`.** `find-anagrams` (best-first, streaming) stays
   exactly as it is, so the two can be diffed side by side and today's streaming
   behaviour remains available. A is a different product — two-phase, completes
   then emits — not a refactor of the existing engine.
2. **Default output: top-N spellings.** An exact global ranking over the full set
   of spellings (see §6 of the summary), not top-N-solutions-fully-expanded.
3. **Segmentation collisions are deduped inside the top-N heap, keyed by the
   sorted word set, keeping the higher score.** This reproduces today's `seen`
   behaviour (`search-driver.cpp:make_seen_key`): `{pen built}` as one contiguous
   phrase (score 7) and `{pen}{built}` as two segments (2.1e-05) share a word set
   and collapse to the 7. Memory stays bounded by N — a global dedup set over all
   solutions (29.3M at 19 letters, more at 21; `phrase-recovery-cost.md` §3.5)
   would reintroduce the unbounded memory A exists to remove.
4. **Single-threaded.** Parallelising the depth-1 branches is a mechanical ~12x
   (summary §8) and is explicitly deferred — it changes no semantics and should
   be the last thing done, if at all.
5. **No `--max-words` flag.** The word cap is `floor(letters / min_word_len)`,
   which `AnagramFilter` already enforces and `find-anagrams` already derives
   (`find-anagrams.cpp:291`). See the caveat in §"Word cap" below — this is the
   one place the plan knowingly diverges from the summary's §8 ceiling rows.

## What already exists and is reused

- **`IndexReader`** (`source/index.h`, `index-reader.cpp`): the trie, the bag
  filter via `children(node, count, CharSet, out)`, `root()`, `count()` (corpus
  total). Phase 1 is a DFS over this.
- **`source/measure-f.cpp`** is a **working prototype of phase 1**, phrases
  included. Its `Extractor` class walks the index under the bag constraint and
  emits every segment (words at `-x 1`, contiguous phrases beyond). Its word-only
  pass reproduces every phase-1 figure in `ancc-inspiration.md` exactly, so the
  extractor is trustworthy. `dfs-anagrams`' phase 1 is a productionised
  `Extractor`. `measure-f` is measurement-only and stays uninstalled; do not
  delete it — Phase 0 below extends it.
- **The score model** is confirmed end to end (summary "What is measured", and
  `search-driver.cpp:step()`): a segment scores the count of the trie node it
  ends on; k segments score `∏count_i × (restart/total)^(k-1)` because each
  restart multiplies by `restart` and re-seeds the count at `total`, which the
  next segment divides back out. `restart` is `1e-6`, `total` is
  `reader->count()` (3,586,472,603 on the wiki index). `measure-f.cpp:210-271`
  already implements this scoring in log space; reuse it verbatim.

## Architecture

Two phases, no frontier anywhere:

```
phase 1  trie DFS under bag filter  ->  entry list (words + contiguous phrases)
                                        grouped into anagram classes
phase 2  bag-subtraction DFS over    ->  solutions (ordered lists of classes)
         classes, rarest-letter          in canonical order, no permutation
         forcing + collapsing            dupes (segmentation variants stay
                                         distinct, collapse at output)
output   lazy per-solution spelling  ->  bounded top-N heap over spellings,
         expansion in score order         deduped by sorted word set
```

Peak memory is the entry list (bounded by the a–z dictionary, tens of MB) plus
the DFS stack (kilobytes) plus the N-entry heap — flat regardless of bag length,
runtime, or solution count.

## Phase 0 — the phase-2 node count, as a de-risking measurement first

**Done. Result: phrase cost is 1.04x nodes at 19 letters — A proceeds.** Details
and the corrected counts are in `findings/phrase-recovery-cost.md` §3.5. What the
measurement was, and the one surprise it turned up:

**This was the opening move, per summary §9 item 3's sub-bullet.** Before building
any output machinery, settle the one number that could still send us to B: does
the collapsing DFS over the *phrase-included* class list actually cost what §3 of
`phrase-recovery-cost.md` predicts, or worse?

- Extended `measure-f` with ancc's `check_dict` DFS (`CollapseDFS`) over the class
  list it already builds — a node/solution counter, plus an independent reference
  enumerator (`-R`) and a word dump (`-W`) for validation.
- **The documented target was wrong.** `ancc-inspiration.md` said words-only at 19
  letters should reproduce 5,488,296 nodes / 156,138 solutions; at 14 letters,
  1,315 solutions. Neither reproduces. The real counts are ~12–190x larger, and
  the DFS was instead validated **four independent ways against `ancc` itself**:
  the forced-letter DFS, the reference enumerator, and the real `ancc` binary on
  the identical trie word list all agree (27,177 solutions at 14 letters, 123 on a
  9-letter cross-check). The recorded targets came from a buggy/differently-scoped
  earlier prototype — do not use them as a correctness gate. Real figures: 27,177
  sol / 53,084 nodes at 14 letters; 29.3M sol / 68.2M nodes / 89 s at 19 letters.
- Ran it with phrases included: the node multiplier is **1.04x at 19 letters**
  (operational cap 4), and it *shrinks* with depth (1.76x/1.25x/1.04x at caps
  2/3/4) — phrase classes are ≥8 letters and can't be placed deep in the search.
  §3 predicted "between 1.0x and 4.4x, much closer to 1.0x"; measured, it is even
  below that.
- **Decision gate — passed.** 1.04x is far under the >6x "reconsider B" threshold.
  Proceed with A. Separately, the corrected (much larger) absolute counts moved
  the summary §8 ceilings down ~3 letters and enlarged the output-stage volume
  budget ~190x — flagged for those sections, no change to the architecture.

Deliverable: done — corrected node/solution counts and the multiplier are in
`findings/phrase-recovery-cost.md` §3.5, and summary §8 has been re-anchored.

## Phase 1 — extraction into a packed class list

Productionise `measure-f`'s `Extractor`. Output is an in-memory structure phase 2
searches:

### Entries and classes

- An **entry** is one extracted segment: its spelling text (a word like `yacht`,
  or a phrase like `pen built` with its internal space), its **count** (the trie
  node count at the terminating space — its corpus score), and its **word count**
  (1 for a word, ≥2 for a phrase; needed only for the sorted-word-set dedup key).
- An entry's **class** is the sorted multiset of its letters, spaces dropped
  (`measure-f.cpp:class_key`). `yacht` and `cathy` share class `achty`;
  `pen built` has class `belnptu`.
- Group entries by class. Within a class, keep **members sorted by count
  descending** (this ordering is what makes lazy expansion in §output cheap — do
  it once here). A member is a distinct spelling; two entries with equal spelling
  and count are one member.
- Also build the **per-letter candidate index** phase 2's forcing needs: a map
  from each letter to the classes containing it. ancc's T2 keys this on each
  class's *own* rarest letter (`let_hash[]`), so a class lands in exactly one
  bucket and "the classes covering L" is an O(1) lookup rather than a scan of the
  whole class list at every node. Without this, rarest-letter forcing degrades to
  a linear sweep per DFS node and the 306x evaporates. This is a phase-1 output,
  not something phase 2 can reconstruct cheaply.
  - Note the build-order dependency: "rarest" is defined by the priority order
    (phase 2, §"The search"), and that order is itself computed from these
    classes' letter frequencies. So phase 1 runs in passes — extract and group
    into classes, tally letter frequencies to fix the priority order, *then* key
    each class into the index by its rarest letter under that order.

### Representation

Correctness first, packing second. Start with whatever `measure-f` uses
(`unordered_map<string, ...>`), get the pipeline correct, then pack only if RSS
matters. The summary (§2) prices the packed form at ~5 MB for 144k words and
~23 MB for the whole a–z dictionary — a 16-byte-mask-per-class array plus inline
member characters — versus 220 MB stored carelessly. The packed form is an
**optimisation, not a precondition**: 220 MB is already flat and fine to ship
with. Note it as a follow-up, don't block on it.

Phase 1 cost is not the bottleneck (summary §2, `phrase-recovery-cost.md` §5):
multi-segment extraction for all 26 letters is under 3 s and ~3.9x the nodes of
the word-only walk. Nothing to optimise here.

### Phrases are on by default

Phase 1 walks *past* the first space (`Extractor` with `max_words > 1`), because
adjacency is a requirement, not an option (summary §5). There is **no phrase
pruning** — `phrase-recovery-cost.md` §4 proved the proposed mitigation is a
tautology (every one of 462,730 measured phrases pays for itself by 6.7–7.3
orders of magnitude). Keep every phrase; F as measured is F.

## Phase 2 — collapsing bag-subtraction DFS

This is ancc's `check_dict`, techniques T2/T3/T5 from `ancc-inspiration.md`,
which the summary's approach C describes as mechanics. Over **classes**, not
spellings — that is the 306x.

### The search

- State: the remaining letter bag (a small per-letter count array — up to 36
  distinct symbols, since `a`–`z` *and* `0`–`9` are bag characters, matching
  `measure-f`'s `is_bag_char`) and the depth. Stack depth is O(word cap); the
  whole live state is kilobytes.
- **Rarest-letter forcing (T2, = approach C).** Fix a global letter priority
  order, computed once. At each node, L = the highest-priority letter still in
  the bag; only classes *containing L* are candidates (via the per-letter index
  from phase 1). This is both fail-first pruning (the hardest letter is resolved
  at depth 1, where the candidate list is tiny) and **free canonicalisation**:
  the class covering L is forced now, so the k! orderings of a solution collapse
  to 1 with no dedup structure at all. This is why the prototype emitted zero
  duplicate word sets at 12 letters (summary "measured").
  - **Any fixed total order on letters is correct** — canonicalisation only needs
    the order to be consistent, not meaningful. The *choice* of order is purely a
    fail-first performance lever: put the letter that constrains hardest first.
    ancc uses least-frequent-**in-dictionary** (over the extracted list), which
    prunes better than raw corpus frequency because it reflects what phase 1
    actually found. Start with dictionary frequency; it is cheap to compute from
    the class list and needs no correctness argument.
- **`entry_point` tie-break.** When L occurs more than once in the bag the choice
  is ambiguous; require non-decreasing class index *among classes covering the
  same forced letter* to keep one arrangement. This is the same ordering `-c`
  uses, restricted to the ambiguous case — see the `out_of_order` /
  forced-letter-advance discussion in `ancc-inspiration.md` §C. Because forcing
  supplies the canonical order structurally, **`dfs-anagrams` has no `-c` flag**;
  it is always on and always free.
- **Subset test and subtraction** per candidate class: it fits if every letter's
  count is ≤ the bag's. Subtract, recurse; on return, add back.
- **Depth is bounded emergently, not by a counter.** Every extracted class is
  ≥ `min_len` letters (≥ `2*min_len` for phrases), so after k segments the bag
  has lost ≥ `k·min_len` and no path can exceed `floor(letters / min_len)`
  segments — the cap falls out of the extraction and never needs checking. This
  mirrors `find-anagrams`, which computes `max_words` for its header line but
  does **not** pass it to the driver (`find-anagrams.cpp:321`); the limit there
  is likewise emergent from `AnagramFilter`'s `min_len`. Keep an explicit guard
  if you like, but know it never binds — see §"Word cap" for why that matters at
  26 letters. A solution is complete when the bag is empty.
- A **solution** is the ordered list of classes chosen (canonical order). Phase 2
  emits solutions; it does not expand spellings — that is the output stage's job,
  done lazily.

### Scoring a solution for ranking

For pruning and for the output cutoff, a class needs a single representative
score. Use the class **maximum** member count (the first member, since members
are sorted descending) — this is admissible: it is the best any spelling of that
class can score, so it never under-estimates a solution's best spelling. A
solution's representative score is `∏max_count_i × (restart/total)^(k-1)`, in log
space to avoid underflow (`measure-f.cpp:229` shows the pattern). The *actual*
per-spelling score uses the chosen members' real counts, computed at expansion.

## Output — lazy expansion, top-N spellings, word-set dedup

### The volume problem this solves

A solution's spellings are the cross-product of its classes' members (~10^3-fold
at 19 letters). With the corrected solution count (29.3M at 19 letters, not the
retracted 156,138), writing them all is on the order of **~1.3 TB at 19 letters**
(summary §6, corrected). So expansion must stay lazy and bounded — the correction
makes this more essential, not less.

### The mechanism

The dedup is the subtle part, and it must not reintroduce unbounded memory —
that is the whole point of A, so the structure has to bound the *map* as tightly
as the heap.

- Maintain a **bounded top-N over word sets** as an **indexed min-heap**: each
  entry holds `(score, word-set-key)`, and a `key → heap-position` map lets us
  find an existing key in O(1) for the update case *and* erase a key when it is
  evicted. Both the heap and the map are therefore capped at N entries. N is a
  CLI flag (default e.g. 10,000 → ~1 MB including keys).
  - **A plain `unordered_map<key, score>` plus lazy heap deletion does not
    bound memory** — nothing would remove a map entry when its word set is
    pushed out of the top-N as the heap fills, so the map grows with the number
    of *distinct word sets ever briefly in the top N*, which over a
    tens-of-millions-of-solutions search (29.3M at 19 letters) is unbounded. The
    `key → position` index that erases on eviction is what closes this; do not
    skip it.
- The **word-set key** is built exactly like `make_seen_key`: split every segment
  of the spelling on spaces, sort the words, rejoin. This makes `{pen built}` and
  `{pen}{built}` collide on `built pen`, and — because A already collapses
  permutations — it otherwise leaves distinct member choices (`yacht` vs `cathy`)
  as distinct rows, which is what "top-N spellings" wants.
- **When phase 2 emits a solution**, generate its spellings **in descending score
  order on demand** — the standard k-best-combination heap over member-index
  tuples, cheap because each class's members are already sorted descending
  (Phase 1). Stop at the first spelling whose score is ≤ the current N-th best
  (the heap's min once full): every later spelling of this solution is worse, so
  none can enter. Typically one or two spellings per solution are touched.
- **Insertion with dedup**, for a candidate spelling with key k and score s
  (heap not yet full → treat the floor as −∞):
  - if k not in the map and s > floor → insert; if the heap was full, evict its
    min first and erase that key from the map;
  - if k in the map with stored score ≥ s → skip (a better arrangement is held);
  - if k in the map with stored score < s → a higher-scoring segmentation of the
    same word set arrived (the phrase form after the split, or vice versa):
    **increase-key in place** at its heap position and update the map. No new
    slot is consumed, so this never grows past N.
- After the search completes, drain the heap into a sorted list and print
  `score text` descending, matching `find-anagrams`' output format
  (`search-printer.cpp` / `PrintAll`).

The cutoff is safe under cross-solution dedup: a spelling scoring ≤ floor can
only match a key already held at a score ≥ floor ≥ its own, so it would be
skipped regardless — nothing below the floor ever needs to enter.

### Why this is exact

Every spelling is reachable and none is preferred a priori; the heap holds the
true top N over the full spelling space, with segmentation duplicates collapsed
to their best score. This is the summary §6 "lazy exact expansion", specialised
to the top-N-spellings output the user chose.

## Word cap — the one knowing divergence from §8

Summary §8's "~25–26 letters" A rows assume a **hard 4-word cap**. With no
`--max-words` switch, `dfs-anagrams` uses the derived cap
`floor(letters / min_word_len)`, which at 26 letters with `-m 4` is **6, not 4**
(`find-anagrams.cpp:281-293` documents exactly this). Two extra levels at a
branching factor in the thousands is not a rounding error, so **long bags will
run deeper and slower than §8's rows predict**. This is the accepted consequence
of "no `--max-words` for now"; the summary's ceilings are for the capped search.
If the uncapped depth proves painful in practice, adding the switch later is
mechanical (the depth cap already exists in phase 2). Flagged here so a future
measurement against §8 isn't read as a regression.

## Memory budget

```
entry / class list   <= ~50 MB packed (bounded by the dictionary, NOT the bag);
                        ~220 MB if stored carelessly to start.  Flat.
phase-2 DFS stack    kilobytes (O(word cap))
top-N heap + dedup   N x (~48 B spelling + key) ; N=10k -> ~1 MB
```

No frontier. Nothing grows with runtime or solution count. This is the entire
point of the design (summary §8 "memory ceases to be the constraint").

## CLI and wiring

- New executable `dfs-anagrams` in `source/meson.build`, linked against
  `optparse_lib` and `index_lib` (it does **not** need `search_lib` — it uses
  neither `SearchDriver` nor `AnagramFilter`; the score model is reimplemented
  from counts). Mirror the `measure-f` executable stanza, but `install: true`.
- Flags, reusing `optparse` and matching `find-anagrams` where they overlap:
  - positional `input.index letters`
  - `-u/--used-letters` (subtract letters already placed), same semantics as
    `find-anagrams`
  - `-m/--min-word-length`, default 4 (`DEFAULT_MIN_WORD_LEN`); the word cap is
    derived from it, not a flag
  - `-n/--top` N, the heap size / number of results (new; default e.g. 10000)
  - `-p/--progress-factor` for progress lines on stderr, same convention as
    `find-anagrams` (`# ` prefix so one filter drops them)
  - **not** `-c` (canonicalisation is structural and always on) and **not** `-x`
- Emit a `# ...` header line like `find-anagrams.cpp:310` stating the bag, `-m`,
  and the derived word cap, since `-m` has a default and the search run is not
  always what the command line spells out.
- Progress: report phase-1 word/class counts, then phase-2 node count and
  solutions found, on stderr.

## Verification

- **Soundness against `find-anagrams`.** On small bags (8, 12 letters) that
  `find-anagrams` runs to completion, both tools are exhaustive over the same
  trie and phrases at the same `min_len` and both dedup by word set, so the sets
  of word *sets* must be **equal** — a missing *or* extra set is a bug. Compare
  via the sorted word-set key. (On large bags the claim weakens to superset,
  because `find-anagrams` OOMs before finishing and A reaches sets it never did.)
- **Score agreement.** For a shared word set, `dfs-anagrams`' score must match
  `find-anagrams`' to the float tolerance both already carry. In particular
  reproduce `7.000 pen built` (contiguous) and the 2.1e-05 split, per
  `measure-f -d` and `find-anagrams` itself.
- **Zero duplicate word sets, words-only** (the forced-letter property). Run with
  phrases *off* and confirm the solution count equals the distinct-word-set count
  exactly — Phase 0 verified this holds (forced-letter DFS == the `-R` reference
  enumerator == `ancc`, no dupes). Do not anchor on the summary's recorded 35,041
  figure at 12 letters; it is from the same buggy prototype whose counts Phase 0
  retracted. With phrases *on*, phase 2 legitimately emits two solutions for one
  word set (`{pen built}` vs `{pen}{built}`), which are distinct class-sequences
  with distinct scores and are collapsed at *output*, not in phase 2 — so the
  phase-2 solution count then exceeds the distinct-word-set count by design, and
  only the final printed output is one row per word set.
- **Node/solution-count target** from Phase 0 (words-only, `-m 4`, emergent cap):
  **27,177 sol / 53,084 nodes at 14 letters; 29.3M sol / 68.2M nodes at 19
  letters** (`phrase-recovery-cost.md` §3.5). These are the real, ancc-verified
  counts — *not* the retracted 156,138 / 5,488,296.
- **Flat memory**: peak RSS at 14 / 19 / 21 letters should track the prototype's
  93 / 177 / 220 MB (careless) and not grow with N or runtime.
- **Exhaustive completion**: at 19 letters the emergent cap `floor(19/4) = 4`, so
  the words-only run should reproduce Phase 0's **29.3M solutions / 68.2M nodes /
  ~89 s** on the untuned prototype (phrases add ~4%). At 21 the emergent cap is
  `floor(21/4) = 5`, one deeper, so expect counts well *above* those — that is the
  §"Word cap" divergence, not a regression. (The old ~3.25 s / 156,138 and
  ~37 s / 1.47M figures were the retracted undercounts; ignore them.)

## Build order

1. ~~**Phase 0** — DFS node count in `measure-f`, decision gate on B.~~ **Done:
   phrase cost 1.04x at 19 letters → A proceeds. The DFS was validated against
   `ancc` directly (the documented 5,488,296 target was a buggy undercount, now
   retracted). See `phrase-recovery-cost.md` §3.5.**
2. **Phase 1** — productionise `Extractor` into the class list with
   count-descending members. Reuse `measure-f`'s scoring.
3. **Phase 2** — the collapsing forced-letter DFS emitting solutions. This is
   the code Phase 0 prototyped, cleaned up and producing solutions rather than
   just counting nodes.
4. **Output** — lazy expansion, top-N heap, word-set dedup, printing.
5. **CLI + wiring** — the `dfs-anagrams` binary, flags, header, progress.
6. **Verification** — the checks above.

## Explicitly out of scope (deferred, not forgotten)

- **`--max-words`** — the summary's capped §8 ceilings need it; derived cap for
  now (§"Word cap").
- **Parallelising the DFS** — mechanical ~12x over depth-1 branches, no semantic
  change, do it last if at all (summary §9 item 5). Single-threaded now.
- **The `h` pruning bound** (summary §7) — the only lever with no ceiling, and it
  drops in cheaply here (carry accumulated log-score `g`, prune when
  `g + h(bag) < heap floor`; `h(bag) = Σ_c best_rate_containing[c]`). Worth a
  follow-up plan of its own; it is an accelerator, not a correctness requirement,
  and the top-N heap already supplies the `floor` it needs. Not in this plan.
- **Packing the class list** to ~5–23 MB — do it only if 220 MB flat proves
  annoying (summary §2).
- **Top-N-solutions-fully-expanded** output mode (summary §6) — the user chose
  top-N spellings; the other mode is the same lazy expansion with a different
  cutoff and can be added later behind a flag.

## Related

- `findings/ancc-inspiration-summary.md` — the digest that selects A; §2 (design
  A), §5 (phrases required), §6 (lazy expansion / output modes), §7 (`h`), §8
  (ceilings), §9 (order of work).
- `findings/phrase-recovery-cost.md` — the F measurement that unblocked A; §3 is
  the node-count gap Phase 0 closes.
- `findings/ancc-inspiration.md` — ancc's techniques T1–T7 and the designs in
  full; §C is the forced-letter / `entry_point` mechanics phase 2 implements.
- `source/measure-f.cpp` — the working phase-1 prototype and score model this
  plan productionises.
- `source/search-driver.cpp` — `step()` (the score model), `make_seen_key` (the
  dedup key), `out_of_order` (the canonical-order tie-break phase 2 mirrors).
