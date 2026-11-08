# bag-stats-cuda: where the time goes, and what would make it fast

An analysis of three `pb,pb,wb,p` runs, with the measurements behind it,
and the changes that would turn minutes into seconds. Only change 1 is
built (S1 −bglllmmpu: 409.9s to 213.9s, output identical; S9 unchanged);
every projected time is an estimate from the measurements, marked as such.

The three runs (`seed` = `seed-pairs -s N -m 4`, `--threads 0 -n 5000000
--wbfrac-min --mp --wbwp-min --ss pb,pb,wb,p --cache-size 5000`):

| Run | Bag | Pair bags | Word bags | Groups | Rows | Distinct rests | Rest letters | Wall |
|---|---|---|---|---|---|---|---|---|
| S9 | 47 letters | 20,765 | 3,649 | 34,822 | 20.05M | 14.3M (71%) | 8–29, peak 26 | 31s |
| S1 −bglllmmpu | 81 | 28,712 | 2,836 | 30,585 | 302M | 205M (68%) | 34–63, peak 56 | 428s |
| S1 −glllpu | 84 | 68,207 | 4,365 | 54,621 | 1.617G | 1.065G (66%) | 35–66, peak 59 | ~45m (not run) |

Rows and distinct rests are from `bag-stats-cuda --leftovers --hll --ss
pb,pb`, which walks all 1.6G rows of the third run on the GPU in 0.7s of
wall time. Word bags are the DICT bags of 5+ letters that fit the bag;
groups are the inner-dictionary groups that fit.

## 1. Measurements

### S9 (31s)

`nsys` kernel sums over the run, 313 batches:

| Kernel | Time | Share |
|---|---|---|
| `count_pairs` | 9.5s | 84% |
| `count_leftovers` | 1.6s | 14% |
| filter build | 0.2s | 2% |

- Pass 1: 19s. Pass 2: 10s with 100% RestTable hits, so pass 2 does no
  GPU work at all: 10s is one CPU thread walking 20M rows, looking each
  rest up and feeding the 5M-row heap, about 500ns a row. Pass 1 is that
  same thread plus ~11s of GPU, serialized: the walker thread blocks in
  `count()` while the kernels run, and the GPU idles while it walks.
- 5.55G leftovers (277 per row) were checked against the pair-sum
  filter; 50.02M passed (0.9%). A full row dump shows 50.01M leftovers
  with pairs: the filter is 99.98% precise. `count_pairs` therefore spent
  9.5s re-deriving, by list scans, 50M counts that are all real, about
  190ns each.
- 4.57M of the 20M rows have any pairs.

### S1 −bglllmmpu (428s)

7,822 batches (302M rows in pass 1, 111M re-counted in pass 2):

| Kernel | Time | Share |
|---|---|---|
| `count_leftovers` | 248s | 97% |
| `count_pairs` | 7.5s | 3% |
| filter build | 0.3s | |

- 742G leftovers were checked against the filter, about 1,800 per
  counted row, at 3.0G checks/s. 4.0M passed. The top-5M output has
  exactly one row with pairs (`hawaiian entertainer + weakened finances`,
  1 pair); the other 4,999,999 rows are tied at zero and ordered by walk
  order. The run computes WORDBAGS and one row.
- Pass 1: 248s; pass 2: 163s at 63% cache hits. The 5,000MB RestTable
  holds 109M entries at 48 bytes each, against 205M distinct rests, so it
  filled a third of the way through pass 1 and pass 2 sent 111M rows back
  to the GPU (about 69s of it).
- CPU side, serial, about 170s of the 428s.

### Why the S1 leftovers have no pairs

A leftover has pairs only if it is the sum of two inner-dictionary
groups. The sums of two groups that fit each bag, counted on the GPU
(one warp per group, HyperLogLog per sum length, `scratchpad/sums/`):

| Bag | Group pairs | Distinct sums | Sums of 27+ letters | Sums of 33+ letters | Longest sum |
|---|---|---|---|---|---|
| S9 | 304M | 60.0M | 29.7k | 2.0k | 33 |
| S1 −bglllmmpu | 429M | 111M | 2.0M | 61k | 41 |
| S1 −glllpu | 1.32G | 368M | 9.1M | 100k | 43 |
| S1 (all 90 letters) | 21.5G | 6.03G | 500M | 15M | 52 |

The group-pair counts match the `pair-sum filter` line of each run. On
S1 −bglllmmpu the rests peak at 56 letters and DICT word bags peak at 6–8,
so a typical leftover has 48–50 letters, and there are no sums that long.
Only the few long word bags (15+ letters: about 55 bags) can leave a
leftover short enough to be a sum, and only from the shortest rests. The
kernel probes every one of the 742G leftovers anyway, one random 32-byte
DRAM read each.

### An exact table of sums fits

Distinct sums are 4–5× fewer than group pairs, and a `sum → pair count`
hash table fits the card for every `-u` bag tried (open addressing, 8-byte
key hash + 4-byte count, load 0.5, built by the same warp-per-group
enumeration as the filter, with `atomicCAS`/`atomicAdd`):

| Bag | Slots | Bytes | Build | Random probes, DRAM-resident | Probes, L2-resident |
|---|---|---|---|---|---|
| S9 | 120M | 1.4GB | 0.35s | 2.75G/s | 23G/s |
| S1 −bglllmmpu | 223M | 2.5GB | 0.51s | 2.73G/s | 18G/s |
| S1 −glllpu | 736M | 8.4GB | 1.6s | 2.70G/s | (209MB, 3.3G/s) |

A probe of the exact table costs the same as a probe of today's filter
(both are one random sector; the filter kernel runs at 3.0–3.4G/s), and
it returns the pair count, so the candidate scan has nothing left to do.
The full 90-letter S1 does not fit (6G sums); it doesn't fit today's filter
either (21.5G pairs at 12 bits is 32GB, clamped to what's free).

## 2. Root causes

1. **Leftovers that cannot have pairs are probed.** On the long-rest
   runs, 97% of GPU time is 742G DRAM probes whose answer is known from
   the leftover's length alone. Sums of 33+ letters number 2k–100k; sums
   of 27+ are at most 9M on these bags.
2. **Known counts are recomputed by scanning.** On S9, every leftover the
   filter passes has pairs, and `count_pairs` spends 84% of GPU time
   finding the count by scanning the row's half-length group list, when
   a 1.4GB exact table answers in the probe already made.
3. **One CPU thread drives everything, in lockstep with the GPU.**
   `Walk::threads` is `--streams` (1); `--threads` only sizes the census.
   That thread walks rows, looks rests up, feeds the ranker's heap, and
   blocks on each batch's `cudaStreamSynchronize`. It costs 10s per pass
   on S9 and about 85s per pass on S1 −bglllmmpu, against 0.4–0.7s for the
   GPU to walk the same rows.
4. **Pass 2 repeats pass 1.** The ranker's key depends only on the rest
   (counts, and the pair length, which is the bag less the rest), but
   pass 2 re-walks every row and re-counts whatever the RestTable
   dropped. The table is bounded by host RAM: 109M entries in 5GB; S1
   −glllpu's 1.06G distinct rests would need 50GB.
5. **Work is per row, not per distinct rest.** 29–34% of rows repeat a
   rest within the same pass.
6. **Batches are small and synchronous.** 65,536 rows per launch, 313 to
   7,822 launches per run, each with a host sort, H2D, D2H and a sync.

## 3. Changes, in order of gain per effort

Numbered so they can be referred to by number.

### 1. Don't probe leftovers longer than the longest sum

In `count_leftovers`, skip the probe when the leftover's length exceeds
the longest sum (the table or filter build knows it). Better: walk the
row's word bags longest first and stop probing once `|rest| − |word| >
max_sum_length`; the fit test still runs on every word bag for WORDBAGS
and SOLO. On S1 −bglllmmpu, rests of 56 letters then probe only the ~55
word bags of 15+ letters instead of ~1,800.

- Kernel-local change; output identical.
- Estimate: S1 −bglllmmpu GPU 256s → under 15s; wall 428s → ~185s (then
  CPU-bound, see 3). S9 unchanged (its leftovers are 0–24 letters, all
  within range).

### 2. Replace the pair-sum filter and `count_pairs` with the exact sum table

Build the `sum → count` table in place of the filter (same enumeration
kernel, `atomicCAS` on a 64-bit key hash, `atomicAdd` on the count; keep
the exact packed key where it fits 64 bits, else the 64-bit hash of the
128-bit key). `count_leftovers` reads the count from the probe and
accumulates `with_pairs`, `total_pairs`, `max_pairs` and `with_min_pairs`
per warp, as it already accumulates `word_bags` and `solo`. `count_pairs`,
`flush`, the candidate buffers, the per-block group lists, `flagged`, the
group hash and `list_capacity` all go: about 400 lines of `bag-stats-gpu.cu`.

Tier the table by sum length: one sub-table per length, each sized to its
own count, so the sub-tables for 27+ letters (24MB on S1 −bglllmmpu) stay
in L2 and run at 18–23G probes/s. Long rests only ever probe those.

- Memory: 1.4GB (S9) to 8.4GB (S1 −glllpu) at load 0.5 and 12 bytes a
  slot; 8-byte slots (40-bit fingerprint, 24-bit count with an overflow
  side table) or load 0.7 bring S1 −glllpu to 4–6GB. Bags with more than
  ~600M sums (the full S1) need a fallback: table the lengths that fit,
  keep the filter and scan for the rest, or refuse.
- Estimate: S9 GPU 11s → ~2s (5.5G probes at 2.7–3.4G/s, nothing else);
  with 1, S1 −bglllmmpu GPU → a few seconds.

### 3. Walk, dedupe, count and rank on the GPU; the host formats the output

The `--leftovers` kernel already walks every `pb,pb` row of S1 −glllpu
in 0.7s. Keep the rows on the device:

1. **Walk.** The pb walk kernel writes each row's rest key (packed, 64 or
   128 bits) and its (unit, seq) to a device buffer, in chunks of a few
   hundred million rows (units never split).
2. **Dedupe.** Sort the chunk's keys (CUB radix sort: ~0.3s per 300M
   64-bit keys) and count each distinct rest once. For S1 −glllpu the
   1.06G distinct rests don't fit any table at once, so dedupe is per
   chunk; the per-rest work after 1 and 2 is cheap enough (a 4.4k-word
   fit scan and a few probes) that recounting across chunks costs seconds,
   not minutes.
3. **Count.** `count_leftovers` with 1 and 2, one row per distinct rest.
4. **Rank per rest.** The default ranker's key is a function of the rest
   alone, so the pass-1 distributions come from the per-rest sort values
   weighted by each rest's row count (a device sort and reduce), and the
   top-N threshold key from the joint (value triple → row count)
   histogram. `--by-length` and `--by-pick` keep their per-row semantics:
   by-length groups by pair length, also a function of the rest; by-pick
   needs the picks and keeps a row-level pass.
5. **Select.** Re-walk the rows on the GPU (0.4–0.7s), keep those whose
   rest key is at or above the threshold (a lookup in a device hash of at
   most N qualifying rests, L2-sized), sort the survivors by (key desc,
   unit, seq), cut to N. `seq` is the row's position in its unit's walk,
   reproducible from a per-unit prefix count, so ties break as today and
   the output stays byte-identical.
6. **Output.** The host receives N (unit, pick, counts) records and
   formats the lines on all threads: about 1s for 5M rows.

This removes `GpuRowWalker`, the RestTable and `--cache-size` from
bag-stats-cuda, the second counting pass, the 65k-row batches and the
host sort per batch. `--score`, `--best-pairs` and `--top-segments` need
the picks of the selected rows, which step 5 carries.

- Estimates, with 1 and 2: S9 ~3s (0.5s load, 0.35s table, 3.9G probes
  ≈ 1.3s, ranking and selection ~0.3s, output ~1s); S1 −bglllmmpu ~6s;
  S1 −glllpu 15–25s (1.6G rests × 4.4k fit tests ≈ 4s; ~130G probes,
  nearly all in the L2 tier, ≈ 7s; chunked sorts; output).
- The honest floor on S9 is the probe count: 3.9G random DRAM reads at
  ~3G/s. Under 2s needs fewer probes, not faster ones (see 5).

### 4. In the meantime: multithreaded walkers and a feeder thread

`plans/archive/bag-stats-cuda-new-arch.md` already describes this:
`--threads` walkers claim rests in the RestTable, a feeder owns the
streams, nobody blocks on the GPU. It cuts the CPU side about 10× and
overlaps it with the GPU, but keeps the host RestTable and its size
cliff, so S1 −glllpu still re-counts most rows in pass 2. Worth doing
only if 3 is deferred.

### 5. Smaller items

- A 2–4 bit-per-sum Bloom filter in front of the exact table, sized to
  L2 (S9: 60M sums × 4 bits = 30MB), answers ~70% of misses without a
  DRAM read. Gain ≤2× on the probe-bound S9; nothing on long-rest runs
  after 1.
- The WORDBAGS fit scan is 2.8–4.4k fit tests per rest. Per-unit
  prefiltering (word bags that fit `bag − pb1`) and the bitset method of
  `findings/faster-bag-subset.md` (26 ANDs over per-letter-count bitsets
  and a popcount, instead of a fit test per word bag) matter once rests
  are short and units share most of their word list, as in 3-pb runs.
- `count_leftovers` already issues one probe per lane per step; a probe
  benchmark with four independent probes in flight per lane reached the
  same 2.7–2.8G/s, so DRAM random access, not latency hiding, is the
  limit. Peak is 13.5G sectors/s; random access reaches ~25% of it.

## 4. Three and four pb steps

Rows grow as (pair bags)^k / k! times the fraction that fit, but distinct
rests grow much more slowly once the rests are short, because short
sub-bags of the bag are few. Measured (`--leftovers --hll`) and projected;
rows here are pick combinations (`COMBINATIONS`), not the PAIRS-weighted
`ROWS`:

| Run | 2 pb rows | 2 pb distinct rests | 3 pb rows | 3 pb distinct rests | 4 pb rows |
|---|---|---|---|---|---|
| S9, 20.8k bags | 20M | 14.3M (71%) | 687M (GPU walk 1.4s) | 64.1M (9.3%) | ~10^10 |
| S1 −bglllmmpu, 28.7k bags | 302M | 205M (68%) | 9.79×10^11 (GPU walk 80s) | 28.5G (2.9%), 17–54 letters, peak 44 | ~10^15 |
| S1 −glllpu, 68.2k bags | 1.6G | 1.07G (66%) | 1.18×10^13 (GPU walk ~16m) | 204G (1.7%), 16–57 letters, peak 47 | 2–4×10^16 |

The 3-pb S1 runs walk at 12.2–12.3G combinations/s. About a third of the
third picks fit (0.34 and 0.32), so the 4-pb column assumes 10–20% of the
fourth picks fit, as the rests are shorter: (3-pb combinations) × (pair
bags / 4) × 0.1–0.2. On S1 −glllpu, 2.83×10^12 of the 3-pb combinations
(24%) and ~75G of the distinct rests are 43 letters or fewer, the longest
sum for that bag; 1.8×10^10 combinations and 3.4G distinct rests are 33
or fewer.


With 1–3 built, each row costs a GPU walk step (~12G rows/s measured
on the 3-pb walks) and each distinct rest a fit scan plus, once rests are
short enough to have pairs, a probe per fitting word bag (hundreds, at
~3G/s). So:

- **3 pb on S9**: 687M rows but 64M distinct rests of 1–20 letters. A
  rest-centric run is about 64M fit scans and ~6G probes: ~5s of GPU,
  about what the 2-pb run takes. This is the case that 3 pays off in.
- **3 pb on S1 −bglllmmpu**: 9.8×10^11 rows, walked on the GPU in 80s,
  with 28.5G distinct rests (2.9%). Today's ~1.4µs a row projects to two
  weeks. Under 1–3 the cost is per distinct rest: 28.5G fit scans of 2.8k
  word bags (8×10^13 tests, ~40s) and probes for the ~10^10 rests of 41
  letters or fewer, ~10^13 of them, mixed DRAM and L2 tiers: an hour or
  more. And 28.5G rests × 8 bytes is 228GB of keys, so dedupe can only
  be per chunk, where the distinct fraction is far above 2.9%; the
  cross-chunk repeats get recounted. Hours, not tens of minutes. The
  lever is the rest length: most of those 28.5G rests are 42+ letters
  and can't have pairs at all; more `-u` letters or shorter lists move
  the whole distribution.
- **4 pb on these lists** is out of reach for any design that visits rows
  or distinct rests: walking alone is about a day on S1 −bglllmmpu and
  20–40 days on S1 −glllpu. A 4-pb run in today's 2-pb budget (tens of seconds
  after 1–3) needs pair lists of 1–2k bags: 1k^4/24 × fit ≈ 10^10 rows,
  ~1s to walk, ~10^12 probes, a few minutes. The levers are the pick
  lists (-s subsets, `pb2.top2k`-style word subsets, a higher `--pm`),
  a `ppb` prospects step, or `-u`.
- A non-enumerative route (subset-sum DP over distinct rests) costs the
  distinct (k−1)-rests times the bag count, the same as walking.
- For k ≥ 3, a row with pairs is a full anagram of the bag in k+3 parts
  (k pair bags, a word bag and two groups). Counting those per rest is
  the anagram-enumeration problem `dfs-anagrams` already attacks with
  pruning and scoring; bag-stats' exhaustive walk is the wrong tool past
  k = 3 on full lists.

## 5. What the S1 2-pb runs are computing

With rests of 34–66 letters and sums of at most 41–43, the `p` step is
dead: one row in 5M has a pair. The three sort keys are all zero on the
rest, and the output is walk order. If the point of those runs is
WORDBAGS, `--ss pb,pb,wb` without a last `p` would say so and skip the
pair machinery; if the point is pairs, the rests have to be shorter (more
picks or more `-u` letters), which is section 4.
