# Plan: pack the phase-1 class list

Implements `findings/optimizing-class-list-2.md`. That document measured a
~450 B/class footprint that kills a 64-letter `-m 2` run at 11 GB, and set a
target of ~24 B/class + ~16 B/entry in flat arenas.

Field widths, escape tables and the abort policy follow the findings as
written. Two sections deviate, both marked **[deviation]**: this plan builds no
letters arena, and it keeps `class_key()` as a test-only accessor. One question
remains under "Open decisions" at the end.

## Goal

Replace `DfsAnagramClass` (a `std::string` key, a `std::vector` of
`(uint8_t, uint32_t)` letters, and a `std::vector<DfsClassMember>` of
`std::string`-bearing members, ~4-5 mallocs per class) with fixed-width records
in shared arenas, and replace the `std::unordered_map<std::string, ...>`
grouping with a numeric-signature table that is freed before phase 2.

No change to results: same classes, same members, same member order, same
scores. Class *order* within a rarest-rank bucket changes tie-breaks only
(see "Ordering" below).

## Stage 0 (landed)

`dfs_diagnostic_to_stream()` and the allocation-free `diagnostic_v` core
landed in `2cd1049`. Nothing further is needed for the diagnostic API; the
abort/error sites below just call it.

## Arenas

Two retained arenas and one transient. Records point directly into them —
`char const*` and `DfsPackedMember const*`, not integer offsets. Chunks are
allocated once and never move, and sorting the class records permutes records
without touching arena bytes, so pointers stay valid; they are the same 8 bytes
as an offset and cost one load instead of a shift, a mask and two loads. Nothing
in scope needs the records to be relocatable.

| Arena | Element | Sized | Owning type | Pointed to by |
|---|---|---|---|---|
| `text_arena` | `char`, spelling with internal spaces, no trailing space | chunked, 64 MiB | `std::vector<std::unique_ptr<char, AlignedFree>>` + cursor | `DfsPackedMember::text`, `text_length` bytes |
| `members_arena` | `DfsPackedMember`, 16 B | one allocation, `entry_count` known after extraction | `std::unique_ptr<DfsPackedMember, AlignedFree>` + count | `DfsClassRecord::members`, `member_count` entries |
| intermediate members | `(class_id, text, count, text_length, word_count)` | chunked, 64 MiB | `std::vector<std::unique_ptr<IntermediateMember, AlignedFree>>` + cursor | stage 2 scatter only, freed at stage 2 step 4 |
| class records | `DfsClassRecord`, 24 B | one allocation, `C` known at stage 1e | `std::unique_ptr<DfsClassRecord, AlignedFree>` + count | `classes()` | 

Not `std::vector`, for three reasons. The scatter is random-access by `base[id]`,
so the buffer must be sized up front, which means `std::vector` would
value-initialize 960 MB that step 3 immediately overwrites. Dedup cannot use
`erase`: each class's gap sits inside a *shared* array, so a per-class erase
would shift every later class's range and invalidate its base, which is why
step 6 compacts once in a hand-written pass instead. And the leftover capacity
after dedup cannot be handed back by `erase` (which only lowers `size()`) or by
`shrink_to_fit` (which reallocates and copies, spiking ~960 MB). Nothing
`std::vector` offers is used here: `std::sort` and `std::unique` take
`DfsPackedMember*` directly, since pointers are random-access iterators.

The chunk *list* is a `std::vector` and may reallocate freely — it holds chunk
pointers, not arena bytes, so nothing pointing into a chunk is disturbed.

Only the two arenas appended to *during* extraction are chunked, because their
final size is unknown then; a 1.2 GB `std::vector` reallocation would hold old
and new buffers at once and spike 2.4 GB above steady state. A spelling never
straddles a chunk boundary — pad to the next chunk when one would. The
intermediate arena holds fixed-size records instead, so it needs no straddling
rule provided chunk capacity is expressed in *records* (`chunk_bytes /
sizeof(IntermediateMember)`) rather than bytes. Because `members_arena` and the
class-record array are both allocated once at known sizes, they need no chunking
and no straddling rule.

**`allocate_aligned(0)` returns NULL, which is not a failure.** A bag that
extracts nothing gives `C == 0` and `E == 0`, and phase 2 already has to special
-case this (`if (requirements != 0)`, `dfs-search.cpp:588`). Phase 1 must do the
same, or the overflow policy's allocation-failure abort fires on a legitimately
empty query — the same bug shape as the `classes.empty()` case flagged under
"Overflow policy".

There is **no letters arena.** A class's letters are decoded from its signature
on demand — see "Decoding letters".

Phase 2's `packed_letters` (`dfs-search.h:394`, indexed by
`FitClass::letters_offset`, `dfs-search.h:201`) is separate storage that already
exists and keeps its current names: `uint32_t` elements, `(count << 6) | rank`,
rank-ordered with repeated symbols first, built once by
`prepare_hot_classes()`.

## Data structures

All in `source/dfs-class-list.h`, all owned by `DfsClassList`.

```cpp
struct DfsClassRecord {           // 24 bytes
  uint64_t signature;             // mixed-radix subbag code; letters decode from it
  DfsPackedMember const* members; // into members_arena, member_count entries
  uint8_t  member_count;          // abort if a class exceeds 255 members
  uint8_t  key_length;            // total letters; replaces key.size()
  uint8_t  letters_count;         // distinct symbols, <= 36
  uint8_t  rarest_rank;           // 0..36, 36 = none
  uint32_t reserved;              // tail padding, named
};

struct DfsPackedMember {          // 16 bytes
  char const* text;               // into text_arena, text_length bytes
  uint32_t count;                 // abort if a corpus count exceeds UINT32_MAX
  uint8_t  text_length;           // <= 255, guaranteed by the 128-letter cap
  uint8_t  word_count;            // <= 128, likewise
  uint16_t reserved;              // tail padding, named
};
```

Every field is the narrowest width the design needs; `reserved` is the tail
padding the compiler inserts anyway, named rather than spent. Both records hold
an 8-byte member, so `alignof` is 8 and `sizeof` rounds up to a multiple of it —
`DfsClassRecord` is 24 bytes whether its tail is one byte or seven, and
`DfsPackedMember` is 16 either way. That is all the findings' `uint32_t
reserved` ever was: a name for padding, not space bought.

No escape tables. Three hard limits, each checked and each aborting with a
diagnostic naming the cause (see "Overflow policy"):

- **128-letter input cap**, rejected at argument parse. This is what makes
  `uint8_t text_length` sound: a stored spelling is at most
  `letters + letters / min_word_len - 1` bytes, which at the worst case `-m 1`
  is `128 + 127 = 255` exactly. The cap is flat rather than
  `min_word_len`-dependent so one number covers every `-m`. It also bounds
  `key_length` and `word_count`. Longest input in `setup.sh` is 117 letters, so
  nothing in current use is affected; a 130-letter bag that runs today is
  rejected after this change. `DfsClassList` re-checks defensively, since it is
  a library that does not go through `parse_args` in tests.

  **Check the post-subtraction bag, not the argument.** The quantity the field
  width depends on is `out->letters` — what `DfsClassList` actually extracts —
  and both binaries build it in the same three lines
  (`dfs-anagrams.cpp:205-210`, `query-index.cpp:245-250`):

  ```cpp
  if (!clean_letters(letters, "letters", &bag)) return false;
  if (!clean_letters(used.c_str(), "used letters", &remove)) return false;
  if (!subtract_letters(bag, remove, &out->letters)) return false;
  ```

  Checking inside `clean_letters` instead would be sound but wrong in three
  ways: it bounds the raw argument rather than the extracted bag; it rejects a
  130-letter argument whose `--used` list leaves a representable 122-letter bag,
  since subtraction only shrinks; and `clean_letters` distinguishes its two
  callers only by an error-message string, so the cap would also apply to the
  used-letters list, which is not a bag and has no such bound.

  So: a new `bool check_bag_length(std::string const& bag)` in `dfs-cli-args.h`
  beside `clean_letters` and `subtract_letters`, called on `out->letters`
  immediately after `subtract_letters` in both `parse_args`. The line below it
  already validates the same string (`finalize_min_word_length(out->letters,
  ...)`), and both functions already `return false` there, so `main` still
  returns 2 like any other bad flag. The message must name the post-subtraction
  count — `error: 132 letters after removing used letters, 128 maximum` — or a
  user who passed `--used` cannot reconcile the number with what they typed.
  `find-anagrams` has its own private `clean_letters` (`:126`) and is unaffected
  either way.
- **255 members per class.** Measured on `wiki-merged.5`, the busiest class is
  `ainost` with **210 members at `-m 1`** (167 at `-m 2`); `ainors` 188,
  `aeinst` 197. Max members per class peaks at 5-7 letter classes and *falls*
  with class length (170 at length 6, 22 at length 16, 2 at length 24 in a
  32-letter `-m 2` run), so this is a corpus property that saturates rather than
  growing with bag length — the 128-letter cap does not make it likelier. But
  210 of 255 is thin: a larger corpus, not a larger bag, is what fires this
  abort, and unlike the signature overflow it is reachable at default settings.
  Accepted by decision; the abort must name the class (`class_key(ci)`) and its
  member count so the cause is actionable.
- **`UINT32_MAX` on a member's corpus count.** Highest observed is 6.26e7
  (`to`), so the headroom is 68x, but per-entry counts scale with corpus size,
  so this is checked on the same footing rather than assumed. Checked in
  `emit()`, at the narrowing from `int64_t` — see stage 2.

### Settled: the narrow fields and their aborts are not up for review

**Do not re-litigate this.** A reviewer will notice that both records have named
padding to spare — `member_count` and `text_length` could be `uint16_t` at
`sizeof` 24 and 16 unchanged, which would retire the 255-member abort and the
128-letter input cap for zero bytes. That is true, it has been raised, and it is
**declined**. The narrow fields and the hard aborts are wanted for their own
sake: a checked limit that fires loudly is the design, not a byte-saving
measure, and `reserved` is deliberately held in reserve rather than spent on
raising a ceiling. Treat the 128-letter cap, the 255-member abort and the
`uint8_t` widths as fixed requirements of this plan and do not propose widening
them again.

### Decoding letters

`DfsAnagramClass::letters` had exactly two readers, both once per class:
`rarest_rank` in phase 1 (`dfs-class-list.cpp:191-195`) and
`prepare_hot_classes()` in phase-2 setup (`dfs-search.cpp:565-568`, `605-609`,
`613-620`, `623-639`). The `!hot_classes_ready` branches that read it per DFS
node are dead under the abort policy. Storing the letters was therefore ~720 MB
caching a value read twice, so the signature carries them instead:

```cpp
// Writes (count << 6) | symbol in ascending symbol order; returns the count,
// which equals record.letters_count. out must hold DFS_SYMBOL_COUNT entries.
size_t decode_class_letters(size_t ci, uint16_t* out) const;
```

Callers pass a `uint16_t out[DFS_SYMBOL_COUNT]` stack buffer, so this allocates
nothing. `DfsClassList` retains the multiplier table built in stage 1b, compacted
to just the symbols present in the input bag — a `DfsClassList` member, since
`prepare_hot_classes` decodes during phase-2 setup, long after the constructor
returns — and walks it descending: `digit = rem / multiplier[s]`,
`rem %= multiplier[s]`, stopping as soon as `rem == 0`. Plain division; the
divisors are per-query constants. `prepare_hot_classes` decodes once into its
buffer and runs its existing four passes over that.

A descending walk produces descending symbols, so **fill `out` from the back**
and return a pointer or offset to the first written entry. Ascending order is
not cosmetic: today's `letters` is built ascending by the `0..35` loop at
`dfs-class-list.cpp:170`, and matching it keeps `prepare_hot_classes`'s
`packed_letters` byte-identical, which is what makes the differential run a
check on this change rather than on two orderings at once.

Budget the early exit pessimistically. `rem` reaches 0 only once every symbol
*below* the current one is absent, and the low symbols are the common letters
that nearly every class contains — so expect close to a full pass over the
compacted table (26 entries for a letters-only bag, up to 36 with digits), not
the half a mean of 9 distinct symbols per class might suggest. That is why
stage 3 decodes once per class rather than twice.

The count in an entry occupies the top 10 bits, capping one symbol at 1023 —
comfortably above the 128-letter input cap.

## Public API

`classes()` keeps its name and returns a `DfsClassSpan` — a by-value
`{DfsClassRecord const* data; size_t count;}` with `size()` and `operator[]` — so
`classes.classes().size()`, `classes()[ci]` and index-parallel arrays at every
existing call site compile unchanged. It cannot stay a `std::vector const&`: the
record array is a `std::unique_ptr<DfsClassRecord, AlignedFree>` (see "Arenas"),
because stage 1e scatters signatures into it by class id and so must size it up
front, which a vector would value-initialize — 960 MB written twice. The two sites
that bind the result by reference (`dfs-search.cpp:537`, `:3407`) are having their
declared type changed from `DfsAnagramClass` regardless, so a by-value span costs
them nothing.

Field access changes; add to `DfsClassList`:

```cpp
size_t decode_class_letters(size_t ci, uint16_t* out) const;  // see above
size_t class_length(size_t ci) const;             // was classes()[ci].key.size()
size_t member_count(size_t ci) const;
DfsMemberView member(size_t ci, size_t mi) const; // text ptr+len, count, word_count
std::string class_key(size_t ci) const;           // [deviation] test/debug only
```

`DfsMemberView` is a by-value `{char const* text; size_t text_length; int64_t
count; int word_count;}` — no allocation, valid while the list lives.

### Invalidating the grouping

Stage 5 sorts `members_arena` in place across class boundaries, which destroys
the class -> member grouping. Both operations that follow from that live on
`DfsClassList`, because `DfsClassList` owns the arenas — `DfsAnagramSearch`
borrows a `DfsClassList const*` and owns no member data at all:

```cpp
// Drops the class -> member grouping: zeroes every record's members pointer
// and member_count, and latches the flag. The member records and their text
// stay alive, because output is about to read them.
void invalidate_members();
bool members_invalidated() const;

// Frees members_arena and text_arena outright. Legal only once no spelling
// will be read again, so the caller must already have copied out what it
// prints. Recovers ~1.8 GB at the 90-letter scale.
void release_members();
```

Two operations, not one, because "invalid" and "freeable" happen at different
moments. After the sort the grouping is meaningless but the member *records* are
exactly what the print loop reads — so the sort can invalidate, and only a caller
that has finished printing can release. That also makes `release_members()`
useful only for a finite `-n`: with `-n 0` every member is about to be printed, so
there is nothing to free.

What has to check the flag:

- `member(ci, mi)` and `member_count(ci)` — assert `!members_invalidated()`.
- `dfs-search.cpp:540`, the `best_member_log_scores` build. This is the **only**
  production reader of the member store outside output, and it reads
  `members[0]` — precisely the field a global sort destroys. Assert here.
- `DfsTopN` (`dfs-output.cpp:53-66`, `:134-170`) indexes members by
  `(class_index, member_index)` throughout expansion, so it must assert at
  construction. `dfs-anagrams` never invalidates, so this is a guard against a
  future caller rather than a live bug.

What does **not** need a check, and is worth knowing so nobody adds one:
`best_member_log_scores` is a `std::vector<double>` copy made once at setup, and
every search-time score read goes to that array (`dfs-search.cpp:715`, `:814`,
`:870`, `:1983`, `:2075`, `:3029`, `:3343`), never to a member. The search is
already immune; the hazard is confined to *rebuilding* the array — a second
`DfsAnagramSearch`, or a re-run of `prepare_phase_two` — after invalidation,
which the assert at `:540` catches.

`class_key(ci)` rebuilds the sorted-letter string from the decoded letters. It
is not used in production paths; it exists so
`test-dfs-class-list.cpp:43`, `test-dfs-output.cpp:35` and
`test-dfs-search.cpp:34` change by one call each instead of being restructured
around letter-count lookups.

It returns **symbol order**, so letters precede digits. Today's `make_class_key`
sorts raw characters (`dfs-class-list.cpp:20`), where `'0'` at 0x30 precedes
`'a'` at 0x61, so a class holding both spells `"1a"` today and `"a1"` here.
Deliberately not corrected: no production path reads key content, and every
existing bag is letters-only. Worth knowing only if a digit bag is ever compared
against pre-change output.

## Stage 1: numeric class signature and sort-free grouping

Replaces findings recommendations 2 and 4.

### 1a. Hoist the checked arithmetic and the aligned allocator

Move `checked_multiply_u64` / `checked_add_u64` (`dfs-search.cpp:49`, `:55`)
into `dfs-class-list.h` as `inline`, and delete the file-scope copies.
`dfs-search.cpp` already includes that header. Do **not** move the diagnostic
calls into the helpers: the call site is the only place that knows which bag and
symbol overflowed, and `dfs-class-list.h` stays free of a diagnostic dependency
(`dfs-class-list.cpp` adds the `#include`).

Also hoist the aligned allocator, so phase 1's arenas use the same mechanism as
phase 2's arrays rather than a second convention. Five things move, all currently
private to `dfs-search`:

| Thing | Now | Note |
|---|---|---|
| `CACHE_ALIGNMENT` | `dfs-search.cpp:40` | 64 |
| `round_up_alignment` | `:61` | `aligned_alloc` requires a size that is a multiple of the alignment |
| `allocate_aligned` | `:77` | rounds, then `aligned_alloc` |
| `allocate_aligned_exact` | `:84` | `posix_memalign`, no rounding; only user is `:1035` |
| `AlignedFree` | `dfs-search.h:187` | **private nested struct** of `DfsAnagramSearch` |

New header `source/dfs-alloc.h`, header-only — `AlignedFree::operator()` is a
one-line `free(pointer)`, so nothing needs a `.cpp` and the meson build is
untouched. Promoting `AlignedFree` out of `DfsAnagramSearch` touches seven
declarations in `dfs-search.h` (`:391-394`, `:419-421`) and four locals in
`dfs-search.cpp` (`:575`, `:580`, `:585`, `:593`), all mechanical.

`dfs-alloc.h` rather than `dfs-class-list.h` on purpose: the class list is a
consumer of the allocator, not its home, and phase 2 must not have to include the
phase-1 header to allocate memory. (The checked arithmetic above could reasonably
move there too; it goes to `dfs-class-list.h` only because phase 1 is its
first-listed user. If both hoists land together, put both in `dfs-alloc.h`.)

Beyond consistency this buys the 64-byte alignment for `members_arena`, so a 16 B
record never straddles a cache line, and it keeps the option of returning the
post-dedup tail to the OS with `MADV_DONTNEED` — which works with any base
alignment, as long as the range is rounded inward to whole pages.

### 1b. Build the phase-1 multiplier table

In `DfsExtractor`'s constructor, from the input bag, indexed by symbol
`0..35` (**not** by rank — `symbol_to_rank()` is derived from phase-1 letter
frequencies at `dfs-class-list.cpp:177` and does not exist yet):

```
multiplier[s] = prod over t < s of (bag_limit[t] + 1)
```

Any fixed symbol ordering is a valid bijection. Also build a 256-entry
`char -> multiplier` table, because `DfsExtractor::bag` is indexed by raw byte
(`dfs-class-list.cpp:111`), not by symbol.

On `checked_multiply_u64` failure, report and stop (see "Overflow policy"):

```cpp
dfs_diagnostic_to_stream(stderr,
    "error: %zu-letter bag needs a class signature wider than 64 bits"
    " (overflowed multiplying radix %u for symbol '%c')\n", ...);
```

Because multiplication is commutative, this product equals phase 2's
rank-ordered `state_count` (`dfs-search.cpp:1184`) exactly, so phase 1's
verdict and `exact_state_encodable` (`dfs-search.h:389`) always agree. Assert
that in `prepare_phase_two` rather than carrying two booleans that can drift.

### 1c. Maintain the signature incrementally

In `DfsExtractor::walk` (`dfs-class-list.cpp:96-103`), add
`multiplier_by_char[ch]` alongside `--bag[ch]` and subtract it alongside
`++bag[ch]`. At `emit()` the running signature *is* the class signature, since
the emitted text's letters are exactly the consumed letters. This deletes
`make_class_key`'s per-emission copy-and-sort (`dfs-class-list.cpp:15-22`,
~60 M calls at 90 letters) and all string hashing.

### 1d. Open-addressed signature table

Two **slot-indexed** parallel arrays, sized to the next power of two above
`expected / 0.6`, grown by rehash:

```
uint64_t key[]        // signature + 1, so 0 is the empty slot
uint32_t class_id[]   // dense id in first-seen order
```

12 B/slot rather than the 16 a single 8-byte-aligned struct would cost. Probing
is an integer compare — the signature identifies a class exactly, so there is
no key comparison.

One further array, indexed by **dense class id** rather than by slot, so it is
not part of the table and does not pay the load factor:

```
uint32_t class_members[]  // running member count per class id, C x 4
```

`emit()` becomes: probe/insert to get `class_id`, append the text to the text
arena, append `(class_id, text, count, text_length, word_count)` to the
intermediate member arena, `++class_members[id]`.

This also fixes recommendation 4 by construction: there is no
`extractor.grouped` map alive next to `class_list`.

### 1e. Allocate the class records and tear the table down

`C` is known once extraction finishes, so the class-record array is allocated
here rather than in stage 3, and `signature` is written into every record
immediately — scan the slots, and for each occupied one store `key[slot] - 1`
into `record[class_id[slot]].signature`. Then free **both** table columns,
before stage 2 begins.

This ordering is load-bearing, not tidiness. The signature is the only copy of a
class's identity: every letter decode in stage 3 and in `prepare_hot_classes`
divides it, and it cannot be recovered from anything else once the table is
gone. Freeing `key[]` before the records exist would strand it.

`class_members[]` survives into stage 2 — it is the input to the prefix sum, and
is dense by class id, so it is unaffected by the table teardown.

## Stage 2: flat member arena

Replaces findings recommendation 3, but by counting sort rather than a
comparison sort — the table from stage 1 already knows each class's member
count, so grouping is O(E) with no `std::sort` over 60 M records.

1. Allocate `members_arena` at exactly the emitted entry count.
2. Prefix-sum `class_members[]` into a per-class base index.
3. Scatter the intermediate arena into `members_arena` at `base[id]++`,
   consuming the prefix sum in place — the classic counting-sort trick, so no
   second `write_cursor` array. After the scatter `base[id]` has advanced to the
   *end* of class `id`'s range; recover its start by subtracting the count.
4. Free the intermediate arena.
5. Per class, sort its range by the existing `member_order` (count desc, text
   asc, word_count asc) and unique by `same_member`. Ranges are tiny — mean 1.55
   members at 60 letters, 210 at the worst class measured — so this is
   single-threaded.

   Both predicates compare `std::string` today (`dfs-class-list.cpp:118-127`);
   on `(text, text_length)` the text comparison must reproduce
   `std::string::operator<` exactly — `memcmp` over the **shorter** of the two
   lengths, then shorter-is-less as the tie-break — and text equality is equal
   lengths plus `memcmp` of 0. A `memcmp` over one length, over the longer
   length, or `strncmp` on non-terminated arena bytes each yields a different
   but still valid total order, so the sort succeeds, dedup still removes
   adjacent equals, and the only symptom is member order inside a class
   differing from `master` — surfacing as a differential mismatch with nothing
   pointing at the cause.
6. Compact in one forward pass to close the gaps dedup left, and accumulate
   `entries`. Class base indexes are rewritten to pointers in stage 3, after
   this pass settles them.
7. Count `frequencies` over the surviving member texts (must be after dedup,
   as today at `dfs-class-list.cpp:155-161`).

Steps 3-4 are the build-time peak: the intermediate arena (E x 24) and
`members_arena` (E x 16) are both live. See "Projected memory" below.

The `UINT32_MAX` check on a member's corpus count belongs in `emit()`, **not**
here. `IndexReader::Choice::count` is `int64_t` and the intermediate record
narrows it to `uint32_t`, so by the time the scatter runs an overflowing count
has already been truncated and there is nothing left to detect. Check at the
one point where the `int64_t` is still in hand.

## Stage 3: packed class records

Replaces findings recommendation 1.

The record array and its `signature` column already exist from stage 1e. What
remains is one pass per class id, and the order of that pass differs from
today's constructor in one way: **`frequencies` -> ranks moves ahead of the
record fill**, so a single decode serves every field that needs one.

1. `frequencies` -> `symbols_by_rank` / `ranks_by_symbol`
   (`dfs-class-list.cpp:177-186`). `frequencies` is complete as of stage 2
   step 7, so nothing blocks this from running first.
2. One pass over class ids. Decode the signature **once** into a stack buffer
   and from that one decode take `letters_count`, `key_length` (the digit sum)
   and `rarest_rank` (min over `ranks_by_symbol[symbol]`, as at `:188-196`).
   Fill `members` as `members_arena + base[id]` and `member_count` from stage 2
   in the same visit.
3. Sort the record array by `(rarest_rank asc, key_length desc, tie-break)`.
   Records are self-contained 24 B values, so this is a plain sort with no
   member movement — the `members` pointer travels with the record.
4. `bucket_starts` (`:206-213`).

Today's constructor decodes nothing, so it can afford to compute `rarest_rank`
in a second sweep (`:188-196`) after the ranks exist. Here that sweep would cost
a second full division pass over every class — order 10 s at 40 M classes on the
pessimistic decode budget above — for a field that costs nothing to fill while
the letters are already in the buffer. Hence the reorder.

Step 2 is also where the 255-member limit is enforced, on the **post-dedup**
count. Stage 1's `class_members[]` counter and stage 2's prefix sum stay
`uint32_t` and must be allowed past 255 transiently — clamping or aborting there
would reject a class that dedup brings back under the limit.

### Ordering

The `key < key` tie-break (`:203`) becomes the class signature ascending. Any
fixed total order canonicalises correctly (the DFS only walks
`class_index >= entry_point`), and the `(rarest_rank, key_length desc)` prefix
is preserved — `prepare_length_certificate` depends on non-increasing length
within a bucket (`dfs-search.cpp:709`) and still gets it.

Consequence to expect: solutions that tie on score can come out in a different
order. `test-dfs-cli-differential.sh` compares sorted keys and is unaffected;
`test-dfs-output.cpp`'s ordered expectations get re-blessed where a score tie
made the order arbitrary, by inspecting the tie rather than pasting new output.

## Stage 4: call-site migration

Mechanical, no behavior change. `key.size()` -> `class_length(ci)`;
`letters[i].first/.second` -> one `decode_class_letters(ci, buf)` into a stack
buffer, then `class_letter_symbol/class_letter_count(buf[i])`;
`members[mi].text/.count/.word_count` -> `member(ci, mi)`.

- `dfs-search.cpp`: `:540-544` (`best_member_log_scores`), `:565-568`, `:599-655`
  (`prepare_hot_classes`), `:684`, `:705`, `:1621-1630`, `:1654-1664`,
  `:1682-1691`, `:1738`, `:1810`, `:3342-3378`, `:3406-3418`.
- `dfs-output.cpp`: `:53-66` (`spelling_log_score`), `:116`, `:134-140`,
  `:162-165`.
- `query-index.cpp`: `:405-435` — see stage 5.
- Tests: `test-dfs-class-list.cpp:42-51`, `test-dfs-output.cpp:34-57`,
  `:118-123`, `:441-444`, `test-dfs-search.cpp:34`.
- `measure-f.cpp` has its own `Class`/`c.letters` (`:341`, `:433-461`) and is
  **not** affected.

Note `prepare_hot_classes` still repacks into `packed_letters` in rank order
with repeated symbols first (`:611-620`); its input is now a decode rather than
a stored vector. What stage 3 removes is the *per-class allocation*, not the
repack.

## Stage 5: drop `ranked` in `query-index`

Findings recommendation 5, ~1 GB. `query-index.cpp:406` reserves
`entry_count()` x 16 B and pushes every entry regardless of `-n`, each holding a
`DfsClassMember const*` that pins the class graph through output.

`model.first_segment_log_score(count, word_count > 1)` is monotone in `count`
within each of the two `word_count > 1` groups, so ranking needs no `double`
per row and no parallel array.

**One path, both `-n` modes.** The class -> member grouping has no reader left in
`query-index` once phase 2 has finished — `dfs-search.cpp` touches `.members` in
exactly one place (`:540-541`, the `best_member_log_scores` build at setup), the
`DfsAnagramSearch` is scoped inside the `if (args.require_completable)` block and
destroyed when that block closes at `:401`, and the print loop reads only `count`
and `text`. So there is
no need for a bounded-heap path for finite `-n` and an in-place path for `-n 0`:

1. Compact the survivors (`completable[ci]`, and `word_count == 1` under
   `--words-only`) to the front of `members_arena`, in place.
2. `invalidate_members()` — the grouping is now gone, by construction.
3. `std::partial_sort` the first `top` survivors.
4. Print. For a finite `-n`, `release_members()` afterward if anything else is
   still to run; with `-n 0` there is nothing to release.

No parallel array, no `double` per row, and no allocation at all. This also fixes
the half of the problem the findings called out but the old wording did not: today
`-n 1` pays the full `reserve(entry_count())`, and under this scheme it pays
nothing.

**The comparator, and why it stays byte-identical.** With the default
`--word-bonus 0` (`query-index.cpp:22`) the two `multi_word` groups collapse and
`log(count)` descending *is* integer `count` descending: over this count range
`log` is injective in `double` (counts <= 6.26e7, so adjacent logs differ by
~1.6e-8 against ~4e-15 of representable spacing at log ~ 18). An integer compare
therefore reproduces today's order exactly, ties included, which matters because
`ranked_order` (`:259-262`) falls through to `text < text` on equality.

With a nonzero bonus, do **not** rewrite the comparison as `count * exp(bonus)`
vs `count`: that rounds differently from `log(count) + bonus`, so it breaks exact
ties differently and changes the text tie-break. Computing two `log` calls per
comparison across a full sort of 60 M rows is also too slow. Instead partition in
place by `is_phrase`, sort each side by integer `count`, and *merge* the two runs
with the `double` comparator — O(E) `log` calls instead of O(E log E), using the
identical expression, so the output is unchanged.

**Ordering constraint.** The compaction must run after `find_completable_classes`,
because `best_member_log_scores` was built from `members[0]` and a global sort
destroys the meaning of `members[0]`. That is satisfied today by construction;
the assert at `dfs-search.cpp:540` is what keeps it true. See "Invalidating the
grouping".

**Landing it first is only a partial win.** Before stage 2 exists there is no
arena to compact, so the pre-arena version still needs a `(class_index,
member_index)` list: 8 B/row against today's 16, a halving rather than an
elimination, and it gets rewritten once the arena lands. Worth doing first only
for the `-n 1` case, which stops over-reserving immediately.

`findings/optimizing-query-index-n0.md` proposes a larger design for the same
file — two extraction passes and an external merge sort, to make peak memory
independent of the output size. Nothing to reconcile: that design also deletes
`ranked`, so this stage is a step along it and not a conflicting one. It is a
separate plan, and this one does not wait for it.

## Overflow policy

`abort()` at every site, per the findings and per the same decision applied to
the member limit. Six causes, each with a diagnostic naming it, all emitted
with `dfs_diagnostic_to_stream(stderr, ...)` so they appear with diagnostics
switched off (a plain `dfs_diagnostic()` is a silent no-op on a NULL stream,
`dfs-diagnostic.h:10` — exactly wrong on a fatal path):

One argument error:

| Cause | Where | Reachable in practice |
|---|---|---|
| bag over 128 letters | `check_bag_length`, after `subtract_letters` in `parse_args` | yes — a clean `return 2`, not an abort |

and five aborts:

| Cause | Where | Reachable in practice |
|---|---|---|
| bag over 128 letters, defensively | `DfsClassList` constructor | only via a caller that skipped `parse_args`, e.g. a test |
| class over 255 members | stage 3 record fill | yes on a larger corpus |
| member count over `UINT32_MAX` | stage 1 `emit()`, where the `int64_t` narrows | 68x headroom today |
| signature wider than 64 bits | stage 1b multiplier build | no (needs ~90 flat chars) |
| unsupported bag in phase 2 | `dfs-search.cpp:1531` | no (same bag classes) |

The bag cap appears in both tables on purpose: `parse_args` rejects it cleanly for
the two CLIs, and the constructor aborts on it for any other caller, since
`DfsClassList` is a library and tests construct it directly. Since phase 1 is a
constructor and cannot return a value, its four causes abort in place rather than
surfacing through a `bool ok()`.

One prerequisite, unchanged from the findings: `prepare_hot_classes()`
returns false at `dfs-search.cpp:557` when `classes.empty()`, which is a
legitimate no-results case, not an unsupported bag. Split that out and handle
it as a clean empty return *before* the hot-class attempt, or valid queries
start failing. The remaining `return false` sites (`:557-570` size caps,
`:574`/`:579`/`:584`/`:591` allocation failure, `:607`, `:631-634`, `:648-653`
overflow) each set a `char const* unsupported_reason` used in the message; an
error that does not say which of the seven fired leaves nothing to act on.

`walk_unoptimized` and the non-hot branches of `exact_class_fits` /
`subtract_exact_class` / `restore_exact_class` are retained as source but are
no longer reachable at runtime, and nothing tests them — the findings' stated
policy.

## Projected memory

At the findings' extrapolated 90-letter scale (E ~ 60 M, C ~ 40 M):

| Structure | Bytes | 90 letters |
|---|---|---|
| class records | C x 24 | 960 MB |
| member records | E x 16 | 960 MB |
| text arena | E x ~14 | 840 MB |
| steady phase 1 | | **~2.8 GB** |

Two transients, and because of the stage-1e reordering they are **not**
simultaneously live with everything else — the signature table dies before
`members_arena` is allocated:

| Moment | Live | 90 letters |
|---|---|---|
| end of stage 1 (1e) | text + intermediate + table 768 + `class_members` 160 + records | **~4.2 GB** |
| stage 2 scatter (steps 3-4) | text + intermediate + `base[]` 160 + records + members_arena | **~4.4 GB** |
| phase-1 build peak | the larger of the two | **~4.4 GB** |

**[deviation]** The findings' ~4.2 GB total counts a letters arena this plan
does not build, and omits the intermediate member arena, which it does. Against
a current-representation projection of ~21 GB, the steady figure is a ~7x cut
and the build peak a ~4.8x cut.

The ~4.4 GB peak supersedes the ~5.0 GB stated in the findings' "Decisions after
review", which assumed the signature table was still live when `members_arena`
was allocated. Stage 1e frees it first, which is worth ~600 MB.

The build peak, not the steady figure, is what has to come in under the 6 GB
success criterion — stages 1 and 2 are where a run dies if this is wrong.

### How much projection error the gate absorbs

The scatter peak is `54 bytes/entry` (text 14 + intermediate 24 + members 16)
plus `28 bytes/class` (records 24 + `base[]` 4). Holding the projected 1.5
entries per class, that is about `72.7 MB` per million entries, so the 6 GB gate
breaks at **E ~ 85 M — 1.4x the projected 60 M**. If entries per class keeps
falling as it has monotonically from 4.32 at 24 letters to 1.55 at 60, the class
term grows faster and the margin narrows: at 1.4 entries per class the gate
breaks at E ~ 83 M.

So the gate tolerates roughly 40% of extrapolation error, on a projection whose
asymptote the findings state outright is unmeasured. That tolerance is the number
to check the 64/68/72-letter measurements against — see "Verification".

## Tests

Smoke level, per `CLAUDE.md`.

- `test-dfs-class-list.cpp`: existing assertions, via `class_key()` and
  `member(ci, mi)`. Add one class with two distinct spellings that dedup, to
  cover the stage-2 sort/unique/compact path.
- `test-dfs-search.cpp` / `test-dfs-output.cpp`: migrate accessors; re-bless
  ordered expectations only where a score tie made the order arbitrary.
- `test-query-index.sh` / `test-dfs-cli.sh`: unchanged, and these are the real
  regression gate — byte-identical output before and after on a fixed bag.
- No test for the unsupported-bag error path, per the findings' policy.

Differential check before and after, at a size that finishes:

```
query-index $IDX -m 2 -n 0 --require-completable "${S6:0:24}"
```

sorted and `cmp`'d against the same run on `master`.

## Verification

Implementation runs the short end of the findings' ladder only — up to 47 letters,
where phase 1 is ~31 s — with `RssAnon` sampled every 200 ms, and reports measured
B/class against the 450 baseline. That is enough to confirm the representation
shrank as designed. Check for other `dfs-anagrams` instances first (`CLAUDE.md`).

**Everything at 56 letters and above is handed off, not executed here.** When the
stages are landed and the short ladder plus the differential check are clean, stop
and tell the user to run:

```
64, 68 and 72 letters, then the 90-letter $S1 if those look good
```

under `systemd-run --user --scope -p MemoryMax=11G -p MemorySwapMax=0`, sampling
peak `RssAnon`, and to record entries, classes and peak at each. Do not run these
without being asked to, and do not report a verdict on the 90-letter case from the
short ladder alone.

Why those four sizes: 64 is where the current representation dies, so 64, 68 and
72 all become measurable for the first time once stage 2 lands, and **one** point
past the old death line cannot tell a plateau from continued 1.1x growth — two
can. Phase 1 is ~100 s at 60 letters and node counts grow ~1.12 per four letters,
so 68 is roughly a two-minute run.

**Success criterion, for the user to judge: phase 1 completes for the 90-letter
`$S1` under 6 GB peak `RssAnon`.** That is the case the findings were written
about. Today the run dies in phase 1 somewhere between 60 and 64 letters at 11 GB.
The 64-letter run is the intermediate checkpoint, expected around 3.2 GB on the
current projection.

The break-even in "How much projection error the gate absorbs" is what those
measurements get compared against. If the refit puts the 90-letter peak over the
gate, that is the user's call to make from their own numbers — report the gap and
stop.

Sample peak for the **whole** run, not just up to `phase 1 complete`. Phase 2
allocates its own per-class arrays (`packed_letters`, `fit_classes`,
`score_key_deltas`, `best_member_log_scores`) plus an `exact_number_memo`
reserve of `classes.size()` entries (`dfs-search.cpp:1766-1775`), none of which
this plan touches. At 47 letters and 5 M classes the findings measured that
whole tail at roughly 200 MB, so it is not obviously a problem — but it scales
with class count, and if it dominates once phase 1 is packed, that is the next
plan and it should be started from a measurement rather than from struct-size
arithmetic.

## Risks

- **Stage 2 is the correctness-critical part.** The counting-sort scatter, the
  dedup and the compaction all rewrite each class's member base; an off-by-one
  silently reassigns members to the wrong class. The 24-letter differential run
  catches this and runs after stage 2 alone, before stage 3 lands.
- **Ordering churn.** Some ordered test expectations get re-blessed, by
  inspecting the tie rather than pasting new output wholesale.
- **Decode cost.** Two per-class decodes replace two per-class array reads.
  Measure phase-1 wall clock at 47 letters (31 s today) before and after.
- **`class_key()` invites production use.** Keep it out of hot paths; it
  allocates.

## Suggested order

Stage 5 (independent, small) -> stage 1 -> stage 2 -> stage 3, with stage 4's
call-site migration landing alongside 2 and 3 as each accessor arrives. Overflow
policy with stage 1. Differential run after each of 1, 2 and 3.

## Decisions that departed from the findings

Recorded in `findings/optimizing-class-list-2.md` under "Decisions after
review", so the two documents do not contradict each other:

- No escape tables. `uint8_t member_count` and `uint8_t text_length`, with a flat
  128-letter input cap at argument parse and a hard abort on any limit exceeded.
  Settled and closed to further review — see "Settled: the narrow fields and
  their aborts are not up for review".
- No letters arena; letters decode from the signature on demand.
- Pointers rather than integer offsets in both records.
- Arenas owned as `std::unique_ptr<T, AlignedFree>`, not `std::vector`, with
  `allocate_aligned` hoisted out of `dfs-search.cpp` into `dfs-alloc.h` (stage
  1a). `classes()` therefore returns a `DfsClassSpan` rather than a vector
  reference.
- Stage 5 is one path for every `-n`, gated by `DfsClassList::invalidate_members()`
  / `release_members()` rather than by a heap-versus-arena split.
- Success criterion is the 90-letter `$S1` phase 1 under 6 GB peak `RssAnon`,
  measured by the user. Implementation verifies up to 47 letters and hands off
  the 64/68/72/90-letter runs rather than executing them.
