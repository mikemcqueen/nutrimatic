# Projected score-bound preprocessing: next optimization targets

## Result

The complete projected bottom-up evaluator is already the right overall
algorithm: it removes recursive atomic coordination, filters an action once
per exact bag, and updates the contiguous wildcard vector in action-outer
order. The next throughput target is neither score-table allocation nor
thread scheduling. It is the scalar wildcard-update loop in
`DfsAnagramSearch::compute_projected_score_bound_bottom_up()`.

The recommended implementation order is:

1. add a runtime-dispatched SIMD implementation of the wildcard-update loop;
2. measure a split exact-support-mask sidecar against that SIMD baseline; and
3. move the per-transition diagnostic counters into worker-local scalars.

Each change must preserve the current upward-rounded score-bound invariant,
the retained output, and the deterministic preprocessing and final-search
counters.

## Evidence

The relevant profile is the 40-letter, `d=15`, top-1,000, one-thread
Callgrind instruction-count run in
`results/dfs-anagrams-40-d15-n1000-c256-t1-debugoptimized.annotate.txt`.
It records `Ir`, not elapsed cycles or hardware cache misses. It therefore
locates instruction work reliably but cannot, by itself, prove that the
kernel is memory-bound.

The worker lambda inside the bottom-up evaluator accounts for 72.96% of all
recorded instructions. Its important source-level entries are:

| Operation | Whole-program `Ir` share |
|---|---:|
| Scan actions and test exact support | 6.46% |
| Wildcard-vector loop control | 5.24% |
| Test a child against `-HUGE_VAL` | 7.28% |
| Update `best[wild]` | 4.70% |
| Update `max_rounding_error[wild]` | 4.70% |

The deterministic counters for this input are 11,154,835,508 action scans,
12,580,372,385 fitting wildcard transitions, and 12,184,378,227 successful
finite-child transitions. Thus 96.85% of fitting child values are finite.
The dead-child branch is necessary at the terminal perimeter, but it is not
the common case.

The recurrence currently performs, for each fitting action and wildcard
count:

```text
load a contiguous child float
convert it to double
skip a dead child
update the score maximum
calculate and update the rounding-error maximum
```

The wildcard vector is contiguous and has 17 entries on this workload. That
is enough work per action for vector lanes, whereas final state publication
and layer construction occur only once per exact bag and are not leading
costs.

GCC 14 vectorization diagnostics report that this loop remains scalar even
under `-march=native`, because of unsupported control flow. Raising the
binary-wide ISA alone is therefore not an optimization.

## Opportunity 1: masked SIMD wildcard updates

Implement a target-cloned, runtime-dispatched SIMD kernel for the inner
wildcard update range. It must retain the scalar implementation as the
portable baseline and for the short prefix/tail.

For several adjacent wildcard counts, the SIMD path should:

1. load contiguous child `float` values;
2. widen them to double lanes;
3. form a mask for finite children (`child != -HUGE_VAL`);
4. evaluate the existing score and rounding-error expressions in each finite
   lane, without fast-math;
5. max-update the corresponding `best` and `max_rounding_error` entries; and
6. count finite lanes locally for the existing diagnostics.

The action's `partial_score` and `rounding_error_base` are loop invariants and
can be broadcast once per action. `-HUGE_VAL` child lanes must not affect
either maximum. The final `round_score_bound_up()` and
`round_float_score_bound_up()` calls remain scalar and unchanged, preserving
the established upward-bound proof.

Use runtime capability dispatch (for example, target-specific functions) so
the normal `x86-64-v2` binary remains runnable on its supported baseline.
AVX2 is the first useful target on the measurement machine; a maintainable
SSE2 implementation may be worthwhile only if it can share the same test
coverage and does not obscure the scalar code.

This is the leading candidate because it attacks the costly operation that
still occurs for almost every fitting transition. It should be measured as a
whole-program setup-plus-search A/B, not selected from a reduced instruction
count alone.

## Opportunity 2: split exact-support-mask sidecar

The current 48-byte `ProjectedAction` contains the exact-support mask beside
the cold data needed only by actions that pass support filtering. The current
scan must load that record to perform:

```cpp
if ((action.exact_support_mask & ~exact_mask) != 0) continue;
```

At `d=15`, support filtering rejects the large majority of scanned actions.
A contiguous `uint64_t` support-mask array lets the loop test support first,
then load the cold action only for survivors. Removing the mask from the cold
record should allow a 40-byte, eight-byte-aligned action record; the 8-byte
sidecar keeps the present 48-byte-per-action total while making rejection
loads much denser.

Existing top-down measurements found 8--15% setup reductions from this
layout. A bottom-up wiring reproduced correctness on a smaller workload, but
there is not yet an uncontended long-input timing. It should therefore be
compared against the completed SIMD baseline before being enabled by default.

Support-group indexing is not the immediate replacement. It reduced action
scans substantially in previous experiments, but its submask enumeration and
indirect action loads left the expensive wildcard updates intact and yielded
only about a low-teens setup improvement. Grouping by wildcard length was
slower despite removing more scans.

## Opportunity 3: local diagnostic counters

The action scan and wildcard loops increment fields through `VectorWorker` on
every event. Those counts are useful public diagnostics but are not needed to
make recurrence decisions. Use local `uint64_t` accumulators for candidate
tests, fitting transitions, successful transitions, and `nextafter` calls
within one worker invocation, then add them to the worker fields once at the
end.

This can reduce hot-loop stores and register pressure without weakening the
reported totals. It is deliberately a small, independently measurable
cleanup; it is not expected to beat SIMD. Keep the overflow behavior identical
to the existing unsigned counters and retain the exact final aggregation.

## Non-targets

- Do not reintroduce atomic table words: bottom-up has one owner per exact
  bag/vector and reads only completed earlier layers.
- Do not prioritize a viability mask. Only 3.15% of fitting transitions in
  this profile are dead children.
- Do not optimize final per-state rounding first. It occurs once per state,
  not once per fitting transition.
- Do not add persistent workers solely for this kernel. Previous A/Bs found
  no repeatable gain over the simple per-layer scheduler.
- Do not add wildcard-length support groups; measured setup regressed.
- Do not infer cache stalls from the Callgrind `Ir` profile. Use hardware
  counters if the platform exposes them, or treat cache simulation as an
  approximation.

## Measurement standard

For every enabled candidate, compare against the immediately preceding
same-binary baseline on an uncontended machine. Check for another
`dfs-anagrams` process before collecting timing data. Record:

- phase-2 setup and total wall time;
- candidate, fitting, successful-transition, and rounding counters;
- final DFS node, solution, spelling-expansion, and retained-result counts;
- byte-for-byte stdout hash; and
- when available, cycles, instructions, branch misses, and cache misses.

The required metric is setup plus final search. A preprocessing speedup that
weakens bounds enough to regress total wall time is not a production win.
