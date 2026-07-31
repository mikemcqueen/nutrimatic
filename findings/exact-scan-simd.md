# Speeding up phase-2 exact validation (6.2x)

Benchmark: `query-index $IDX ${S1:0:50} -m 4 -n 0 -S 20 --require-completable`
on wiki-merged.5.index, 20 search threads. Timing is the "exact validation"
figure from the phase 2 timing diagnostic. Output md5 `9bc5bf82...` is
unchanged by every step below, as are the memo state/hit counts.

| stage | exact validation |
| --- | --- |
| baseline | 45.0s |
| + scan reads only FitClass | 15.6s |
| + skip score keys when bounds are off | 14.3s |
| + skip the re-probe on recursion | 14.3s (noise) |
| + contiguous support masks, AVX2 scan | **7.2s** |

## Where the time actually goes

Instrumenting the candidate loop settled it. Per exact-validation run:

- 6.26M search nodes
- **224.5 billion** candidate iterations — 35,880 scanned per node
- 45.5M of those fit: a **0.02% fit rate**

So ~99.98% of phase 2 is a linear scan rejecting candidates on the support
mask. Everything else — memo probing, bag arithmetic, recursion — is rounding
error by comparison. 25M memo probes at DRAM latency is ~2.5 core-seconds
against a 260 core-second budget.

An earlier version of the counter added `end - begin` per node and reported
5.2 trillion, which is impossible in the wall time observed. The loop exits
early on the first success, so only an in-loop counter measures real work.

## Why each change helped

**Scan reads only FitClass.** The loop read `classes[class_index].key_length`
to length-filter, then `fit_classes[class_index]` to fit-test. That streams a
24-byte `DfsClassRecord` (123MB array) to consume one byte, alongside the
16-byte `FitClass` (82MB). Two facts make the first read removable:
`FitClass::packed_length_and_count` already carries `key_length`, and records
are sorted by length descending within a bucket, so after
`first_length_candidate` every remaining candidate already satisfies
`length <= letters_left` — the length test was dead. Dropping it removes ~60%
of the loop's memory traffic, which is most of the 45s -> 15.6s.

**Skip score keys when bounds are off.** This query prints "score-bound mode
off", and `cached_reachability` answers UNKNOWN for every key in that mode.
Yet `subtract_exact_class` / `restore_exact_class` each did a random 8-byte
read from the 41MB `score_key_deltas` array, and `classify_exact_child` did
another on every memo miss, all to feed a function that cannot answer. Gating
them on `score_bounds_active()` removes three random DRAM reads per expansion.

**Skip the re-probe on recursion.** `classify_exact_child` probes the memo for
the child key and misses; the caller then subtracts and calls
`exact_remainder_completable`, which immediately probes that same key again.
`exact_expand_node` is the tail of that function, entered directly when the
caller has already probed. Only ~1.1M of ~25M probes, so it does not show
above the noise — kept because it is provably redundant work, not for measured
gain.

**Contiguous support masks + AVX2.** Given the 0.02% fit rate, the reject path
should touch nothing but the support mask. `class_supports` holds the masks in
their own array: 8 bytes of stride instead of `FitClass`'s 16, and loadable
four at a time. `next_support_fit_avx2` tests 16 candidates per iteration
(4x `vpcmpeqq` + `movemask`) and returns the first fitting index, so scan
order — which the lookahead window depends on — is unchanged. This halves the
remaining traffic and cuts the per-candidate instruction count ~4x: 14.3s ->
7.2s.

The masks are duplicated rather than moved out of `FitClass`, costing 41MB.
`FitClass::support_mask` still serves the `run()` search path and the bound
evaluator, and its `static_assert(sizeof(FitClass) == 16)` keeps four per
cache line. Splitting it properly would save the duplicate but touches far
more than the exact path.

## Correctness

- All 7 meson tests pass.
- Benchmark output md5 identical at every stage, and identical with
  `NUTRIMATIC_SUPPORT_SIMD=0` (scalar fallback).
- Differential vs. a HEAD-built binary over 7 configurations covering
  bounds-off, `projected` (including `-d 1` and `-d 2` at 50 letters, and
  `-C 512`), and `dense` (`-D`) bound modes — all match. This is the case that
  matters for the score-key change, since those runs still need the key.

## Measuring this benchmark

`-S 20` and the exact search only do anything with `--require-completable`;
without it the run is phase 1 only (~20s) and `-S` is inert.

Contention invalidates results badly and quietly. A concurrent 20-thread
`query-index` in a sibling checkout inflated the baseline from 45s to 81s, and
three consecutive "consistent" 46.6s samples were all contended. `tmp/bench-exact.sh`
waits for `pgrep -x query-index` / `dfs-anagrams` to come back empty before
starting and samples every 2s during the run, flagging any sample that
overlapped. Use `pgrep -x`, not `-f`: sandbox and shell wrappers carry
"query-index" in their argv and match `-f` spuriously.

## Not pursued

The fit rate suggests the linear scan is the wrong structure outright.
Candidates within a bucket could be grouped by distinct support mask, testing
each distinct mask once and skipping whole groups — potentially another large
factor. It conflicts with the length-descending order that
`first_length_candidate` relies on, so it is a redesign rather than an
optimization, and is left alone here.
