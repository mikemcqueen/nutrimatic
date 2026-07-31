# Projected score-bound preprocessing: measured outcomes

Companion to `findings/projected-score-optimization.md`, recording what the
three recommendations there actually bought, and the final default for each
switch. Commits `1eb380b` through `6869539`.

## Result

All three recommendations landed. On the reference workload, phase-2 setup
fell 28% and setup plus final search fell 22%, with byte-identical stdout and
identical deterministic counters throughout.

| Change | Setup, -T 1 | Setup, -T 20 | Default |
|---|---:|---:|---|
| Baseline (`86b7a9a`) | 35.3s | 3.4s | — |
| Worker-local counters | −1.5% | — | always on |
| Kernel extraction alone | +2% | — | always on |
| AVX2 wildcard kernel | −21% | −13% | mandatory |
| Support-mask sidecar | −5% | −7% | on, mandatory since `6869539` |
| All of it (`6869539`) | **25.4s** | **2.5s** | |

Setup plus search, `-T 1`: 44.5s → 34.8s. At `-T 20`: 12.7s → 11.8s, where
final search dominates and is untouched.

The recommendations' relative-importance claim held: SIMD dominated, the
sidecar was a real but smaller second, and the counter cleanup was nearly
free. The order was inverted deliberately (counters first), because the SIMD
kernel's contract already presumed the local-accumulator refactor.

## How it was measured

```
export IDX=~/code/nutrimatic/idx/wiki-merged.5.index
source ./s.sh
taskset -c 0 ./build/dfs-anagrams $IDX ${S6:0:40} \
    -d 15 -n 1000 -C 256 -F -p 10000 -T 1        # reference workload
taskset -c 0 ./build/dfs-anagrams $IDX ${S6:0:38} \
    -d 15 -n 200 -C 256 -F -p 100000 -T 1        # fast iteration workload
```

- Machine: i7-13700H, 20 logical CPUs, WSL2, AVX2 present. Release build,
  GCC 14.2.0, `-O3 -march=x86-64-v2`, static libraries, `b_lto=true`.
- `pgrep -a dfs-anagrams` was empty before every timing run.
- Every A/B ran the two binaries interleaved, at least two rounds, and
  compared the minimum; the tables above quote setup seconds.
- Reference workload: 7,050,240 projected states, 151,440 actions, wildcard
  span 17. Fast workload: 6,220,800 states, span 15.
- `results/projected-opt-baseline.{stdout,stderr}` and
  `results/projected-opt-final.{stdout,stderr}` are the endpoint runs. Their
  stdout hashes are equal:
  `5cf8a34f71e78a47270897fb32161526807fb2ff906580708986f8e42f0ec601`.
- Invariant counters, unchanged by every commit here: 11,154,835,508
  candidate tests, 12,580,372,385 fitting transitions, 12,184,378,227
  successful transitions, 17,491,291 `nextafter` calls, 203,098,015 nodes,
  24,767 solutions, 15,900 spellings expanded, 1,000 retained.

### Pin single-threaded runs

This is a hybrid CPU behind WSL2, which reports a uniform 10-core, 20-thread
topology it does not have. An unpinned `-T 1` run drifts between core types
and swings up to 25% between runs, which is larger than most of the effects
measured here; two consecutive unpinned runs of the same binary produced
12.3s and 9.2s of search time. Pinned to logical CPU 0, three consecutive
runs of one binary agreed within 0.6%. Every single-threaded number in this
document is pinned. Multi-threaded runs are not.

No hardware counters are available on this machine, so wall time plus the
deterministic counters is the whole basis for these decisions.

## Recommendation 1: masked SIMD wildcard updates

Landed as `projected_wild_update_avx2()`, four wildcard counts per iteration
behind `__attribute__((target("avx2")))` with `__builtin_cpu_supports()`
dispatch resolved once per preprocessing run. It is the largest win here:
setup 35.0s → 27.5s at `-T 1`, and 3.1s → 2.7s at `-T 20`.

The smaller multi-threaded win is the memory-bound risk the plan named,
partly realized: twenty workers over a 28 MB score table have less headroom
than one. It is still a win at both thread counts, so the kernel is on by
default.

Bit-identity is structural, not incidental:

- `float` → `double` widening is exact.
- The dead-child test is `_CMP_NEQ_UQ`, so a NaN child counts as live exactly
  as the scalar `child == -HUGE_VAL` leaves it live.
- Both maxima blend dead lanes back to their current values before `maxpd`,
  which keeps `inf` out of the error envelope.
- `_mm256_max_pd(candidate, current)` reproduces
  `std::max(current, candidate)` for ties, NaN, and +0.0 against −0.0.
- No expression here can contract into an FMA, and intrinsics would not
  contract anyway.

The removed verification mode re-ran the scalar kernel on copies of both spans
and compared them plus the finite count. It was clean over the fast workload's
8.95e9 wildcard transitions and was confirmed to fire against a deliberately
perturbed broadcast.

### The kernel's shape matters more than its contents

Three separate attempts at the same kernel differed by up to 19% of setup:

| Shape | Setup, fast workload |
|---|---:|
| Inline loop, no kernel (`86b7a9a`) | 25.6s |
| Kernel taking a `ProjectedWildUpdate const&` | 27.1s |
| Same, forced `always_inline` | 26.5s |
| Kernel taking flat arguments, `always_inline` | 26.2s |
| Flat arguments, runtime branch on the kernel choice | 29.9s |
| Flat arguments, scan specialized on the choice | 26.1s |

Passing a descriptor struct by const reference made GCC materialize it in
memory once per surviving action. Branching on the kernel choice inside the
scan made the scalar path spill loop-invariant registers across a call it
never makes. The landed code therefore passes flat arguments and specializes
the whole action scan on the kernel choice through an `integral_constant`
tag, so the scalar path keeps a plain inlined loop and the vector path costs
one direct call per action. That is a deviation from the plan's function
pointer, forced by measurement.

No SSE2 kernel was added; there is no target machine without AVX2 to justify
one.

## Recommendation 2: split exact-support-mask sidecar

Landed in two commits. `0e694fe` adds `projected_action_support`,
index-parallel to `projected_actions`, and tests it before binding the cold
record: setup 26.5s → 25.3s at `-T 1` and 2.7s → 2.5s at `-T 20`, measured
against the completed SIMD baseline as the findings required. This is the
first uncontended long-input timing for this change; the earlier top-down
8–15% figure was not carried over, and 5% is what it is worth here.

`6869539` then removes `exact_support_mask` from `ProjectedAction`, leaving
the sidecar as the only copy. As the plan predicted, this is maintenance
rather than throughput and measures neutral: `alignas(16)` stays, the
remaining 40 bytes of members pad back to 48, so the per-action footprint is
48 + 8 rather than the flat 48 the original findings assumed — about 1.2 MB
of sidecar at 151,440 actions, against a 28 MB score table.

Because the mask no longer rides along in the record through the per-bucket
sort, the sort now works on a temporary paired array which is split into the
two final arrays. That is what makes the sidecar index-parallel by
construction rather than by convention, which was the drift risk.

Revisiting `alignas(16)` would recover the findings' original 48-byte-flat
outcome. That is a separate, separately measured decision.

## Recommendation 3: local diagnostic counters

Landed in `1eb380b`: four `uint64_t` accumulators for the whole worker
invocation, folded into the `VectorWorker` fields once before returning. The
per-`wild` fitting-transition increment became a closed form, since the
wildcard loop runs unconditionally over `[wild_length, score_wild_span)`.

Worth about 1.5% of setup on the reference workload and inside the noise on
the fast one, which matches the findings' expectation that it would not beat
SIMD. It was still worth doing first, because the SIMD kernel's contract
already presumed local accumulation.

Note for future counter assertions: candidate tests do **not** bound fitting
transitions in the layered bottom-up path, since one surviving action
contributes a whole wildcard span. The reference workload records 11.2e9
candidate tests against 12.6e9 fitting transitions.

## Coverage

- A table-driven test calls the scalar and dispatched kernels on identical
  inputs and compares both spans and the finite count bitwise: all-finite,
  all-dead and mixed children; spans shorter than a lane group, exactly one
  group, and a group plus a tail; every destination offset; seeds that move
  the maximum in some lanes and not others; and no writes outside the span.
  It reaches the kernels through one small static test hook.
- The forced-depth end-to-end block exercises each projected depth through the
  mandatory AVX2 kernel and checks its counters and retained spellings.
- Both were confirmed to fail against deliberately broken kernels: a
  perturbed broadcast, and a dropped dead-lane blend on the error envelope.

## Switches, after this work

| Variable | Default | Effect |
|---|---|---|
| `NUTRIMATIC_PROJECTED_SUPPORT_SIDECAR` | removed | the sidecar is the only support-mask copy |

## Not done

Everything in the findings' non-targets list, plus support-group indexing,
packed 16-bit score storage, layer-list streaming, an SSE2 kernel, raising
the binary-wide ISA above `x86-64-v2`, and revisiting `alignas(16)` on
`ProjectedAction`.
