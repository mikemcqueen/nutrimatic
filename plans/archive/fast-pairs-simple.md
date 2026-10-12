# Plan: speed up `pairs` on one thread

## Status

Implemented in `source/pairs.cpp`, with focused CLI coverage in
`source/test-pairs.sh`.

## Outcome

Make the compute portion of `pairs` substantially faster without adding
threads or changing its CLI. The implementation has two complementary parts:

1. store each letter-count vector as an aligned, zero-padded 32-byte value and
   make the hot fit test branchless so the compiler can vectorize it; and
2. group dictionary words with identical letter counts, test each pair of
   count groups once, and expand only successful group pairs into word pairs.

Output buffering, custom formatting, parallel work distribution, and other
I/O work are explicitly deferred.

## Current path

`source/pairs.cpp` currently stores 26 `uint8_t` counts per word. For each
dictionary word `i`, it subtracts that word from the input bag and calls
`fits()` for every later dictionary word `j`. `fits()` walks the 26 counts and
returns immediately at the first excess letter.

For a large bag with `N` eligible dictionary rows, this performs
`N * (N - 1) / 2` fit tests. The early return introduces data-dependent control
flow inside every test and prevents the compiler from comparing the count
lanes as a regular SIMD vector. It also repeats the same test for every pair of
anagrams even though all members of an anagram group have identical count
vectors.

Letter counts are already calculated only once per dictionary row. This plan
does not attempt to remove a repeated count calculation that is not present.

## Required behavior

- Remain single-threaded.
- Retain the current bag cleaning, `-u`, dictionary selection, and minimum-word
  filtering behavior.
- Use the existing case-insensitive, ASCII-letter-only count semantics. The
  32-byte group key is the count vector itself, not a separately sorted word.
- Emit every valid pair of distinct dictionary words exactly once and retain
  the final total-count line.
- Never emit a word paired with an identical word. Repeated byte-identical
  dictionary lines are one logical word and are retained only once.
- Words in the same count group may pair with one another only when the bag
  contains two copies of that group's count vector. Emit combinations of two
  distinct words, never a Cartesian product of the group with itself.
- Pair rows may be grouped differently from the current dictionary-order
  traversal. Preserve the current left/right orientation by retaining each
  word's original dictionary ordinal and printing the earlier word first, but
  do not spend compute time reproducing the old global row sequence.

The deliberate duplicate-row rule changes the current accidental behavior for
a dictionary containing the exact same line more than once: the old loop can
emit `word,word`, while the new implementation must not.

## Implementation

### 1. Introduce an aligned padded count type

Replace the 26-byte C array with a value type along these lines:

```cpp
constexpr size_t ALPHA = 26;
constexpr size_t COUNT_LANES = 32;

struct alignas(32) Counts {
    std::array<uint8_t, COUNT_LANES> values{};
};
```

Add compile-time checks that both `sizeof(Counts)` and `alignof(Counts)` are
32. Value-initialize the whole object in `letter_counts()`, increment only the
first 26 lanes, and keep lanes 26 through 31 zero in bags, words, keys, and
temporary remainders. This makes a full-width load legal and makes equality of
all 32 bytes equivalent to equality of the 26 real counts.

Use references to `Counts` rather than array-parameter decay. Update
`subtract()` to leave the six padding lanes zero. It remains outside the hot
quadratic comparison path and does not need a more elaborate optimization.

Do not add AVX2 intrinsics or raise the target's CPU requirement. The normal
build currently uses `-march=x86-64-v2`, which guarantees 128-bit SIMD but not
AVX2. A fixed branchless 32-byte loop can therefore compile to two 16-byte
operations in the normal build and to one 32-byte operation in an AVX2 build.
The representation and algorithm should benefit from either width.

### 2. Make the fit predicate branchless

Replace the per-letter early return with an OR reduction over all 32 lanes:

```cpp
uint8_t excess = 0;
for (size_t i = 0; i < COUNT_LANES; ++i)
    excess |= static_cast<uint8_t>(word.values[i] > pool.values[i]);
return excess == 0;
```

The source expresses 32 independent byte comparisons followed by one
reduction, with no data-dependent exit inside the loop. Keep this small enough
to inline into the group scan.

Inspect optimized code or the compiler's vectorization report after building;
alignment and padding are prerequisites, not proof that vectorization
occurred. The delivered implementation must show SIMD instructions for the
fit reduction under the repository's normal release flags. If the compiler
does not vectorize the exact source form, adjust the reduction form rather
than silently accepting scalar code.

### 3. Build count groups while loading the dictionary

Represent an accepted word with its text and original dictionary ordinal.
Represent a group with one `Counts` key and a vector of its words. Build:

```text
Counts -> group index -> [word, word, ...]
```

Use a hash and equality operation over the complete 32-byte value. Keep the
groups in a vector in order of first occurrence; the lookup table exists only
to find the vector slot while loading.

Maintain a separate set of accepted word texts. Ignore a byte-identical word
after its first accepted occurrence so duplicate dictionary rows cannot become
a pair. Do not deduplicate merely because two differently written words have
the same counts: those words belong in the same group and remain distinct
pair candidates.

Continue rejecting words that do not individually fit the bag before adding
them to a group. The first word that creates a group supplies its count key;
later group members need only retain text and ordinal, not another count copy.

### 4. Scan group pairs once

For each group `A`:

1. subtract `A.counts` from the bag once to obtain `remaining`;
2. test `A.counts` against `remaining` once for the same-group case;
3. test each later group `B` once with the branchless SIMD-friendly `fits()`;
   and
4. expand only successful group matches into output pairs.

For different groups, a successful match represents the Cartesian product of
their word vectors. Add `A.words.size() * B.words.size()` to the total, using a
wide type before multiplication, and emit that many rows. Use the stored
ordinals to put the earlier dictionary word on the left of each row.

For `A` paired with itself, require both `A.words.size() >= 2` and
`fits(remaining, A.counts)`. Add and emit exactly
`size * (size - 1) / 2` distinct combinations. Iterating the second word after
the first within the group enforces the no-self-pair rule.

Avoid reintroducing an unpredictable branch into every cross-group test.
Allocate a reusable array large enough for group indices, store the candidate
`B` index at the current end unconditionally, and advance the end by the
boolean fit result. After the row scan, expand the compact successful prefix.
This separates the compute-heavy compatibility scan from the intentionally
unchanged `printf` work.

### 5. Keep output work simple

Continue using the existing `printf("%s,%s\n", ...)` and stdout buffering.
Do not add hand-written integer or string formatting, larger custom buffers,
writer threads, temporary output files, or ordered chunk queues in this
change.

The output set and final total must agree with the old implementation for a
dictionary with unique lines. Global row order is not part of this phase;
group expansion will change it. If byte-for-byte ordering later proves to be a
downstream requirement, address that as separate output-focused work rather
than obscuring the group compatibility loop.

## Minimal validation

Add one focused shell smoke test for `pairs` and register it in
`source/meson.build`. Use tiny dictionaries and compare pair sets without
depending on global row order:

1. Two different count groups: confirm a successful group match expands to
   every cross-product pair and the total line matches.
   - Interleave the groups' members in dictionary order and confirm every row
     still prints the earlier dictionary word first.
   - Include an individually eligible third group that does not fit with the
     first group, after a group that does, to exercise rejection and the
     branchless successful-candidate compaction.
2. One anagram group with a bag containing two copies of its letters: confirm
   every combination of distinct group members appears once.
3. The same group with only one copy available: confirm no within-group pair
   appears.
4. Repeat an identical dictionary line: confirm it neither duplicates other
   output nor produces `word,word`.

Keep the test at the CLI boundary; no broad test matrix or unrelated DFS tests
are needed.

Suggested focused validation:

```bash
source ./setup.sh
source .env/bin/activate
source build/dep-info/conanbuild.sh
meson compile -C build pairs
meson test -C build pairs --print-errorlogs
git diff --check
```

For performance validation, temporarily time only the group compatibility
scan, excluding group expansion and `printf`, using the same large bag and
dictionary as the baseline. Remove the temporary timing instrumentation before
delivery. Before every accurate timing run, inspect the host process table for
both `query-index` and `dfs-anagrams`, as required by `AGENTS.md`. Pin baseline
and candidate runs to the same CPU when comparing them on this host.

Verify these performance facts rather than relying only on elapsed time:

- eligible word count and distinct group count;
- word-pair opportunities before grouping and group-pair checks afterward;
- total valid word pairs, which must remain unchanged for unique input; and
- SIMD code generation for the branchless fit reduction.

Run `/review` on the completed implementation before committing, as required
by `AGENTS.md`.

## Expected files

Implementation should normally be limited to:

- `source/pairs.cpp`;
- `source/test-pairs.sh`; and
- `source/meson.build`.

## Out of scope

- Worker threads or any other parallel execution.
- Output-buffer and formatting optimization.
- Preserving the old global output row order.
- A new count-only, benchmark, or diagnostics CLI option.
- Global `-march` changes, AVX2-only binaries, or runtime CPU dispatch.
- Changes to pair filtering, DFS search, query-index, or workflow semantics.
- General dictionary normalization beyond dropping byte-identical accepted
  lines.
