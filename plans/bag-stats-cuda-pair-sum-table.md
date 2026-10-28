# Plan: bag-stats-cuda skips leftovers no group pair sums to

## Outcome

`bag-stats-cuda` builds a filter of every sum of two inner groups at
startup and checks each leftover against it before scanning. A leftover
the filter rules out has no pairs, so the warp counts it as a word bag
combination and moves on, skipping the half-length list scan. Output is
unchanged: the filter has no false negatives, and a leftover that passes
is counted exactly as today.

No new options. With `--progress`, two new lines:

```
bag-stats-cuda: pair-sum filter: N group pairs, M MB (B bits per pair), built in Ts
...
bag-stats-cuda: pair-sum filter: scanned N of M leftovers (P%)
```

## Why a filter, not a table of counts

A leftover's pair count depends only on its letters, so one global table
from each sum of two groups to its count would turn every scan into a
single lookup. It doesn't fit. On the current run (S1 minus one used
pair, 64 letters, `words.filtered`), 149,620 groups fit the bag and about
6.0 billion unordered group pairs fit it, estimated by sampling 1,500
groups. `plans/archive/bag-stats-cuda.md` found 7.2 billion on a
46-letter bag and rejected the table then. At 16 bytes a slot, even 2–3
billion distinct sums need 50–75 GB.

What does fit is a record of which sums exist. Nearly every leftover has
none: the prototype in `plans/archive/bag-stats-cuda.md` (46 letters,
pb,pb rows) found about 60 partner hits per row against 1,478 leftovers,
so at least 96% of leftovers scan the whole half-length list for nothing.
That scan is most of the kernel's work: on the prototype's heavy rows,
1,478 leftovers against about 2,000 half-length groups make about 3
million fit tests, against at most about 150,000 for the row's setup. (The
97.7% in `findings/archive/bag-stats-codex.md` is the CPU `bag-stats`
profile of the same loop.) A filter at 12 bits per pair takes about 9 GB
and answers each leftover with one 32-byte read.

## Keys

The filter is keyed by a leftover's key under the bag's packing, the
`BagKey` the kernel already uses for `group_keys` and `word_keys`, with no
guard bits set. These keys add: when `a + b` fits the bag, `key(a + b) =
key(a) + key(b)`, and a leftover's key is the row's `y` minus its word
bags' keys. So the build and the query compute keys by addition and
subtraction alone, whichever rest-local packing a row uses.

The hash is `mix_key(key)`, as for the per-row hash.

## The filter

A split-block Bloom filter in device memory: `filter_blocks` blocks of 8
`uint32_t` words (32 bytes, one sector). For hash `h`:

- the block is `home_slot(h, filter_blocks)`;
- word `k` (0–7) gets bit `(uint32_t(h2) * SALT[k]) >> 27`, where `h2 =
  mix64(h)` and `SALT` is Parquet's eight odd constants.

A key is present when all eight bits are set.

`GpuRowCounter` owns it, shared read-only by both streams, allocated after
the streams' buffers.

### Size

1. A counting kernel finds the number of unordered group pairs `{a, b}`
   whose sum fits the bag (with `a == b` only when `group_size(a) >= 2`).
2. `filter_blocks` is `pairs * 12 / 256`, rounded up, capped at what
   `cudaMemGetInfo` reports free less 512 MB, divided by 32 (0 when less
   than 512 MB is free), and at least 1. With no pairs, the one block is
   never written and rules out every leftover; with no room, it fills and
   lets every leftover through to a scan.
3. The startup line gives the pairs, the filter's MB and its bits per
   pair.

Pairs over-count distinct sums, since many pairs share a sum, so bits per
distinct key run higher than the line says.

### Build

One warp per group `a`, over all groups. Lanes stride `b` from `a` to
`lenstart[bag_length - length(a) + 1]`, both in `groups_by_length` order.
For each `b` that fits `bag - a` (`fits` on bag keys and `bag_guards`),
and with `b == a` only when `a`'s size is at least 2, set the eight bits
of `key(a) + key(b)` with `atomicOr`. The counting kernel is the same loop
with a warp-summed count in place of the inserts.

Both run once in the `GpuRowCounter` constructor, after the groups are
uploaded, timed on the host clock through a `cudaDeviceSynchronize`. The
filter is allocated between them and cleared with `cudaMemset` before the
build kernel sets any bits, since `cudaMalloc` leaves it uninitialized.

## Query

`count_rows` keeps each depth's leftover under the bag's packing next to
its rest-local key:

- the per-block word list gains a `BagKey` array, written in the loop
  that fills the word list, from `p.word_keys[j]`, alongside the
  rest-local key and length;
- the word walk keeps `bag_rests[depth]`, `y` minus the bag keys picked so
  far, beside `rests[depth]`;
- `count_leftover` takes the leftover's bag key.

In `count_leftover`, after `++word_bags`, the empty-leftover `solo` case
and the `2 * min_group_length` check, and before the scan:

- add 1 to the warp's leftovers count;
- every lane computes the hash, and lanes 0–7 each read their word of the
  block and test their bit;
- every lane votes `lane >= 8 || bit` in `__ballot_sync(~0u, ...)`, so
  the whole warp takes the same branch; if the vote isn't `~0u`, return;
- otherwise add 1 to the warp's scanned count and scan as today.

The two counts are per-warp locals declared once before `count_rows`' row
loop, so they run across all the block's rows, and `count_leftover`
takes a pointer to them beside `counts`. When the block runs out of rows,
lane 0 of each warp adds them to the stream's two-entry device array with
`atomicAdd` before returning. `Shared` is unchanged.

## Reporting

`GpuRowCounter` gains:

```cpp
// The pair-sum filter: the group pairs it was sized for, its device
// bytes and the seconds its two kernels took.
struct GpuFilterInfo {
    uint64_t pairs;
    size_t bytes;
    double seconds;
};
GpuFilterInfo filter_info() const;

// The leftovers checked against the pair-sum filter and those it let
// through to a scan, over every count() so far.
struct GpuFilterCounts {
    uint64_t leftovers;
    uint64_t scanned;
};
GpuFilterCounts filter_counts() const;
```

With `--progress`, `main` prints the filter line from `filter_info()`
right after the existing startup line, with bits per pair `bytes * 8 /
pairs` (0 with no pairs). The filter goes through `allocate`, so
`bytes()` and the existing line's MB include it.

Each `Stream` has its own two-entry device array and a two-entry pinned
host copy. `count()` clears the array with a second `cudaMemsetAsync`
beside the `next` one, so it covers both launches of a 128-bit batch, and
copies it back with `host_out`.
After the stream synchronizes, it adds the copy to `Impl`'s totals in the
locked block that clears `busy`; `filter_counts()` reads them under the
same mutex. `main` in `bag-stats-cuda.cpp` keeps `run_bag_stats`'
return value and, with `--progress`, prints the `scanned N of M
leftovers` line before returning it.

## Memory

On this run: the existing buffers, about 1.4 GB, plus the filter, sized to
leave 512 MB free: about 9 GB at 12 bits per pair, within the roughly
10 GB left on the 12 GB card.
The word lists' `BagKey` arrays add `blocks × word_count × 16` bytes per
stream, about 25 MB on this card's 58 SMs.

One run with a large filter owns the card. A second `bag-stats-cuda`
started beside it gets whatever filter fits after its own buffers: with
less than 512 MB free, the one-block filter, which fills and scans every
leftover as today, shown by its bits per pair. With too little room for
its stream buffers, it exits with CUDA's out-of-memory error.

## Tests

`test-bag-stats-cuda.sh` already compares `bag-stats-cuda` with
`bag-stats` byte for byte, which covers the filter dropping no leftover
that has pairs, but only on a 22-bit bag. One more comparison runs on a
78-letter bag, each of a–z three times, which packs in 78 bits, so that
one run launches both the `<Key128, Key64>` and `<Key128, Key128>`
kernels and each has leftovers with pairs.

Its own pairs file has two pairs:

- `vwxyzvwxyz,vwxyz` takes every v–z, leaving a–u three times each:
  63 bits, a narrow row;
- `cat,dog` leaves every letter at 2 or 3: 78 bits, a wide row.

It reuses the DICT file with `--min-pb-len 6 --min-wb-len 3` and the
default `pb,wb,p`. The shared `words.filtered` gains four long words,
two pairs whose sums are leftovers of those rows:

- narrow, word bag `ant`: a–u twice (42 letters) and a–u once less `a`,
  `n`, `t` (18 letters);
- wide, word bag `low`: a–z twice less `l`, `o`, `w` (49 letters) and
  a–z once less `a`, `c`, `d`, `g`, `o`, `t` (20 letters).

All four are longer than the existing 15-letter bag, so they fit none of
its rests and the existing comparisons' output is unchanged.
`run_both`'s bag and pairs file become its first two arguments.
