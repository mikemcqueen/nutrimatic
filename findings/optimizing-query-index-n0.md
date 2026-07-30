# Bounded-memory `query-index --require-completable -n 0`

## Use case

The practical unlimited-output workload is:

```text
query-index $IDX LETTERS -m 2 --require-completable -n 0
```

Unlike `dfs-anagrams`, `query-index` really must print every qualifying index
entry in this mode. A finite per-class or global top-N cap therefore cannot
solve its memory problem.

The goal should instead be:

- retain only class-level data while calculating completability;
- defer concrete entry handling until the class filter is known;
- bound the memory used to order the surviving entries;
- preserve the current output order exactly.

## Current memory behavior

`query-index` first constructs a complete `DfsClassList`. The current class
builder retains every extracted entry as a `DfsClassMember`, including an
individually owned `std::string`, inside:

```cpp
std::unordered_map<
    std::string,
    std::vector<DfsClassMember>
> grouped;
```

After phase 2 produces the class-parallel `completable` bitmap, `query-index`
constructs another entry-sized structure:

```cpp
struct RankedMember {
  double log_score;
  DfsClassMember const* member;
};

std::vector<RankedMember> ranked;
ranked.reserve(classes.entry_count());
```

It appends every printable member of every completable class and then runs
`partial_sort`. With `-n 0`, `top == ranked.size()`, so this is a complete
in-memory sort.

On a 64-bit target, `RankedMember` is normally 16 bytes. A 100-million-entry
extraction therefore reserves approximately 1.6 GB for this array alone,
separate from:

- the much larger member strings and vectors in `DfsClassList`;
- the class map and class keys;
- packed phase-2 metadata;
- the score-bound cache;
- the file-backed source index.

The current design consequently retains the same entry data in both an
allocation-heavy owning representation and a large pointer/ranking array at
the memory high-water mark.

## Key property: completability is class-wide

`find_completable_classes()` returns one decision per anagram class. All
spellings in a class consume the same letter multiset, so removing any member
leaves the same remainder. A class is either printable in its entirety or
rejected in its entirety, subject to later display-only filters such as
`--words-only`.

Phase 2 does not require every spelling. It requires compact class metadata:

- packed letter requirements;
- support mask and length;
- rarest-letter bucket;
- score-key delta;
- a best-member score for its bounds.

This permits class discovery and completability checking to be separated from
concrete output-entry production.

## Recommended pipeline

### Pass 1: extract classes only

Traverse the source index under the existing letter-bag, minimum-length, phrase,
and dictionary filters. At each terminal entry:

1. increment diagnostic entry counts;
2. update the class-frequency statistics needed for rarity ordering;
3. compute the entry's numeric anagram-class signature;
4. insert or update one compact class-builder record;
5. update the class's best member score;
6. discard the concrete spelling when the traversal backtracks.

Do not retain member strings, member vectors, or sorted-string class keys.
The extractor's current path string is sufficient temporary storage during
the traversal.

An anagram class can be represented by a query-relative mixed-radix subbag ID:

```text
multiplier[0] = 1
multiplier[s] = product(limit[0..s-1] + 1)

class_key = sum(used_count[s] * multiplier[s])
```

Update this key incrementally as letters are consumed and restored. Use checked
arithmetic and a compact open-addressed table mapping:

```text
numeric class signature -> compact class index
```

For the current 47-letter `S6`, the theoretical subbag count is
3,135,283,200, so the query-relative signature fits in `uint32_t`. Inputs with
a larger state product should use `uint64_t` or a packed fallback.

After class ordering is fixed, build the phase-2 hot arrays directly. Avoid
constructing rich `DfsAnagramClass` objects and then duplicating their contents
in `prepare_hot_classes()`.

### Phase 2: calculate the class bitmap

Run the existing shared phase-2 preparation and
`find_completable_classes()`. Its result remains one bit per ordered class.

After this step, output production needs only:

- a mapping from numeric class signature to ordered class ID;
- the class-parallel completability bitmap;
- the display options and scoring model.

The score cache, projected actions, and other phase-2-only structures can be
released before allocating the external-sort buffer.

Care is required for `--words-only`: phrase classes and phrase members must
remain available during phase-2 preparation because phrases can complete the
remainder, even though phrase members are not printed. The display-only
`word_count == 1` test belongs in pass 2.

### Pass 2: extract and filter concrete entries

Traverse the source index again with exactly the same extraction filters. For
each emitted entry:

1. obtain its numeric class signature from the incremental traversal state;
2. look up the ordered class ID;
3. test `completable[class_id]`;
4. apply display-only filtering such as `--words-only`;
5. calculate its displayed ranking score;
6. append the entry to a bounded sort buffer if it survives.

No per-entry state survives unless the entry is printable. No rejected member
is copied into long-lived storage.

The second traversal reads the same mmaped trie and should usually benefit from
warm file-cache pages. It repeats trie decoding and emission work, but avoids
the allocator traffic and memory pressure of retaining the first pass's
spellings.

## Preserving output order with an external sort

The current `query-index` output is globally ordered by:

1. score descending;
2. spelling text ascending.

For the default zero word bonus, score order is equivalent to corpus count
descending. With a word bonus, single-word and multi-word entries can be
reordered, so the external record must preserve the same `DfsScoreModel`
comparison used today.

Streaming pass-2 survivors directly to stdout would not preserve this order.
Instead, use bounded sorted runs:

1. Fill a configurable in-memory buffer, initially perhaps 256 or 512 MiB.
2. Sort its records with the production comparator.
3. Write the sorted records sequentially to a temporary run file.
4. Clear and reuse the buffer.
5. After extraction, k-way merge the run files directly to stdout.

A run record needs:

- the comparison score or the values required to reproduce it;
- the original count used in output;
- spelling length;
- spelling bytes;
- any word-count/flags needed by the selected display format.

Variable-length records make random access during sorting awkward. Reasonable
implementations include:

- a byte arena plus fixed-width descriptors during run construction, followed
  by rewriting into sorted variable-length records;
- fixed-width descriptors referring to a run-local text spool;
- length-prefixed inline records plus a separately sorted offset array.

The completed run should be sequentially readable. The final merge needs only
one buffered head record per run and a small heap or loser tree.

Limit the number of simultaneous runs. If extraction creates too many, merge
them in bounded fan-in stages rather than opening every temporary file at once.

Memory then becomes:

```text
compact metadata per distinct class
+ phase-2 cache, only while phase 2 is alive
+ class-signature lookup and completable bitmap
+ one bounded external-sort buffer
+ bounded merge buffers
```

It no longer scales with the number of extracted or surviving entries.
Temporary disk usage scales with the surviving output, which is unavoidable
when all output must be globally sorted.

## One traversal plus a spool

An alternative is to spool every pass-1 entry to disk together with its numeric
class signature:

```text
class ID, count, word count, text
```

After phase 2, scan the spool, discard non-completable records, and build the
same external sorted runs.

This avoids the second trie traversal but writes every extracted entry,
including entries later rejected by completeness. It is attractive when trie
decoding is substantially more expensive than sequential temporary I/O, but
can require much more disk bandwidth and space when the completeness filter is
selective.

The two designs should be measured:

| Design | Trie traversals | Writes before filtering | RAM |
|---|---:|---:|---:|
| class-only pass plus re-extraction | 2 | none | bounded |
| class pass plus complete entry spool | 1 | every extracted entry | bounded |

The likely initial choice is two trie traversals because it is simpler, writes
only actual output candidates, and the second traversal should use warm mmap
pages.

## Persistent anagram-class sidecar

For repeated large queries, a persistent companion index is the better
long-term design. It would store:

```text
letter-multiset class signature
    -> packed hot metadata and best score
    -> score-ordered member block
```

A query would enumerate fitting classes once, calculate their completability,
and then read member blocks only for completable classes. This removes the
second traversal of the spelling trie and avoids query-time regrouping.

There are two ways to produce globally sorted unlimited output:

1. Feed the surviving member blocks into bounded external sorted runs.
2. Keep each sidecar member block internally score ordered and perform a
   multiway merge across completable class streams.

The second avoids rewriting surviving entries, but a cursor and merge-heap
record per surviving class may itself be large and random reads across many
member blocks may page poorly. A staged external merge is more predictable.
Measurements should decide between them.

For arbitrary `--word-bonus`, the sidecar can keep single-word and multi-word
members as two count-ordered streams per class. Each stream remains internally
ordered because the bonus is constant within that category; the query merges
them using the requested effective score.

## What does not solve `-n 0`

The following optimizations remain useful but are insufficient alone:

- A top-N heap cannot be used because every survivor is requested.
- Keeping the best N members per class is not exact for unlimited output.
- Merely packing members still leaves memory proportional to the output size.
- Streaming directly from the trie loses the established global output order.
- Compressing anonymous in-memory arenas delays the limit but does not bound
  it.

Numeric signatures and packed class metadata solve the class-level overhead.
External storage and merge sorting solve the unavoidable entry-level output
volume.

## Suggested implementation sequence

1. Add phase-1 diagnostics for:
   - emitted entries;
   - distinct classes;
   - estimated current member and class bytes;
   - classes accepted and rejected by completability;
   - surviving printable entries.
2. Introduce query-relative numeric class signatures.
3. Add a class-only extraction mode that retains no members.
4. Allow `find_completable_classes()` to consume the resulting compact class
   representation directly.
5. Implement a second extraction pass that filters through the class bitmap.
6. Add bounded in-memory sorted runs and a k-way output merge.
7. Release phase-2-only allocations before constructing sort runs.
8. Compare second traversal against a one-pass complete-entry spool.
9. Consider the persistent anagram-class sidecar after the bounded-memory
   query path is working.

## Verification

Keep the tests small but require exact output equivalence:

- compare the new bounded path byte-for-byte with the current implementation
  on a synthetic index;
- cover `-n 0`, finite `-n`, `--words-only`, dictionary filtering, and positive
  and negative word bonuses;
- force multiple tiny sort runs to exercise the final merge;
- verify equal-score spellings remain text-ascending;
- verify phrase classes remain available for completion under `--words-only`;
- compare dense and projected completion modes;
- confirm temporary files are removed on success and ordinary error paths.

For real-index measurements, use `$IDX`, source `setup.sh` for `S6`, and check
that no other `dfs-anagrams` process is running before collecting timing data.
Record:

- peak RSS;
- temporary bytes written;
- pass-1, phase-2, pass-2, run-sort, and merge time;
- emitted, surviving, and printed entry counts;
- distinct class count;
- output hash against the current implementation where the baseline fits.

The primary success criterion is that peak RSS is bounded by class count,
configured phase-2 cache, and configured sort memory rather than by the number
of printed entries.
