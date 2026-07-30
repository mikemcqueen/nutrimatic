# Reducing phase-1 class-list memory

## Problem

`wiki-merged.5.index` contains roughly 110 million entries. With a low minimum
word length such as `-m 2`, phase 1 can extract a substantial fraction of
them. At even 100 bytes per retained entry, 100 million entries require 10 GB.
That estimate is plausible and may be low for the current representation.

The important distinction is that phase 2 does **not** need all spelling
records in its hot lookup structure. Phase 2 searches anagram classes and needs
only:

- the class's letter requirements;
- its length and support mask;
- its best member score;
- its score-key delta;
- its rarest-letter bucket and class ID.

The remaining spelling records are cold phase-3 data used to expand a complete
class path into concrete output.

## What the current implementation stores

`DfsExtractor` currently groups every emitted spelling in:

```cpp
std::unordered_map<
    std::string,
    std::vector<DfsClassMember>
> grouped;
```

The map key is constructed by copying an emitted spelling, removing spaces,
sorting its letters, and hashing the resulting string. Each member contains:

```cpp
struct DfsClassMember {
  std::string text;
  int64_t count;
  int word_count;
};
```

On a typical 64-bit libstdc++ ABI, that record is likely 48 bytes before:

- separately allocated spelling characters beyond the small-string limit;
- vector unused capacity;
- allocator metadata and alignment;
- the hash-map node, buckets, and separately allocated class-key characters;
- the containing `DfsAnagramClass`, its key, and its `letters` allocation.

At large scale, the per-object allocations and vector growth are as important
as the nominal member size. A 10 GB estimate for 100 million phrase-heavy
members is therefore credible.

After construction, `DfsAnagramSearch::prepare_hot_classes()` already copies
the phase-2 data into a compact representation. `FitClass` is 16 bytes and is
supplemented by contiguous score-delta, wildcard-length, score, and packed
requirement arrays. The rich class objects remain alive because
`DfsTopN::emit()` later reads their member strings and counts.

This means the large memory term is principally a cold spelling store, not the
phase-2 lookup table.

## Recommendation 1: separate hot classes from cold spellings

Build a phase-2-oriented class representation directly instead of retaining a
rich object graph and then copying it:

```cpp
struct HotClass {
  uint64_t support_mask;
  uint64_t score_key_delta;
  double best_member_log_score;
  uint32_t requirements_offset;
  uint32_t packed_length_and_count;
  uint32_t members_begin;
  uint32_t members_count;
};
```

The exact layout should be selected after measuring cache behavior; the
important property is that phase 2 traverses only contiguous fixed-width
records and packed letter requirements. Strings, member records, and class-key
strings must not be interleaved with this hot data.

Store cold members in flat arrays rather than one `std::string` in one vector
element per entry:

```cpp
struct PackedMember {
  uint64_t count;
  uint32_t text_offset;
  uint16_t text_length;
  uint8_t word_count;
  uint8_t flags;
};
```

This example is 16 bytes per member. Text lives in one contiguous byte arena.
Large stores may need block-relative offsets or 64-bit offsets, but those can
be confined to block metadata instead of making every member larger.

Even without discarding any entries, this should reduce the anonymous-memory
cost substantially while preserving sequential phase-2 access.

## Recommendation 2: use numeric class signatures

An anagram class is a subbag of the input bag. It does not need a sorted string
as its build-time identity.

Given input counts `limit[symbol]`, assign each symbol a mixed-radix
multiplier:

```text
multiplier[0] = 1
multiplier[s] = product(limit[0..s-1] + 1)

class_key = sum(used_count[s] * multiplier[s])
```

The extractor can update this key incrementally when it consumes and restores
a letter. Emission then performs an integer lookup without copying or sorting
the spelling.

For the current 47-letter `S6` value, the theoretical subbag count is:

```text
3,135,283,200
```

so a query-relative class signature fits in `uint32_t`. Other inputs should
use checked `uint64_t` arithmetic and fall back to a wider or packed signature
if their state product overflows.

A dense array indexed by all 3.1 billion possible states would itself be too
large. Use a preallocated open-addressed flat hash table:

```text
numeric class signature -> compact class-builder index
```

This removes:

- a sorted string allocation per emission;
- string hashing and comparison;
- node allocation per class;
- much of the pointer chasing during construction.

After phase 1, phase 2 does not need this hash table. Classes are reordered into
their rarest-letter buckets and addressed by compact class IDs.

## Recommendation 3: exactly cap members for finite top-N output

For a finite global result limit `N`, a class does not need more than its best
`N` distinct local word sets.

The qualification about word sets matters. Output deduplicates a complete
spelling by sorting all its constituent words. Two phrase spellings such as
`foo bar` and `bar foo` can therefore represent the same output key. Before
applying the cap, collapse members within each class by that same normalized
local word-multiset key, retaining its highest-scoring spelling and the
deterministic tie winner.

### Why the cap is exact

Suppose a complete result contains member `m` of class `C`, and `m` ranks below
the best `N` distinct local word sets of `C`. Hold every other selected member
fixed. Replacing `m` with each of the best `N` members produces:

- `N` strictly better scores; and
- `N` distinct global word multisets.

The second property follows because multiset union with a fixed multiset is
injective: `A + X = B + X` implies `A = B`.

Therefore the result containing `m` has at least `N` distinct better results
and cannot enter the global top `N`. The same argument holds when the class is
selected repeatedly: fix all other occurrences and replace just the occurrence
using `m`.

Members must be ranked by their actual segment score, including any multi-word
bonus, rather than by count alone.

This optimization is not available for `-n 0`, which requests every result and
therefore requires every distinct member word set.

The cap bounds members per class, not total memory. If most extracted entries
belong to distinct classes, class metadata will still be large. It is most
effective when the phrase-heavy extraction produces many spellings or phrase
orders for the same letter multiset.

## Recommendation 4: make the cold member store file-backed

If the packed cold members remain too large, write them to a custom temporary
file and mmap the completed store read-only for phase 3.

Phase 2 would retain only member ranges. It would not touch member pages.
Phase 3 would fault in only the member blocks belonging to promising complete
solutions. Unlike anonymous heap allocations, untouched file-backed pages do
not consume process RSS and touched pages are reclaimable by the kernel.

A simple sequential binary representation should outperform a general-purpose
database:

```text
class table:
    member-block offset
    member count

member block:
    score-ordered packed member descriptors
    spelling bytes, optionally block-compressed
```

For bounded memory during construction, partition emitted records by high bits
of the numeric class signature. Process one partition at a time, collapse and
sort its classes, and append the final blocks. This is an external radix
grouping operation:

1. Traverse the source trie once.
2. Append compact emission descriptors to partition files.
3. Reduce each partition in bounded RAM.
4. Write hot class metadata and cold member blocks.
5. Sort the much smaller class table into phase-2 rarest-letter order.

The spelling blob can remain in original emission order with descriptors
pointing into it, or be rewritten into class-contiguous blocks during the
partition reduction. Class-contiguous blocks make phase-3 expansion and
compression simpler.

## Recommendation 5: build a persistent anagram-class sidecar

If large `-m 2` queries are common, the best long-term solution is a persistent
companion index built once from `$IDX`.

Its logical structure would be:

```text
letter-multiset signature
    -> packed class metadata and best score
    -> offset/count of a score-ordered compressed member block
```

The class signatures themselves could be stored in a trie over sorted symbols
and counts. A query would traverse that trie under its letter-bag limits and
emit every fitting class once. It would no longer traverse and materialize
every fitting corpus spelling.

The complexity of phase 1 would change from proportional to matching index
entries to proportional to matching anagram classes, which is the quantity
phase 2 actually consumes. Member blocks would remain compressed or mmaped
until phase 3 needed them.

This has the largest implementation and index-build cost, but it is the design
that makes repeated queries over a 110-million-entry corpus scale naturally.

## Alternative: an exact two-pass query without a sidecar

It is also possible to avoid storing all members without writing an
intermediate member file:

1. First extraction retains only compact class metadata and the best member of
   each class.
2. Run phase 2 using representative members only, while marking every class
   appearing in a solution that survives the representative score floor.
3. Traverse the source index a second time and retain full or top-N member data
   only for marked classes.
4. Run the final phase-2/phase-3 search using those members.

The representative-only top-N floor is no higher than the true final floor,
because it is computed from a subset of valid spellings. Consequently, any
class solution whose best-member score cannot beat that floor cannot contribute
a final result.

This can be exact, but it repeats phase-2 traversal and index extraction. Score
tables and packed class preparation should be reused between the two searches.
It is attractive when disk spill is undesirable and phase 2 is cheap relative
to retaining 100 million spellings; otherwise a file-backed cold store is more
predictable.

## Suggested implementation order

1. Instrument phase 1 with:
   - emitted entry count;
   - distinct class count;
   - member text bytes;
   - vector capacities;
   - estimated hash-table and allocator overhead;
   - maximum and distribution of members per class.
2. Replace sorted string keys with numeric mixed-radix signatures and a flat
   hash table.
3. Split hot phase-2 class data from cold members and eliminate the duplicate
   rich/hot representations.
4. Store members and text in contiguous packed arenas.
5. For finite `-n`, collapse local word-set duplicates and keep only the best
   `N` per class.
6. Add bounded disk partitioning if the remaining entry payload can still
   exceed the memory budget.
7. Build a persistent anagram-class sidecar if this is a recurring production
   workload.

The first four steps are local representation changes and should preserve or
improve phase-2 performance. The sidecar is the architectural change that
removes the original 110-million-entry spelling count from query-time memory
and mostly from query-time work.
