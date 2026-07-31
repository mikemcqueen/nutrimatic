# Where find-anagrams actually spends its time

A profile-led pass over `find-anagrams`, looking for wins in data layout,
branching and memory behaviour rather than in the search algorithm. Everything
below was measured, not reasoned about; the changes described landed in the
working tree together and the whole set is worth **~3.2x throughput** and
**~1.65x more frontier in the same memory**.

This is deliberately *not* the algorithmic work. `findings/anagram-perf.md`
idea #3 (goal-directed priority) remains the largest untouched item and is
about what the search spends its steps *on*; this document is about making each
step cheaper. The two compose.

## Headline: OpenFST is not involved

`find-anagrams` does not link OpenFST at all. `source/meson.build` gives it
`optparse_lib`, `search_lib` and `index_lib`; `fst_dep` goes only to
`expr_lib`, and from there to `find-expr` and `test-expr`. `AnagramFilter` is a
hand-written mixed-radix counter in `source/find-anagrams.cpp`, not an FST.

So OpenFST costs `find-anagrams` exactly nothing, and no amount of tuning it
would matter. (It is squarely on `find-expr`'s critical path, which is a
separate question this document doesn't touch.)

## Method

- Index `idx/wiki-merged.5.index` (1.2 GB), page cache warmed.
- Workload `find-anagrams idx/wiki-merged.5.index abcdeeghilmnorst -m 4 -p 10`
  — 16 letters, 4-letter minimum. Long enough to reach a multi-gigabyte
  frontier, short enough to iterate on.
- Metric: **steps completed in a fixed 30 s**, read off the last progress line.
  Throughput falls as the frontier grows, so this is a whole-run figure rather
  than an instantaneous rate; it is stable to about ±3%.
- `perf record -F 499`, using `/usr/lib/linux-tools-6.8.0-136/perf` — the
  distro `perf` refuses to run against the WSL2 kernel.

Two environment limits worth knowing before trusting anything here:

- **No PMU on WSL2.** `cycles`, `instructions`, `cache-misses` and
  `dTLB-load-misses` all report `<not supported>`. Every attribution below is
  from `task-clock` sampling, which is enough to locate a hot instruction but
  cannot confirm *why* it is hot. Where it says "cache miss", that is inference
  from the access pattern, not a counter reading.
- **No transparent huge pages.** `/sys/kernel/mm/transparent_hugepage/enabled`
  is `madvise` and `hpage_pmd_size` is 2 MB, but a 1 GB anonymous mapping with
  an explicit `madvise(MADV_HUGEPAGE)` still yields `AnonHugePages: 0`, and
  `GLIBC_TUNABLES=glibc.malloc.hugetlb=1` changed neither `AnonHugePages` nor
  runtime. A ~3 GB frontier under 4 KB pages needs ~750k TLB entries, far past
  any TLB, so **huge pages remain a plausible unmeasured win on a real kernel**
  and should be retried there before anything else on this list.

## Starting profile

| Symbol | Share |
|---|---|
| `SearchDriver::step()` | 46.4% |
| `SearchDriver::queue_median_score()` | 20.5% |
| `IndexReader::children()` | 18.0% |
| `AnagramFilter::has_transition()` | 8.9% |
| `__log2f_fma` | 3.7% |

Two counts taken by instrumenting `step()`, which set up most of what follows:

- **23.5 children decoded per step, of which 25.6% survive the filter.** Three
  quarters of the decode work is thrown away.
- **~6 pushes per pop.** The frontier grows by ~5 entries per step.

And the heap's sift distances, which corrected a wrong guess:

- **sift-up walks 0.91 levels on average; sift-down walks 23.9 of 26.4.**

## The changes, in the order they were measured

| # | Change | Steps / 30 s | Delta |
|---|---|---|---|
| — | baseline | 19.5M | — |
| 1 | median from a sample, then from buckets | 27M | +38% |
| 2 | bucket queue replaces the binary heap | 47.5M | +76% |
| 3 | static linking + LTO | 51M | +7.4% |
| 4 | `-march=x86-64-v2` | 58M | +14% |
| 5 | filter pre-declares its characters | 63M | +8.6% |
| — | **total** | **~62.5M** | **~3.2x** |

### 1. `queue_median_score()` was 20% of runtime — for a progress line

`queue_median_score()` copied the `log_score` of **every** frontier entry into
a vector and ran `nth_element` over it. At 70M+ entries that is a ~280 MB
allocation and a full linear pass, and `PrintAll` called it every 100k ×
`progress_factor` steps.

Removing it bought **+38%**, well above its 20% profile share. The extra comes
from what the profile can't show: the pass streams the entire multi-gigabyte
frontier through the cache and evicts everything the search actually needs, so
it costs both its own time and a cold restart afterwards.

It went through two forms. Striding over at most 2^18 entries made the cost
constant and landed within 1% of the exact median. Once change #2 bucketed the
frontier by score, the median became a walk over bucket *sizes* — no per-entry
work at all — which is what is in the tree now.

Worth stressing how the cost scaled: `-p 1` (the default for every tool other
than `find-anagrams`, including `find-expr`) calls this **ten times more
often**. This was the single worst line in the search and it existed purely to
print a diagnostic.

### 2. The binary heap → a monotone bucket queue (the big one)

One instruction — the `comiss` inside the inlined sift-down — was **29% of
total runtime**. Not arithmetic: a dependent load from a random position in a
3 GB array, ~24 of them per pop, each one a near-certain cache and TLB miss.

The instrumented sift distances say where the cost is and, importantly, where
it *isn't*:

- **sift-up: 0.91 levels.** Pushes are children of the just-popped maximum but
  land at the array's end, and almost always stop immediately. Pushes are
  nearly free despite outnumbering pops 6:1.
- **sift-down: 23.9 of 26.4 levels.** `pop` moves the array's *last* element —
  a near-minimum — to the root and sifts it essentially all the way back down.

That rules out the obvious fix. **A 4-ary heap would not have helped**: it
halves the depth, but the children compared at each level are contiguous
either way, so a binary heap touches ~1 cache line per level over 26 levels and
a 4-ary heap touches ~2-3 over 13. The miss counts wash. Worth recording,
because "use a d-ary heap" is the standard advice for exactly this profile and
it would have been wasted effort here.

The property that does help: **the search never raises a priority.** A
character transition holds the scale and can only shrink the count; a restart
adds `log2(restart) < 0`. The maximum only ever falls — this is a *monotone*
priority queue, and those admit a bucket queue:

- Buckets indexed by quantized `log_score`, 16 per log2 unit.
- `push` appends to a bucket's tail; `pop` takes from the current bucket's tail.
- A cursor walks the buckets downward once and never goes back.

Both operations are O(1) and touch memory sequentially. ~24 dependent random
accesses per pop become ~1 sequential one. Worth **+76%**.

**It also fixes the memory cliff**, which `findings/anagram-perf.md` lists as
unsolved under idea #5. A heap is one allocation that doubles, needing the old
and new buffers live simultaneously — the `std::bad_alloc` at a third of
available RAM. Bucket vectors double independently and are tiny by comparison,
and `settle()` hands a bucket's storage back as the cursor passes it, so memory
behind exhausted scores is *released* mid-search. Under an identical
`ulimit -v 6000000` (6 GB):

| | steps before `bad_alloc` | frontier | matches found |
|---|---|---|---|
| binary heap | 13M | 66M | 4 |
| bucket queue | 22M | 109M | **12** |

Same steady-state bytes per entry (~32-34, measured both ways); the difference
is entirely the doubling transient. 1.65x more frontier and 3x the matches in
the same RAM, on top of running 1.8x faster.

**What this gives up.** Within a bucket, entries come out last-in-first-out
rather than by exact score. A bucket is 1/16 of a log2 unit, or 4.4% in score.
Consequences:

- Printed matches are descending only to that tolerance — the run above emits
  `7.970e-07` before `8.113e-07`. Anything downstream that assumes sorted
  output needs to know this.
- The arrangement `seen` keeps for a word set can be any scoring within a
  bucket width of the best, rather than the best.

This is the same class of approximation `findings/priority-invariant.md`
already records for the float round-trip, but ~60x larger. `SCALE` is a single
constant if a finer order is wanted; the cost of raising it is more buckets,
which is cheap.

One correctness detail: `push` clamps a bucket index below the cursor up to the
cursor. The float round-trip in `step()` can land a child an ulp *above* its
parent, and without the clamp that entry would be filed behind the cursor and
never popped — a silently lost path.

### 3 & 4. Build flags: +22% combined, for no code change

Both were visible in the profile and neither needed a line of source.

**Static linking + LTO (+7.4%).** The libraries are built shared, so
`step()` → `children()` is a cross-DSO PLT call once per step and
`has_transition()` is a virtual call into the executable ~23 times per step.
Neither can inline. `default_library=static` + `b_lto=true` lets them.

**`-march=x86-64-v2` (+14%).** The profile showed `__popcountdi2` — a
*libgcc function call* — at 5.0% plus 1.2% in its PLT stub. Baseline x86-64 has
no POPCNT instruction, so `__builtin_popcountll` in `SearchDriver::collect()`'s
`Marks` was compiled as a call. The v2 level turns it into the instruction.

**`-march=native` measured no better than v2** (58M vs 59M, inside noise), so
there is no reason to give up portability: v2 is Nehalem (2009) and Bulldozer
(2011) onward.

Both are now in `source/meson.build`'s `default_options`, with the `-march`
guarded by `host_machine.cpu_family() == 'x86_64'`, so a plain `conan build .`
picks them up.

### 5. Let the filter say which characters it can accept (+8.6%)

`children()` decoded all 23.5 children of a node and `has_transition()` then
rejected 74% of them. But a child's *character* is one byte at a known offset,
while its count and node offset are the variable-width fields behind it — so
rejecting on the character skips all of the expensive part.

`SearchFilter` gained `allowed_chars(state, CharSet*)`, asked once per step and
passed down to `children()`, which now tests a 256-bit mask instead of the
vestigial `min`/`max` range (the search always passed `CHAR_MIN, CHAR_MAX`).

`AnagramFilter` answers it exactly rather than approximately, running the same
two tests over the bag's distinct letters (~15) instead of over the node's
children (~23), from a compact `bag_letters[]` array built in the constructor.
That also lifts the two divisions by `product` out to once per step. The
default implementation admits everything, so `ExprFilter` and
`find-phone-words` are unchanged.

## What didn't work

Recorded because each looked obviously right beforehand.

- **A faster `log2f` — no gain, reverted.** `__log2f_fma` was 4.2% and is
  called ~7 times per step. A bit-trick exponent plus a degree-5 minimax
  polynomial (max error 3.2e-5, comparable to the float's own precision, and it
  telescopes rather than accumulating along a path) measured 61-64M against
  63M — nothing, possibly slightly worse. glibc's routine is already about as
  fast as five dependent FMAs, and the calls aren't on the critical path. Not
  worth the precision concession.
- **`GLIBC_TUNABLES=glibc.malloc.hugetlb=1` — no effect**, because this kernel
  produces no huge pages at all. See the caveat above; retry on real Linux.
- **A 4-ary heap** — see #2. Ruled out by measurement before being built.

## What's left

Final profile, at ~62.5M steps:

| Symbol | Share |
|---|---|
| `main` (LTO has absorbed `step()` and `has_transition()`) | 61.5% |
| `IndexReader::children()` | 29.7% |
| `__memmove_avx_unaligned_erms` | 4.5% |
| `__log2f_fma` | 4.2% |

Roughly in order of expected value:

1. **Huge pages**, on a kernel that has them. The frontier is multi-gigabyte
   and randomly accessed; this is the largest untested item and costs one
   `madvise` behind a custom allocator. Untestable on WSL2.
2. **`children()` at 29.7%** is now the largest single function. The existing
   `// TODO: binary search` in `index-reader.cpp` still stands — the child
   table is sorted by character, and the loop is linear. With the mask in
   place, a cheaper option may be to skip the fixed-size stride arithmetic
   entirely for masked-out entries.
3. **`memmove` at 4.5%** is bucket vectors growing. Chunked buckets (a deque or
   a hand-rolled block list) would remove the copying and cut the growth
   transient further.
4. **`Next` is 28 bytes**, and the bucket queue changes the arithmetic behind
   `findings/anagram-perf.md` idea #5: `operator<` is gone, so `log_score` is
   now read only to pick a bucket. That makes the 16-bit fixed-point score it
   describes a much easier trade than it was — the quantization it costs is
   already being paid by the bucket width.
5. **Goal-directed priority** — `anagram-perf.md` idea #3, untouched, and still
   the largest item overall. 3.2x more steps per second does not help if the
   steps are spent in the wrong part of the space.

## Related

- `findings/anagram-perf.md` — the frontier-size analysis this sits under;
  ideas #4 and #5 there are directly affected by the bucket queue.
- `findings/priority-invariant.md` — the pop-order invariant, which the bucket
  queue relaxes further.
- `findings/multithreaded-analysis.md` — the axis deliberately excluded here.
