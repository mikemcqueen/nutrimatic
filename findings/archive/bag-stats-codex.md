# bag-stats inner-loop investigation, 2026-10-14

Repository source and build configuration were left unchanged. All experiment
sources, binaries, logs and profiler data are in this directory.

## Profile of the supplied command

Ran the exact supplied command through a 120-second timeout with all 20 hardware
threads, sampling cpu-clock at 49 Hz with DWARF call stacks. Host checks found no
query-index, dfs-anagrams or bag-stats before the run. perf collected 111,351
samples with none lost. The run stopped by timeout; it did not finish.

- 97.74% self samples: PairsExactCounter::count_with, including the inlined
  remainder hash lookup.
- 1.39%: RestCounter::visit.
- 0.55%: PairsExactCounter::filter.
- At the 60-second progress checkpoint: 815/294,780 first pair bags complete,
  5,946 rows, 0 remainder-cache hits, 482,992/6,471,619 pair-cache hits (7.5%).
- Actual input after subtracting tennessee: 46 letters, 17,915,904,000 possible
  sub-bags, 294,780 pair bags, 50 prospect bags, 19,639 outer dictionary word bags,
  195,988 inner dictionary groups. The length-bounded pair table covers through
  13 letters, 198,912,801 entries. The prepared full table is unavailable.

## Best general candidate: packed counts plus staged fit checks

The 22 active letters' counts need 61 bits including one borrow guard per field.
Represent each group's counts in a uint64_t. Subtract from a guarded pool and
check its guard mask to test fit; clear the guards to obtain the remainder.
Build a local hash table with 64-bit exact keys and compact group metadata.
Compute the length cutoff before scanning, using the existing length ordering.

Generate fitting remainder keys and group positions in a reusable scratch buffer
using conditional count increments; then do hash lookups in a second loop. This
removes the fit/miss branch from the scanning loop. Keys are exact, not merely
fingerprints. The prototype retains a full key comparison after tag matching and
preserves same-group and equal-length distinct-pair counting.

Microbenchmarks compare the production exact counter against scratch candidates
on real rows from the input. Every query result is compared individually. The
new table is rebuilt for each row inside the measured interval. Baseline also
rebuilds its existing remainder hash inside the interval. These are single-thread
inner-count benchmarks, not timings of complete bag-stats runs.

Medians of three repeats:

| Sample | Rows | Queries | Current | Packed only | Packed and staged | Speedup |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| ppb,pb,wb,p, harry houdini outer pick | 32 | 64,989 | 2.394273s | 1.802983s | 1.150106s | 2.08x |
| pb,pb,wb,p, no --pp, sampled distinct pair picks | 32 | 38,160 | 0.888441s | 0.660574s | 0.412126s | 2.16x |

All 103,149 sampled query counts matched in all three repeats. Selection is
deterministic and limited to 32 rows per configuration; it does not establish
whole-workload speedup, correctness on arbitrary inputs, or multithread scaling.
The --pp sample uses one prospect, while the no---pp sample varies both picks.
Host process checks were repeated before every timed benchmark.

Implementation direction: add this as a runtime-selected exact-counter fast path,
used by both search sequences. Retain the existing path when the packed layout
needs more than 64 bits; calculate layout width before performing shifts. Reuse
buffers across rows. Integrate it at RestCounter's existing per-row hash build
and PairCountTable's cache-miss count call. Preserve the cache and ranking/output
contracts. Full-run timings and focused output differential checks remain to be
done after integration.

## Other experiments

Deferring sizes_[a] loads and hoisting the length cutoff alone did not produce a
consistent substantial improvement. Do not treat hot assembly instructions as
proof that moving those loads will help.

A per-prospect prepared dense table gives much larger query speedups but depends
on a small outer list. For harry houdini: 972 MiB, preparation 2.7-5.8 seconds,
61,177 queries in 0.005-0.009 seconds versus 1.8-3.5 seconds counting directly.
All counts matched. Across all 50 prospects the dense tables would total about
40.5 GiB; 38 fit individually within 1 GiB. This could be an optional prospect
optimization, but does not meet the general pb,pb requirement as the primary
approach. Query speed excludes preparation and is not an end-to-end speedup.

## Artifacts

- baseline.perf, baseline.log: exact-workload profile and progress.
- annotated.txt, profile-children.txt: disassembly samples and call attribution.
- scope.cpp, scope.csv: exact input loading and per-prospect sizing.
- bench-final.cpp, pairs-public.h, bench-final: final scratch benchmark.
  pairs-public.h exposes existing fields for experimentation without changing
  production layout or source.
- final-pp.out, final-pbpb.out: repeated timings and equality-check results.
- final-pp.log, final-pbpb.log: /usr/bin/time summaries.

Build the scratch benchmark with the release toolchain and existing static
libraries, using -O3 -flto -std=gnu++20 -march=x86-64-v2. Run it with the same
arguments as bag-stats; use --ss pb,pb,wb,p and omit --pp for the second case.
The harness intentionally samples rows and runs its own counting comparisons;
it does not implement ranking or produce bag-stats output.
