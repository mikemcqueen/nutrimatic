Result

Reference workload (40 letters, -d 15 -n 1000 -C 256 -F), stdout sha256 and all deterministic counters identical to baseline throughout:

┌─────────────────────────┬──────────────────────┬─────────────────────┐
│                         │    -T 1 (pinned)     │        -T 20        │
├─────────────────────────┼──────────────────────┼─────────────────────┤
│ setup, baseline 86b7a9a │                35.3s │                3.4s │
├─────────────────────────┼──────────────────────┼─────────────────────┤
│ setup, final 6869539    │         25.4s (−28%) │         2.5s (−28%) │
├─────────────────────────┼──────────────────────┼─────────────────────┤
│ setup + search          │ 44.5s → 34.8s (−22%) │ 12.7s → 11.8s (−7%) │
└─────────────────────────┴──────────────────────┴─────────────────────┘

Per recommendation: AVX2 kernel −21%, support sidecar −5%, local counters −1.5% (single-thread setup).

Deviations from the plan, all forced by measurement

- Kernel contract is flat arguments, not a ProjectedWildUpdateor by const reference made GCC materialize it in memory once per surviving action — 5% of setup. Letting GCC choose the inlining cost another 2%.
- Dispatch is an integral_constant tag on the action scan, note branch on the kernel choice inside the scan made the scalarpath spill loop-invariant registers across a call it never makes — 14% of setup.
- No new d > 0 test was needed. The existing forced-depth blocpp already runs the layered worker; it now runs each depth under SIMD 0/1/verify instead.
- 4b does not shrink the record (as the plan itself predicted)ed the per-bucket sort onto a temporary paired array, since themask can no longer ride along in the record.

Two things worth knowing

The machine misreports its topology. It's a hybrid i7-13700H behind WSL2, presenting a uniform 10×2 layout it doesn't have. Unpinned -T 1 runs drift between core types and swing up to 25% — larger than most effeinned runs of the same binary gave 12.3s and 9.2s search time;pinned to CPU 0, three runs agreed within 0.6%. My phase-1 baseline was collected before I noticed, so the final comparison re-measured both endpoinpinned. Any future A/B on this box needs taskset.
                                                                                                                                                    I could not run /code-review. It's user-triggered and billed, instead and validated the risky parts empirically: verify modeis clean over 8.95e9 wildcard transitions and was confirmed to fire against a perturbed broadcast; the kernel test was confirmed to fail against a ddead-lane blend; the AVX2 body was disassembled to confirm reacodegen rather than a silent baseline compile. The seven commits are still unreviewed by the tool your CLAUDE.md asks for — abb289d (the AVX2 kernel) is the one I'd point a reviewer at first.                      
Two pre-existing test failures are unrelated to this work: NUTRIMATIC_PROJECTED_BOTTOM_UP=0 and NUTRIMATIC_PROJECTED_ACTION_QUOTIENT=0 in the outer environment conflict with what test-dfs-search sets and unsetscally at 86b7a9a.

Results are written up in findings/projected-score-optimizatiore in results/projected-opt-{baseline,final}.{stdout,stderr}(untracked — results/ is gitignored).
