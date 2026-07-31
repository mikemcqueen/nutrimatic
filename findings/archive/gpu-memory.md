# Using GPU memory against the memory wall

The question that prompted this: as the frontier and crumbs grow toward
available RAM, disk reads rise sharply — presumably the page cache holding the
1.2 GB index being squeezed out. Would keeping the index in VRAM and pulling
node bytes back over PCIe beat going to disk for them?

**Short answer: the diagnosis is right, the cure is wrong, and the right cure is
one `mlock` away.** PCIe does beat a disk seek, but that is the wrong
comparison — moving the index to VRAM makes *every* lookup pay PCIe latency
instead of only the misses, which is a net loss at any plausible hit rate and a
hard ~10x throughput regression at the step rate this search actually runs at.
There *is* a good use for the 12 GB sitting idle on the GPU, but it is the
opposite of the one proposed: the **frontier** belongs in VRAM, and the index
belongs pinned in RAM.

Nothing here is measured — the idea asked for theory. The hardware facts below
were read off the machine; everything downstream of them is reasoning, and the
last section says what to measure first to check it.

## Findings at a glance

- **The mechanism is sharper than "the cache shrinks."** The frontier is
  anonymous memory, reclaimable only into 4 GB of swap; the index is file-backed
  and clean, so reclaiming it costs *nothing*. The kernel evicts the index first
  because it is the cheapest thing in the system to throw away. Readahead
  compounds it. → *Why the disk reads rise*
- **PCIe really does beat a disk seek** — ~5 µs native, likely 20-50 µs through
  WSL2's GPU-PV, against ~50-150 µs for a major fault. The premise holds on its
  own terms. → *The question as asked*
- **But it replaces a hit rate with an unconditional cost.** Break-even is around
  a **95%** page-cache hit rate, which a 1.2 GB working set in 15.5 GB of RAM
  should never approach. → *The question as asked*
- **The throughput ceiling disqualifies it outright.** 2.1M steps/s × one serial
  `children()` per step means a 5 µs round trip caps the search at 200k steps/s —
  a 10x regression with a *free* GPU side. → *The throughput ceiling*
- **The actual fix is `mlock` + `MAP_POPULATE` + `MADV_RANDOM`.** The index is 8%
  of RAM and the kernel merely doesn't know it is precious. Note `ulimit -l` is
  64 KB here. → *What actually fixes the stated problem*
- **The inversion: VRAM is the right home for the frontier, not the index.** Cold
  buckets below the cursor are write-once, read-once and in order — ideal for
  PCIe — while the index is fine-grained random, the worst case. → *The inversion*
- **The one genuine argument for GPU over disk:** spilling 10 GB of frontier
  through the page cache would evict the index, i.e. cause the original problem.
  VRAM leaves the page cache untouched. → *The one real argument for the GPU over
  disk*
- **A concrete result for `ideas/pq-persistence.md`:** the worst per-step drop is
  **~508 buckets**, so anything beyond `cur + 508` is provably closed to further
  pushes — and a one-way cursor means re-admission needs **no merge at all**. That
  machinery was an artifact of the binary heap. → *Which buckets are safe to
  spill*
- **Weigh this against the algorithm work:** ~3x frontier buys ~3x steps against
  an exponential space. `anagram-perf.md` #3 remains the larger item. → *Does more
  frontier actually help?*
- **Measure one thing first:** `majflt` vs. `vmstat`'s `si`/`so`. "Disk reads
  increase" is equally consistent with the *frontier* being swapped, and `mlock`
  does nothing for that case. → *What to measure first*

## The hardware being reasoned about

| | |
|---|---|
| RAM | 15.5 GB (`MemTotal: 16200764 kB`), 4 GB swap, `swappiness=60` |
| GPU | RTX 4080 Laptop, **12 GB VRAM, 0 MiB in use**, not driving the display |
| Link | PCIe Gen4, x16 max, x8 at idle — ~25 GB/s at x16, ~13 GB/s at x8 |
| Index | `idx/wiki-merged.5.index`, 1.21 GB |
| Mapping | `mmap(PROT_READ, MAP_SHARED)`, `index-reader.cpp:15` — no `madvise`, no `mlock` |
| `ulimit -l` | **65536** (64 KB) |

Two environment facts that matter as much as the numbers:

- **This is WSL2.** There is no `/dev/nvidia*`, only `/dev/dxg`: CUDA runs
  through GPU-PV to the Windows driver, so every API call is a paravirtualized
  round trip and small-transfer latency is materially worse than native. This is
  the same environment that `findings/perf-analysis.md` found has no PMU and no
  transparent huge pages. Any GPU work here is being prototyped on the worst
  case of the platform.
- **The index is 1.2 GB out of 15.5** — 8% of RAM. Nothing about this problem is
  a capacity problem for the index. It is entirely a *policy* problem.

## Why the disk reads rise: the kernel is evicting the index on purpose

The mechanism is sharper than "less RAM, so less cache". Reclaim is not
symmetric between the two things competing here:

- The frontier is **anonymous** memory. To reclaim a page of it the kernel must
  write it to swap, and there are only 4 GB of swap for a multi-gigabyte
  frontier.
- The index is **file-backed and clean**. Reclaiming a page of it costs
  *nothing* — no writeback, just drop it. Re-reading it later is a major fault
  against the index file.

So when pressure arrives, the index is the cheapest thing in the system to
throw away, and the kernel throws it away first. `swappiness` tunes the balance
but not that asymmetry. The observed symptom — frontier grows, disk reads climb
— is exactly what this predicts.

Default readahead makes it worse in both directions. The mapping is
`MADV_NORMAL`, so a major fault on one randomly-placed trie node drags in ~128 KB
of neighbours that the walk has no reason to visit. That wastes bandwidth on the
fault *and* consumes cache that displaces pages the search does want, which
accelerates the eviction that caused the fault.

## The question as asked: PCIe latency vs. disk latency

Order-of-magnitude figures for this machine:

| Path | Latency |
|---|---|
| Page-cache hit (already mapped) | ~80-100 ns, no syscall |
| Minor fault (resident, not mapped) | ~1-2 µs, once per page |
| Major fault → NVMe through the VHDX | ~50-150 µs |
| `cudaMemcpy` D2H, small block, native Linux | ~5-10 µs — launch/submit, not wire time |
| Same, through WSL2 GPU-PV | worse; plausibly 20-50 µs |

So yes: **PCIe beats a major fault**, probably by 2-10x on this box. The idea's
premise survives on its own terms, and the guess that VRAM has no hidden caching
layer is also correct — a D2H copy is honest DMA.

But 2-10x is not what the idea needs, for two reasons.

### It replaces the hit rate with an unconditional cost

`mmap` gives a *hit rate*. VRAM does not: every `children()` call becomes a
round trip. With hit rate `h`, average cost per lookup is
`h × 0.1 µs + (1 − h) × 100 µs`, against a flat ~5 µs for VRAM (already
optimistic here):

| h | mmap | VRAM | winner |
|---|---|---|---|
| 0.99 | 1.1 µs | 5 µs | mmap, by 4.5x |
| 0.95 | 5.1 µs | 5 µs | tie |
| 0.90 | 10 µs | 5 µs | VRAM, by 2x |

Break-even sits near a **95% hit rate**, and the whole argument for the idea is
that the hit rate is falling — so it is not absurd. But at a WSL2-realistic
25 µs the break-even drops to ~75%, which is a hit rate a 1.2 GB working set in
15.5 GB of RAM should never approach. The trade is bad exactly where the machine
is, and only becomes good in a regime that shouldn't be allowed to exist.

### The throughput ceiling is the harder wall

`findings/perf-analysis.md` measures ~62.5M steps in 30 s — **2.1M steps/s**,
with one `children()` call per step, and the search is best-first and serial:
step *n+1*'s node is not known until step *n* pops. A synchronous 5 µs round
trip per step caps the whole search at **200k steps/s**, a 10x regression even
if the GPU side of the lookup were free. Nothing about VRAM's capacity or
bandwidth enters into it; per-step synchronous PCIe is disqualifying by itself.

## What could rescue it — and why that argument ends up against the GPU

The only escape from a per-step round trip is issuing many at once, and the
search does have batch structure available: **every entry in `nexts.buckets[cur]`
is within 1/16 of a log2 unit and already pops LIFO**, so the order among them
is a tolerance the tree has already conceded (`NextQueue` in `source/search.h`,
`findings/perf-analysis.md` §2). Popping *k* of them together and fetching all
*k* nodes in one transfer stays inside an approximation that already ships.

At k=1024 and ~128 B/node that is a 128 KB transfer: ~10 µs of wire time plus
~20 µs of overhead, or **~30 ns/node amortized**, which is DRAM-competitive.

The trouble is that batching is not a GPU idea. The same *k* known-in-advance
node addresses make every cheaper option viable too:

- `madvise(MADV_WILLNEED)` over the batch before decoding it, keeping `mmap`
  exactly as it is — a dozen lines, no new dependency;
- `io_uring` at high queue depth, if it really has to come from disk.

Batching is the real insight here. Once you have it, VRAM adds nothing the page
cache doesn't do better and for free.

## What actually fixes the stated problem

The index is 8% of RAM and the kernel merely doesn't know it is precious. In
increasing order of effort:

1. **`mlock()` the mapping** (with `MAP_POPULATE` to fault it in up front).
   Pins 1.2 GB; the frontier then competes with swap instead of with the index.
   *Gotcha:* `ulimit -l` is **65536** here, so the call fails past 64 KB until
   `RLIMIT_MEMLOCK` is raised (`limits.conf`, systemd, or `CAP_IPC_LOCK`).
   Failure should warn and carry on, not exit — the tool must still run
   unprivileged.
2. **`madvise(MADV_RANDOM)`** on the mapping. The trie walk has no locality
   worth prefetching for, and readahead under pressure is actively harmful (see
   above). One line, no privileges.
3. **If `mlock` can't be had, read the index into an anonymous 1.2 GB buffer**
   instead of mapping it. That puts it on the same reclaim footing as the
   frontier rather than making it the cheapest victim; it can still be swapped,
   but it will cost 4 GB of swap traffic before it loses a byte.

This should be built and measured **before** anything involving a bus. It is
plausibly the entire observed problem, and it is a handful of lines.

## The inversion: VRAM is the right place for the *frontier*

PCIe rewards bulk sequential transfer and punishes fine-grained random latency.
Sort the two data structures by that criterion and the idea flips:

- The **index** is randomly accessed at ~128-byte granularity, latency-bound,
  2.1M times a second. Worst possible PCIe workload.
- The **frontier below the cursor** is write-once, read-once, in cursor order,
  and untouched in between. `settle()` already hands each bucket's storage back
  as the cursor passes it. Best possible PCIe workload.

So: pin the index in RAM, and spill cold buckets to VRAM. 12 GB at 28 bytes per
`Next` is **~430M entries**, against the 109M the bucket queue reached under a
6 GB cap. Transfers are megabyte-scale and sequential, so the ~13-25 GB/s figure
genuinely applies — a 1 GB spill costs 40-80 ms against minutes of search.

### The one real argument for the GPU over disk

`ideas/pq-persistence.md` proposes the same offload to disk, which is the
obvious alternative and needs no CUDA. But **writing 10 GB of frontier through
the page cache would evict the index — the exact problem being solved.** Spilling
to VRAM leaves the page cache untouched. Disk can match that only with
`O_DIRECT` and its alignment plumbing, which is roughly the complexity of a
`cudaMemcpy` anyway. That asymmetry is the whole non-obvious case for using the
GPU here, and it is a decent one.

### Which buckets are safe to spill: a provable window

The awkward part of any spill is that a bucket can still receive pushes after
being evicted. For this queue that is answerable exactly, because the per-step
drop in bucket index is **bounded**:

- A character transition holds the scale and can only shrink the count, so it
  drops at most `log2(count()) = log2(3.586e9) ≈ 31.7` log2 units.
- A restart adds `log2(1e-6) ≈ −19.93`.

At `SCALE = 16` buckets per log2 unit, the worst single-step drop is **~508
buckets**. So a pop at cursor `c` can only push into `[c, c+508]`, and **any
bucket beyond `cur + 508` is provably closed**: it will receive no further
pushes, and it is not needed until the cursor arrives. That is the spill window,
it is checkable at runtime as `ceil(log2(reader->count()) * SCALE)`, and it is
tiny next to the ~16000 buckets the score range spans.

This also simplifies `ideas/pq-persistence.md` considerably, and that document
should be updated: because the queue is monotone with a one-way cursor, "the
bottom half" is not something that has to be found, tracked by top score, and
merged back on re-admission. It is a contiguous range of buckets far below the
cursor, and re-admission is just the cursor arriving — **in order, with no merge
at all**. The merge machinery that sketch worries about was an artifact of the
binary heap, and it died with it.

One implementation wrinkle: pushes land in buckets *inside* the window
constantly, so a bucket can't be a single vector that gets shipped out whole.
Make each bucket a RAM-resident open block plus a list of full blocks, and spill
only full blocks once the bucket is outside the window.

Caveats on the 12 GB: it is shared with anything else using the GPU, and WSL2
carves some for the WDDM path even on a card that isn't driving the display.
And RAM (15.5 GB) and VRAM (12 GB) are the *same order of magnitude* here — this
is a ~1.8x capacity win, not a change of category. On a machine with 128 GB of
RAM the idea would not be worth having.

## Does more frontier actually help?

Honestly: not much, and this is the strongest reason to not do any of it yet.

`findings/anagram-perf.md` idea #3 and `findings/perf-analysis.md`'s "what's
left" both conclude that the binding problem is *what* the search spends steps
on, not how many it can hold. The frontier grows ~5 entries per step, so ~3x
capacity buys ~3x steps against a space that is exponential in letters — perhaps
one or two more letters of reach. Goal-directed priority (#3) and a frontier
bound (#4) change the base of that exponent. Building a VRAM spill first spends
the hardest engineering in the document on the smaller constant factor.

## Running the search itself on the GPU

Rejected quickly, for completeness. SIMT is the wrong shape for this workload at
every level:

- `children()` is a variable-width byte decode where `count_size` and
  `offset_size` are derived per node (`index-reader.cpp:70-119`), so threads in
  a warp diverge on essentially every node;
- the reads are single-byte gathers at unrelated offsets — uncoalesced by
  construction;
- each expansion emits a variable number of survivors, needing a warp-wide scan
  and dynamic output allocation;
- `seen` is a `std::set<std::string>` and the crumb chain is a pointer-linked
  history mutated every step, both host-side and both awkward to move.

This is also a codebase where 22% came from two build flags. The CPU version is
nowhere near the point where the answer is a different processor.

## What to measure first

The idea said not to measure, and nothing above is measured — but one confound
should be resolved before any of this is built, because it changes the answer
completely. "Disk reads increase substantially" is consistent with **two**
different failures:

- **index pages being evicted and re-faulted** — the theory above, fixed by
  `mlock`;
- **the frontier itself being swapped out** — same "disk is busy" observation,
  and `mlock` does nothing for it. Only bounding the frontier helps.

One command distinguishes them: watch `majflt` in `/proc/<pid>/stat` against
`si`/`so` in `vmstat 1`. Major faults climbing with quiet swap means the index;
swap-in/swap-out traffic means the frontier. `/proc/pressure/memory` corroborates
either.

## Recommended order

1. Confirm which failure it is (`majflt` vs. `si`/`so`).
2. `madvise(MADV_RANDOM)` + `mlock`/`MAP_POPULATE` on the index, with a graceful
   failure when `RLIMIT_MEMLOCK` is low. Re-measure. This may end the problem.
3. Goal-directed priority — `findings/anagram-perf.md` #3. Still the largest
   item overall, and it reduces the frontier rather than relocating it.
4. Batch the pop: expand *k* entries from `buckets[cur]` at once. Wins on its
   own via `MADV_WILLNEED` prefetch, and is the prerequisite for any offload
   design being worth building.
5. Only then, VRAM as a frontier spill target, using the `cur + 508` window.

**Do not** put the index in VRAM. That is the one form of the idea the analysis
rules out rather than merely deprioritizes.

## Related

- `ideas/pq-persistence.md` — the same offload aimed at disk; the bucket-queue
  cursor removes the merge step that sketch is built around, and the page-cache
  argument above is the case for VRAM over disk as the target.
- `findings/perf-analysis.md` — the bucket queue, the 2.1M steps/s figure the
  latency budget is drawn from, and the WSL2 limits (no PMU, no THP).
- `findings/anagram-perf.md` — ideas #3 and #4, which reduce the frontier rather
  than finding somewhere else to put it.
- `findings/multithreaded-analysis.md` — the other "throw more hardware at it"
  axis, reaching a similar conclusion about where the real limit is.
