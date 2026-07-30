# Plan: bounded memo-aware phase-2 candidate traversal

## Outcome

Prototype candidate optimization 2 from
`findings/fast-p2-search-codex.md`: let exact completability states use the
shared memo knowledge found later in their candidate bucket before committing
to an expensive unknown child.

The implementation should improve total exact-validation wall time for:

```bash
query-index "$IDX" "${S1:0:50}" \
  -n0 -m4 --require-completable -S20
```

without changing any class's exact true/false answer. Candidate order is not
observable in this boolean search, but the implementation must still enumerate
every fitting unknown child unless another child proves the state true.

The starting point includes candidate optimization 1 in the current worktree:
fitting children are probed in the exact memo and reachability bound before
the worker bag and keys are mutated. Preserve that work and build the
memo-aware traversal on top of it.

## Evidence and design choice

The temporary shadow experiment on the 50-letter workload found:

- 7,397,167 recursive expansions attempted;
- 2,126,781 expansions (28.8%) where an unknown fitting child preceded a
  later memoized-true child; and
- 21,615,009,205 unknown fitting candidates encountered by an unbounded
  shadow traversal.

The first number justifies a prototype. The last number rules out retaining
all unknown IDs in a `std::vector` and makes a complete two-pass traversal a
poor first design: it would either allocate enormous logical buffers or scan
large buckets again after already paying to classify them.

Use a **bounded lookahead window** instead:

1. Continue forward through the current rarest-symbol bucket.
2. Apply the existing length and exact-fit tests.
3. Return true immediately if a candidate consumes the whole remainder or
   its child is already known true.
4. Drop children already known false.
5. Put fitting, undecided class IDs in a small fixed-capacity stack array.
6. When the array is full, revisit its IDs in original order:
   - probe the child again, because another worker or an earlier recursive
     child may have published it meanwhile;
   - use a newly known true/false result without mutating the worker;
   - otherwise subtract, recurse, restore, and return on true.
7. Clear the window and continue scanning from the saved bucket cursor.
8. Drain a partial final window the same way.

This scans each bucket candidate for fit at most once. It performs a second
memo/bound probe only for the bounded set of children that were undecided
when discovered. Unknown recursive children retain their original relative
order; only already available knowledge is promoted ahead of them.

## Invariants

- `exact_remainder_completable()` remains an exact boolean recurrence.
- A finite projected reachability result is never treated as conclusive
  unless `cached_reachability()` already reports `REACHABILITY_YES`.
- Every buffered ID was tested against the unchanged parent bag. Recursing
  and restoring must return the worker's bag, mask, exact key, and score key
  to that same parent before processing the next ID.
- A class whose length equals `letters_left` proves true only after the
  existing exact-fit check.
- A false result is stored only after the bucket cursor reaches the end and
  every buffered unknown child has been decided false.
- The fixed buffer contains `uint32_t` class IDs. This is safe under
  `prepare_hot_classes()`'s existing `classes.size() <= UINT32_MAX`
  requirement.
- Do not allocate, resize a container, or parse an environment variable in
  the recursive hot path.
- Do not change ordinary ranked `dfs-anagrams` traversal. The optimization is
  limited to exact batch completability.
- Additional lookups deliberately change the memo-hit count. Candidate
  reordering can also reduce the set of expanded exact states. Neither
  counter is a correctness invariant.

## Phase 1: isolate child classification

Refactor the candidate-optimization-1 logic in
`source/dfs-search.cpp` into a small private/inlinable operation used by both
the immediate-recursion control and the lookahead prototype.

Given the unchanged parent worker, class ID, and candidate length, it should
classify the child as:

```text
true / false / unknown
```

in this order:

1. exact whole-remainder fit;
2. exact child memo lookup using
   `worker.exact_key - class.signature`;
3. child reachability lookup using
   `worker.score_key - score_key_delta[class_id]`;
4. store a conclusive bound answer in the exact memo;
5. otherwise report unknown.

Keep computation of the child keys outside bag mutation. Avoid widening the
public `DfsAnagramSearch` interface; an enum and helper may remain private.
The control path with lookahead disabled must still recurse at the first
unknown fitting candidate, matching optimization 1.

Build and run the narrow smoke tests after this refactor before changing
traversal. Reject the refactor if it creates measurable control-path
regression on alternating 50-letter runs.

## Phase 2: add the bounded traversal

Add a private exact-search lookahead setting, read once before phase-2 exact
validation. Use a temporary same-binary experiment switch such as:

```text
NUTRIMATIC_EXACT_MEMO_LOOKAHEAD
```

with:

- `0` selecting the current immediate-recursion control;
- positive decimal values selecting the number of unknown fitting children
  retained per window; and
- a small compile-time maximum so the recursive stack footprint is bounded.

Start with a maximum of 64 IDs (256 bytes per active recursive frame) and
measure widths `4`, `16`, and `64`. Invalid and oversized experimental values
should fall back to the normal default rather than enter undefined behavior.
Parse the setting once; do not call `getenv()` or `strtoull()` per state.

Keep the old loop in a separate control helper or branch before declaring the
fixed buffer so a `0` run is a credible same-binary baseline and does not pay
the lookahead frame size. The lookahead loop should:

- retain only IDs, not copied class records or child bags;
- keep one forward cursor, with no second scan of already classified bucket
  candidates;
- avoid repeating the fit test when draining a window;
- re-run the cheap child classifier before mutation;
- call the existing `subtract_exact_class()` /
  `restore_exact_class()` pair only for still-unknown children; and
- stop filling as soon as a known true child is encountered.

Do not add the old shadow counters to the normal loop. If diagnostics are
needed to understand a result, use only coarse per-worker totals such as:

- windows filled;
- states won by a known-true child after at least one unknown was buffered;
- buffered children decided by a re-probe; and
- buffered children recursively expanded.

Merge them after workers join, as existing counters are merged. Run timing
comparisons without any per-candidate shadow model.

## Phase 3: choose or reject a window

Run alternating, uncontended same-binary trials for lookahead widths:

```text
0, 4, 16, 64
```

Test `-S1` first to measure intrinsic work, then `-S20` for the production
case. A wider window can expose more later true children, but it also delays
the first unknown recursion and performs more probes before learning anything.
Choose by measured total exact-validation wall time, not by the number of
lookahead wins alone.

Record for every trial:

- phase-1, phase-2 setup, exact-validation, and whole-command wall time;
- user and system CPU;
- peak RSS;
- exact states inserted and memo hits;
- any coarse lookahead counters;
- requested and actual search workers; and
- complete output hash.

Decision:

- If one width wins repeatably on the primary 50-letter workload without a
  material regression on the 32- and 40-letter checks, make it the default.
- If results are flat or negative, remove the traversal prototype and retain
  only the child-classification refactor if that refactor is neutral.
- Do not proceed from a negative bounded-window result to an unbounded vector
  or full two-pass scan; the recorded 21.6-billion-unknown observation already
  makes those poor defaults.

After selecting a winner, either retain `0` as an internal same-binary
diagnostic opt-out following the repository's existing experiment-switch
pattern, or remove the switch and its alternate path before committing. Do
not add a public CLI option for this internal traversal detail.

## Correctness and smoke validation

Keep tests minimal:

1. Build with the repository environment:

   ```bash
   source ~/code/nutrimatic/.env/bin/activate
   conan build .
   source build/dep-info/conanbuild.sh
   ```

2. Run the narrow existing tests:

   ```bash
   meson test -C build dfs-search query-index-cli --print-errorlogs
   ```

3. Exercise the synthetic `wxyz` case already in
   `source/test-query-index.sh` with the control and selected lookahead. It
   must continue to retain `wx`/`yz` and reject dead-end `xy`.
4. With `IDX` exported, compare complete output hashes for the manageable
   `${S1:0:32}` workload under:
   - lookahead control versus prototype;
   - `-S1` versus `-S20`; and
   - bound-off versus a shallow projected bound.
5. Run the selected prototype on `${S1:0:40}` and `${S1:0:50}`. Exact-state
   and memo-hit totals may differ, but output must not.

No timing assertion belongs in the unit tests.

## Benchmark protocol

Follow the repository instructions:

```bash
source ./setup.sh
export IDX=~/code/nutrimatic/idx/wiki-merged.5.index
```

Before every timed run, check for competing instances:

```bash
pgrep -af '(^|/)query-index( |$)'
pgrep -af '(^|/)dfs-anagrams( |$)'
```

Use `/usr/bin/time -v` around:

```bash
./build/query-index "$IDX" "${S1:0:50}" \
  -n0 -m4 --require-completable -S20
```

Redirect stdout to a file when establishing correctness and to `/dev/null`
only after its hash is recorded. Alternate width `0` and prototype runs rather
than collecting all controls first. The earlier 68.9-second 50-letter result
is directional context, not a substitute for a fresh same-binary baseline.

Useful secondary cases:

| workload | purpose |
|---|---|
| `${S1:0:32}` | quick correctness and regression iteration |
| `${S1:0:40}` | medium intrinsic-work and parallel-scaling check |
| `${S1:0:50}` | primary memo-maturity workload |
| completion-heavy bag | exercise known-true short circuiting |
| dead-end-heavy bag | ensure lookahead does not overpay on false states |

## Files and review boundary

Expected implementation files:

- `source/dfs-search.cpp` — child classification, control traversal,
  bounded-window traversal, experiment parsing, and optional diagnostics;
- `source/dfs-search.h` — private enum/helper declarations and any private
  setting or worker counters;
- `source/test-query-index.sh` or `source/test-dfs-search.cpp` — only if a
  focused control/prototype differential is needed; and
- `findings/fast-p2-search-codex.md` — final timings, selected width, counter
  changes, and keep/reject decision.

Do not mix support-mask grouping, dynamic rarest-symbol selection, memo
ownership, exact-memo representation changes, or projected-cache policy into
this experiment. They change different costs and would make the result
unattributable.

Before committing, run `/review` as required by `AGENTS.md`, address its
findings, rerun the smoke tests, and record the final benchmark commands and
result in the findings document.
