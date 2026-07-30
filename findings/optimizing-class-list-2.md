# Class-list memory, measured — `query-index -m 2 -n 0 --require-completable`

`findings/optimizing-class-list-1.md` reasoned from an estimate: ~110 M index
entries, ~100 bytes each, ~10 GB. This document replaces that with measurements
against `idx/wiki-merged.5.index` at `-m 2`. The conclusion differs in one
important way: **almost the entire footprint is per-class overhead, not member
payload**, and the share grows with input length. At the 90-letter input that
motivated this, per-class cost is ~93% of the total.

## Measurements

`RssAnon` sampled from `/proc/PID/status` every 200 ms. Runs at 52+ letters were
confined to a `systemd-run --user --scope -p MemoryMax=11G -p MemorySwapMax=0`
cgroup so that exhaustion is a local kill rather than a global OOM (see
`findings/avoid-wsl-shutdown.md` — `OOMPolicy=continue` is now in place on this
box, but the cap keeps failures clean).

Bags are `${S6:0:N}` up to 47 letters and `${S1:0:N}` from 52 on. `$S1` is the
90-letter input in question.

| Letters | Entries | Classes | Entries/class | Peak anon | Trie nodes |
|---|---|---|---|---|---|
| 24 | 388,518 | 89,925 | 4.32 | 272 MB | 1.9 M |
| 28 | 1,524,494 | 439,311 | 3.47 | 634 MB | 7.2 M |
| 32 | 3,300,059 | 1,047,057 | 3.15 | 1102 MB | 15.9 M |
| 36 | 5,430,802 | 2,074,514 | 2.62 | 1075 MB | 26.0 M |
| 40 | 7,931,708 | 3,518,651 | 2.25 | 1760 MB | 37.5 M |
| 47 | 10,300,633 | 5,080,410 | 2.03 | 2.81 GB | 47.7 M |
| 52 | 21,693,522 | 12,495,224 | 1.74 | — | 100.2 M |
| 56 | 31,601,047 | 19,800,850 | 1.60 | 9.55 GB | 148.8 M |
| 60 | 35,061,615 | 22,678,850 | 1.55 | 10.90 GB | 166.3 M |
| 64 | *(killed)* | *(killed)* | — | **>11 GB** | — |

The 64-letter run was SIGKILLed by the cgroup **during phase 1** — it never
printed `phase 1 complete`. Phase 1 alone therefore exceeds 11 GB somewhere
between 60 and 64 letters, less than three quarters of the way to 90.

The 47-letter row includes phase-2 structures (projected score table 24.9 MB,
`fit_classes` 81 MB, `best_member_log_scores` 41 MB); phase 1 alone is ~2.6 GB.
`RssFile` peaks at 664 MB of the 1.2 GB mmapped index, which is file-backed and
reclaimable — not real pressure.

### Two premises corrected

**The bag is only binding at short inputs.** At 47 letters `-m 2` extracts
10.3 M entries, ~9% of the index — estimating the entry count from index size
overshoots by 10x *there*. But that reverses as letters grow: by 60 letters the
walk visits 166 M trie nodes and extracts 35 M entries. The two regimes need
separate reasoning, and the 90-letter case is firmly in the second.

**Extraction decelerates but does not plateau within reach.** Entries grow ×1.46
from 52→56 letters, then only ×1.11 from 56→60 (nodes: ×1.49 then ×1.12). The
index is being exhausted, so the naive geometric extrapolation is wrong. But the
64-letter run died before reporting, so the asymptote is *unmeasured*.
Continuing at ×1.05–1.11 per four letters puts 90 letters near **~60 M entries /
~40 M classes**, and every projection below inherits that uncertainty.

### The cost is per-class, not per-entry

Fitting `bytes = C·c + E·e` on the 36- and 40-letter rows gives ~408 B/class +
~52 B/entry. Refitting on 56 and 60, where the user's regime actually lives:

```
c ≈ 450 bytes per class
e ≈  35 bytes per entry
```

Entries/class falls monotonically — 4.32 at 24 letters to 1.55 at 60 — so the
per-class term keeps gaining share. At 60 letters classes are
`22.7M × 450 = 9.6 GB` of the measured 10.9 GB: **~93%**.

This kills two recommendations from `optimizing-class-list-1.md` for this
workload:

- **Recommendation 3 (cap members per class at N)** is unavailable — `-n 0`
  requests everything — and would be near-worthless anyway against a mean of
  1.55 members per class.
- **Recommendation 1's emphasis** on packing the cold member store is real but
  now third-order. Perfect member packing alone recovers under 10%.

## Where the 450 bytes per class go

Verified on this ABI: `sizeof(DfsClassMember) == 48`,
`sizeof(DfsAnagramClass) == 88`, `sizeof(std::string) == 32`,
`sizeof(std::pair<uint8_t,uint32_t>) == 8`.

| Cost | ~B/class | Cause |
|---|---|---|
| `grouped` hash node | 80 | 8 next + 8 cached hash + 32 key string + 24 vector, in an 80 B chunk |
| `grouped` bucket slot | 8 | ~one bucket per element |
| `grouped` key heap | ~6 | keys over the 15-char SSO limit |
| members vector allocation | ~110 | 24 B vector + `malloc(capacity × 48)` with doubling slack + chunk header |
| `DfsAnagramClass` | 88 | `key` 32 + `letters` 24 + `members` 24 + `rarest_rank` 4 + pad |
| `class_list` key heap | ~6 | second copy of the same key string |
| `letters` heap | ~56 | one `malloc` per class, 8 B per distinct symbol |

Three structural problems drive this:

1. **`grouped` and `class_list` are both fully live at peak.** `DfsExtractor` is
   a constructor local (`dfs-class-list.cpp:136`) not destroyed until the
   constructor returns, while `class_list` is filled in the loop at `:142`. The
   map's nodes, buckets and key strings — ~95 B/class, **~2.2 GB at 23 M
   classes** — are dead weight throughout. Only the member vectors are moved
   (`members.swap`, `:146`).
2. **Four to five small allocations per class.** Hash node, key characters,
   member vector, `letters` vector. At 23 M classes that is ~100 M malloc
   chunks, each with header and rounding.
3. **Two oversized fields.** `std::pair<uint8_t, uint32_t> letters` spends 8
   bytes on a symbol plus a count that cannot exceed the bag length; 2 bytes
   suffice. And that array is *already* repacked into `packed_letters` by
   `prepare_hot_classes()` (`dfs-search.cpp:548`), so the per-class allocation is
   pure duplication.

`DfsAnagramClass::key` is also removable. Outside `dfs-class-list.cpp`,
production code reads only `key.size()` (`dfs-search.cpp:644,684,705,1738,1810,3349`).
The string content is used by the internal tie-break sort (`:202`) and by
`test-dfs-output.cpp:35,79` / `test-dfs-search.cpp:34`. A `uint8_t key_length`
plus a tie-break on packed letters replaces it; the tests can look classes up by
letter counts.

## Decisions after review

`plans/optimizing-class-list-claude.md` implements the recommendations below
with four departures, all decided after the measurements in this document:

1. **No letters arena.** Recommendation 1 keeps a `C × ~9 × 2` = 720 MB arena of
   `(symbol, count)` pairs. But once the overflow policy aborts rather than
   degrading, `letters` has exactly two readers, both once per class —
   `rarest_rank` (`dfs-class-list.cpp:191`) and `prepare_hot_classes()`
   (`dfs-search.cpp:565-639`) — because the `!hot_classes_ready` branches that
   read it per DFS node become dead. The signature is a bijection onto subbags,
   so the letters decode from it in fewer than 36 divisions. 720 MB was caching
   a value read twice.
2. **No escape tables.** Recommendation 1's `uint8_t member_count` escape and
   recommendation 3's `UINT32_MAX` count escape are both replaced by checked
   aborts, and the field widths stay `uint8_t`. Measured worst case for members
   per class on `wiki-merged.5`: **`ainost` has 210 members at `-m 1`** (167 at
   `-m 2`), `aeinst` 197, `ainors` 188. Max members per class peaks at 5-7
   letter classes and *falls* with class length — 170 at length 6, 22 at
   length 16, 2 at length 24 in a 32-letter `-m 2` run — so it is a corpus
   property that saturates rather than scaling with bag length. 210 of 255 is
   thin margin, and a larger corpus rather than a longer bag is what would fire
   the abort; accepted deliberately.
3. **Flat 128-letter input cap** at argument parse, rather than recommendation
   3's `min_word_len`-dependent rejection at 171 letters (`-m 2`) or 128
   (`-m 1`). One number covers every `-m`, since `-m 1` is the worst case:
   `128 + 127 = 255` is exactly `uint8_t text_length`. Longest input in
   `setup.sh` is 117 letters.
4. **Pointers, not `uint64_t` offsets**, in both records. Same 8 bytes, one load
   instead of a shift, a mask and two loads. The offsets bought relocatability
   for a future file-backed sidecar, which is not in scope.

The projected total below is therefore wrong in two directions: it charges
720 MB for a letters arena that is not built, and omits the transient
intermediate member arena (`E × 24` ≈ 1.4 GB) that the counting-sort build
needs. Corrected: ~2.8 GB steady, ~5.0 GB build peak, against a success
criterion of 6 GB for 90-letter `$S1` phase 1.

## Recommended changes, ordered by bytes recovered

Targets use the extrapolated 90-letter scale: E ≈ 60 M, C ≈ 40 M.

### 1. Pack the class record and share one letters arena (~18 GB → ~1.7 GB)

This is where the money is, and it was under-weighted in doc 1.

```cpp
struct DfsClassRecord {        // 24 bytes
  uint64_t letters_offset;     // into a shared 2-byte-per-symbol arena
  uint64_t members_offset;     // into the flat member array
  uint8_t  letters_count;      // distinct symbols
  uint8_t  key_length;         // replaces key.size()
  uint8_t  rarest_rank;
  uint8_t  member_count;       // escape value for the rare overflow
  uint32_t reserved;
};
```

`C × 24 = 960 MB` plus a letters arena of `C × ~9 × 2 = 720 MB`, against
~450 B/class today. Drops the `key` string, its duplicate heap, and ~40 M small
allocations.

Offsets are `uint64_t` by decision, not by necessity — 32 bits would fit these
arenas at 90 letters, but the margin is not worth defending and the 8 extra bytes
per class buy the property that no arena can ever outgrow its addressing. The
record stays 8-byte aligned at 24 bytes with room in `reserved`.

### 2. One open-addressed table instead of a string-keyed map (~3.8 GB, and faster phase 1)

Replace the string-keyed `unordered_map` with a **`uint64` mixed-radix signature**
in one flat open-addressed table, freed before phase 2. This deletes
`make_class_key`'s per-emission copy-and-sort (60 M times), the string hashing, and
the node-per-class map.

Give symbol `s` multiplier `prod(limit[0..s-1] + 1)` and maintain the signature
incrementally in `DfsExtractor::walk` — add on consume, subtract on restore. The
encoding is a bijection from subbags onto `[0, prod(limit[s]+1))`, so a signature
identifies a class exactly and probing is an integer compare with no key
comparison needed.

**The key is 64 bits unconditionally.** The phase-2 preflight prints the bound
directly: for 47-letter `$S6` it is `3135283200` states, which would fit
`uint32_t`, but `$S1` gives **4.719e13 = 2^45.4**. There is no `uint32_t` fast
path worth carrying for the gap between them.

A packed-letters key — hash the `(symbol, count)` bytes, resolve by `memcmp` — is
retained in the source as a contingency for bags that overflow 64 bits. It is not
a supported runtime path; see the overflow policy below.

Either way the table is two parallel arrays rather than a padded struct — a
`uint64_t` key column (the signature, or the letters-arena offset) plus a
`uint32_t class_id[]`, 12 B per slot instead of the 16 a single 8-byte-aligned
struct would cost. At C ≈ 40 M and a 0.6 load factor, 64 M slots is
`64M × 8 + 64M × 4 = 768 MB`, freed before phase 2. The sizing is the same for
both designs.

#### The numeric key's machinery already exists in phase 2

Do not write a new checked multiply or a new overflow-detecting product. Phase 2
has both. `dfs-search.cpp:49`:

```cpp
static bool checked_multiply_u64(uint64_t a, uint64_t b, uint64_t* out) {
  if (a != 0 && b > UINT64_MAX / a) return false;
  *out = a * b;
  return true;
}
```

Division-based rather than `__builtin_mul_overflow`, which is fine — it runs 36
times at setup. `checked_add_u64` sits beside it. And `dfs-search.cpp:1184`
already builds the whole mixed-radix table with the overflow verdict:

```cpp
uint64_t state_count = 1;
exact_multipliers.fill(0);
if (encodable) {
  for (int rank = DFS_SYMBOL_COUNT - 1; rank >= 0; --rank) {
    exact_multipliers[size_t(rank)] = state_count;
    uint64_t const radix = uint64_t(bag[size_t(rank)]) + 1;
    if (!checked_multiply_u64(state_count, radix, &state_count)) {
      encodable = false;
      break;
    }
  }
}
exact_state_encodable = encodable;
```

Three consequences for the phase-1 implementation:

- **Reuse the helper, not the table.** `exact_multipliers` is indexed by *rank*,
  and rank comes from `class_list->symbol_to_rank()`, computed from phase-1 letter
  frequencies at `dfs-class-list.cpp:177`. It is therefore built after phase 1 and
  depends on phase 1's own output. Phase 1 must build its own table indexed by
  symbol `0..35` from the input bag, which is known before extraction starts; any
  fixed symbol ordering yields a valid bijection. Hoist `checked_multiply_u64` out
  of its `static` file scope into `dfs-class-list.h` and delete the duplicate —
  `dfs-search.cpp` already includes that header, so the dependency direction is
  correct.

- **The two overflow verdicts are provably equal.** Multiplication is commutative,
  so phase 1's symbol-ordered product equals phase 2's rank-ordered `state_count`
  exactly. Phase 1's `encodable` and `exact_state_encodable`
  (`dfs-search.h:389`) always agree; assert that rather than carrying two
  booleans that can drift.

- **An overflowing bag has already lost its fast path.**
  `hot_classes_ready = encodable && prepare_hot_classes()`
  (`dfs-search.cpp:1331`), and at `:1531`:

  ```cpp
  if (!hot_classes_ready) {
    walk_unoptimized(letters.size(), 0, 0, 0.0, sink);
  }
  ```

  So such a query already forfeits `fit_classes`, the projected score bounds, and
  parallel search. The packed-letters fallback is therefore preserving
  *correctness* on a query that is already running the unoptimized walk — it is
  not protecting a fast path.

#### Overflow policy: abort in both phases, no fallback tests

**The `uint64` numeric signature is the design.** The packed-letters key is
retained in the source as a contingency but is not a supported runtime path, and
nothing tests it. On overflow, phase 1 aborts; phase 2's degradation branch aborts
too.

This is affordable because overflow is out of reach for real inputs:

| Bag shape | `uint64` overflows at |
|---|---|
| 26 symbols (letters only), spread evenly | ~120 chars |
| 36 symbols (letters + digits), spread evenly | ~90 chars |

Those are worst cases requiring a near-flat letter distribution. Real text is
skewed, and skew shrinks the product sharply: `$S1` is 2^45.4 at 90 chars against
2^55.9 for the worst 90-char case, and `$S5` — the longest at 117 chars — reaches
only 2^56.8, still seven bits clear. No input in `setup.sh` comes close.

**Abort rather than degrade, in both phases.** The `checked_multiply_u64` /
`checked_add_u64` helpers abort on overflow, and the `walk_unoptimized` branch
aborts too. Together that removes every silent-degradation route, so no path
outside the supported one needs test coverage.

Both abort sites must print a diagnostic naming the cause, and it must appear even
when diagnostics are switched off. A plain `dfs_diagnostic()` call is not enough on
its own — a NULL stream makes it a silent no-op (`dfs-diagnostic.h:10`), which is
exactly the wrong behaviour on a fatal path.

**Add an explicit-stream variant rather than reassigning the global.** The
diagnostic core becomes a `va_list`-taking helper, and both entry points forward to
it:

```cpp
// dfs-diagnostic.h
void dfs_diagnostic_to_stream(FILE* stream, char const* format, ...)
    __attribute__((format(printf, 2, 3)));
```

```cpp
// dfs-diagnostic.cpp — both entry points forward to one va_list core.
void dfs_diagnostic_to_stream(FILE* stream, char const* format, ...) {
  va_list args; va_start(args, format);
  diagnostic_v(stream, format, args);
  va_end(args);
}

void dfs_diagnostic(char const* format, ...) {
  va_list args; va_start(args, format);
  diagnostic_v(g_diagnostic_stream, format, args);
  va_end(args);
}
```

**This part is implemented** (not just planned). `diagnostic_v` assembles the line
in a 256-byte stack buffer and truncates rather than growing, replacing the old
`std::vector<char>` sizing pass. That makes the core allocation-free, so a single
API serves every caller including abort paths reached *because* an allocation
failed — there is no separate non-allocating route to remember.

Measured headroom: the longest line any tool currently emits is 139 characters,
and the worst case is the bag-echoing line in `dfs-anagrams.cpp:252`, which reaches
173 characters for the 117-letter `$S5`. Truncation begins around a 200-letter bag.
Accepted for now.

This is preferable to `dfs_set_diagnostic_stream(stderr)` on the fatal path for a
concrete reason, not just tidiness. `dfs_check_failed` already notes that "worker
threads can reach this concurrently" (`dfs-diagnostic.cpp:35`), and phase 2 runs up
to 20 threads. Reassigning `g_diagnostic_stream` from a dying thread races any
concurrent `dfs_diagnostic()` on another worker and leaves the global permanently
redirected for whatever runs afterward. An explicit parameter touches no shared
state, so the abort path never mutates global configuration.

Phase 1, where the signature product is built:

```cpp
if (!checked_multiply_u64(product, uint64_t(limit[s]) + 1, &product)) {
  dfs_diagnostic_to_stream(stderr,
      "error: %zu-letter bag needs a class signature wider than 64 bits"
      " (overflowed multiplying radix %u for symbol '%c')\n",
      letters.size(), limit[s] + 1, symbol_char(s));
  abort();
}
```

Phase 2: all nine uses of the two helpers in `dfs-search.cpp` (`:631`, `:633`,
`:648`, `:650`, `:1190`, `:1239`, `:1258`, `:1285`, `:1293`, `:1313`) currently
feed *designed degradation* rather than error handling — `:631–634` returns false
from `prepare_hot_classes()` so `hot_classes_ready` becomes false, and
`:1190`/`:1285`/`:1313` clear `encodable`. Both routes land at `walk_unoptimized`
(`:1531`). Under this policy all of them become aborts and `walk_unoptimized`
becomes unreachable — retained as source, not as a runtime path:

```cpp
if (!hot_classes_ready) {
  dfs_diagnostic_to_stream(stderr,
      "error: %zu letters, %zu classes: bag not supported by the"
      " optimized phase-2 search (%s)\n",
      letters.size(), class_list->classes().size(), unsupported_reason);
  abort();
}
```

`unsupported_reason` matters more than it looks: `prepare_hot_classes()` has seven
distinct failure returns, and an abort that does not say which one fired leaves
nothing to act on. Have it set a `char const*` at each `return false` — overflow,
which size cap, or allocation failure.

The codebase's existing noreturn idiom is `DFS_CHECK` / `dfs_check_failed`
(`dfs-diagnostic.h:30–34`), which survives `NDEBUG` and reports file, line and
expression. It is a reasonable alternative if you want that provenance, but it
prints no domain context, so pair it with `dfs_diagnostic_to_stream` rather than
replacing it.

#### `dfs_check_failed` now shares the diagnostic API (implemented)

`dfs_check_failed` previously wrote its own bare `fprintf`, with a comment
explaining that "one fprintf keeps the message intact". That was the right call
while the diagnostic core allocated. Both halves of the rationale are now satisfied
by `dfs_diagnostic_to_stream` — `diagnostic_v` assembles into a stack buffer and
emits with a single `fputs`, so concurrent failures still cannot interleave, and it
allocates nothing even when the failing invariant is itself about memory. So it is
now:

```cpp
fflush(stdout);
dfs_diagnostic_to_stream(
    stderr, "%s:%d: invariant failed: %s\n", file, line, expr);
abort();
```

The gain beyond consistency: `DFS_CHECK` failures used to print with no timestamp,
leaving nothing to correlate them against the surrounding progress lines. They now
carry the same `[hh:mm:ss]` prefix as everything else, which is how you tell *when*
the invariant broke. Verified by forcing a failure:

```
[00:00:00] .../checktest.cpp:3: invariant failed: 1 + 1 == 3
```

with `SIGABRT` as before. Two notes on the change:

- **Assertion output now carries the timestamp prefix.** Nothing depends on that
  text — the only `DFS_CHECK` uses are `dfs-search.cpp:2542`, `:2548` and `:2551`,
  and no test or script matches "invariant failed" — but it is a visible change.
- **Very long expressions can now truncate** at 256 bytes, where the old `fprintf`
  had no limit. In practice `__FILE__` is a short build-relative path such as
  `../source/dfs-search.cpp` and the three existing checks are one-liners, so there
  is ample headroom; a very long multi-line `DFS_CHECK` expression would be the
  first thing to clip.

**Use `dfs_diagnostic_to_stream` for every abort cause, including allocation
failure.** This was a concern while the diagnostic core still built its line in a
`std::vector<char>` — reporting an OOM through an allocating function is fragile,
which is why `dfs_check_failed` uses a bare `fprintf`. The 256-byte stack buffer
removes the problem, so `allocate_aligned` failure at `dfs-search.cpp:572` can be
reported the same way as the overflow causes, with no special case.

Include-wise this is nearly free: `dfs-search.cpp` already includes
`dfs-diagnostic.h` (line 3), so phase 2's abort site needs nothing new.
`dfs-class-list.cpp` does not, so phase 1 adds it there — to the `.cpp`, not the
header. That is the other reason to emit these messages at the call sites rather
than inside the hoisted helpers: `dfs-class-list.h` stays free of a diagnostic
dependency, and the call site is anyway the only place that knows which bag and
which symbol overflowed.

**One case must be split out first, or valid queries crash.**
`prepare_hot_classes()` also returns false at `:557` when `classes.empty()` —
which is a legitimate result, not an unsupported bag. A query whose letters make
nothing extractable at the given `-m` produces zero classes today and correctly
prints nothing via `walk_unoptimized`. Handle emptiness as a clean no-results
return *before* the hot-class attempt, so the abort covers only genuine
unsupported-bag causes: the arithmetic overflows, the `UINT32_MAX` / `SIZE_MAX`
size caps at `:557–570`, and `allocate_aligned` failure at `:572`.

Should a real bag ever hit the abort, the retained packed-letters key is the
escape hatch. Note that the "no extra memory" argument for it assumed
recommendation 1's letters arena, which the plan does not build (see "Decisions
after review"), so reviving that fallback would mean storing the
`(symbol, count)` bytes somewhere first.

For the record, the numeric key is also the faster of the two, though not by much:
an 8-byte compare instead of a ≤44-byte `memcmp`, plus O(1) incremental
maintenance instead of an O(36) scan at emit — order 1–2 s across ~60 M emissions
against a phase 1 that takes ~100 s at 60 letters. That estimate is derived, not
benchmarked.

### 3. One flat member arena instead of a vector per class (~2.6 GB → ~1.8 GB)

```cpp
struct PackedMember {          // 16 bytes
  uint64_t text_offset;        // into one char arena
  uint32_t count;              // 0xFFFFFFFF escapes to a side table
  uint8_t  text_length;
  uint8_t  word_count;
  uint16_t reserved;
};
```

Highest count observed is 6.26e7 (`to`), so `uint32_t` is comfortable; keep the
escape as insurance. `E × 16 = 960 MB` plus a text arena of `E × ~14 = 840 MB`.

The two field-width decisions at this scale:

- **`text_offset` is `uint64_t`.** ~840 MB of text would fit in 32 bits, but the
  arena is the one structure whose size is hardest to bound in advance, so it gets
  full-width addressing. This is what takes the record from 12 to 16 bytes.
- **`text_length` stays `uint8_t`,** which requires an explicit guard rather than
  an assumption. `emit()` strips the trailing space, so a stored spelling is at
  most `letters + (letters / min_word_len - 1)` bytes: 134 for 90 letters at
  `-m 2`, and 179 even at `-m 1`. The bound is exceeded at 171 letters with
  `-m 2` or 128 with `-m 1`, so reject over-long bags at argument-parse time
  instead of letting the field silently truncate.

Grouping without a per-class vector needs a sort-based build: append
`(signature, text_offset, count, word_count)` to one growing array, then sort or
counting-sort by signature so each class's members land contiguous.

### 4. Don't keep `grouped` alive past its use

Independent of the above and a few lines: consume the map destructively, or scope
the extractor so it dies before `class_list` is built. Subsumed by 1–3, but worth
having on its own if those are deferred.

### 5. Drop `ranked` in `query-index` (~1 GB)

`query-index.cpp:406` reserves `entry_count() × sizeof(RankedMember)` = 16 B per
entry and pushes **every** entry regardless of `-n` — so `-n 1` pays for it too,
and each element is a `DfsClassMember const*` that pins the whole rich class
graph through output. The sort key
`model.first_segment_log_score(count, word_count > 1)` is monotone in `count`
within each of the two `word_count > 1` groups, so the packed member array can be
sorted (or counting-sorted on `count`) in place with no parallel array and no
`double` per row.

### Projected total

| Component | 90 letters |
|---|---|
| class records (`C × 24`) | 960 MB |
| letters arena (`C × ~9 × 2`) — *not built, see "Decisions after review"* | 720 MB |
| member records (`E × 16`) | 960 MB |
| text arena (`E × ~14`) | 840 MB |
| grouping table (transient, freed pre-phase-2) | ~770 MB |
| **peak** | **~4.2 GB** |

Corrected for the decisions above — no letters arena, plus the transient
intermediate member arena the counting-sort build needs — this is ~2.8 GB steady
and ~5.0 GB at the build peak.

Against a current-representation projection of ~21 GB — which is why 64 letters
already dies at 11 GB. That is **~5x**, and it turns the 90-letter case from
impossible into comfortable on a 15.8 GB box. Choosing `uint64_t` offsets and
states over the minimum-width alternative costs ~500 MB of the total (24 vs 16 B
per class record, 16 vs 12 B per member) and buys the removal of every
offset-overflow failure mode. All four class-list changes also improve phase-2
locality, and #2 removes real work from phase 1.

## If bounded-regardless-of-input is wanted

~4.2 GB fits, but it is still a function of letter count, and the projection
rests on an unmeasured asymptote. To make phase-1 memory a *configuration*
rather than a function of the input, add `optimizing-class-list-1.md`
Recommendation 4: emit `(signature, text, count, word_count)` records straight to
a temporary file in emission order, then radix-sort by signature so each class's
members land contiguous. Peak becomes the sort buffer. At 60 M entries the spill
is a few GB of disk, which is not the constraint here.

The `todo` item about filtering configurable skip words (especially `the`) during
phase 1 also bites much harder in this regime than at 47 letters, and is far
cheaper to implement than any of the above.

## Two-pass extraction: probably not worth it

`--require-completable` means member data is only ever *printed* for classes that
survive phase 2, so a class-only first pass then a members-only second pass would
cap peak at the larger of the two. But phase 2 needs only `letters` and
`best_member_log_scores`, so after changes 1–3 the first pass is already the
~1.7 GB packed form, and the second pass is nearly the whole member store unless
`--require-completable` prunes hard. At `-m 2` it should prune very little: a
2-letter minimum leaves almost every remainder fillable. Measure the surviving
fraction before building this.

## Separately: phase 2 wall-clock at `-m 2`

Measured on 47-letter `$S6`, not the 90-letter case. Phase 1 finished in 31 s,
then `find_completable_classes` ran **11+ minutes to visit 2.9 M nodes with 0
solutions**, single-threaded at the default `-S 1` with 19 of 20 cores idle. I
killed it there; peak was 2.81 GB, never close to a constraint.

So at 47 letters the binding constraint is time, and at 60+ letters it is memory.
Fixing the class-list representation is what gets a 90-letter input to *start*;
it will not make it finish. Phase-2 exact validation at `-m 2` deserves its own
investigation, and `-S 1` as the default looks wrong for this workload.
