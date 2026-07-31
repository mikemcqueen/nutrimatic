# Plan: fast exact completability filtering for `query-index`

## Goal

Make `query-index --require-completable` report only vocabulary entries whose
anagram class can participate in at least one complete anagram of the supplied
letter bag, subject to the same `-m` and dictionary rules as `dfs-anagrams`.

The mode should normally finish faster than `dfs-anagrams` on the same input.
It does not need a strict worst-case wall-time guarantee, and it does not need
to find or score the best completion.

## Settled behavior

- Build phase 1 once for the original full bag. Never rebuild `DfsClassList`
  for individual remainders.
- Prepare one score-bound cache and reuse it for every candidate class.
- Use the projected score cache by default, with the same default cache size
  and automatic preprocessing policy as `dfs-anagrams`.
- Treat projected bounds as a one-sided accelerator:
  - `-HUGE_VAL` proves that a remainder cannot be completed.
  - A finite projected bound is not proof of an exact completion when wildcard
    letters have been merged, so validate it with exact first-solution search.
- Stop an exact validation as soon as its first completion is found.
- Share exact reachability results between candidate validations so the same
  exact remainder state is not searched repeatedly.
- Evaluate every vocabulary class before ranking. Do not stop after finding
  enough completable entries for `-n`: a lower-frequency multi-word entry can
  move above the eventual output floor after `--word-bonus` is applied.
- Preserve `query-index`'s existing ranking:
  raw corpus frequency, adjusted by `--word-bonus`, followed by the existing
  text tie-break. Do not rank by the score of a completion.
- Build reachability bounds with neutral class scores; `--word-bonus` affects
  output ranking only and must not affect completability.
- Apply `-n` only after exact completability filtering and ranking.
- Use the same default minimum word length as `dfs-anagrams` (`4`), including
  its short-input adjustment and validation.
- The current uncommitted per-class-search implementation is disposable.

## Why the current implementation is slow

The current worktree does this once per `DfsAnagramClass`:

1. subtract the class key from the full bag;
2. extract another `DfsClassList` from the index for that remainder;
3. construct another `DfsAnagramSearch`;
4. run that search with the score cache disabled.

This repeats both trie extraction and phase-2 setup for every class. Dead-end
remainders are especially expensive because they cannot trigger the
first-solution short circuit.

On a representative 24-letter S6 prefix with `-m 4 -n 100`, the current
implementation produced no output before a 30-second timeout, while
`dfs-anagrams` completed in about 4 seconds.

## Important cache semantics

The existing score cache is a score upper-bound cache, but its reachability
information is still useful:

- An exact dense bound is finite if and only if that exact remainder state has
  a completion.
- A projected bound may be finite because wildcard letter identities have
  been relaxed. Such a value is only "possibly completable."
- A projected `-HUGE_VAL` is conclusive: if even the relaxed state has no
  completion, the exact state has none.

The existing cache also omits the full root plane for the globally rarest
letter. A candidate that does not consume that letter can leave a remainder
whose key is outside the table. Those states must enter exact validation
without assuming that an O(1) bound lookup exists. Once validation consumes
the forced rarest letter, descendant states can use the shared table.

These details mean that merely exposing `load_score_bound()` as a public
arbitrary-state lookup is insufficient.

## Design

### 1. Add a batch completability operation to `DfsAnagramSearch`

Add a public operation conceptually like:

```cpp
bool find_completable_classes(
    std::vector<bool>* completable,
    int progress_factor = 1,
    bool allow_cache_fallback = false,
    bool dense_cache = false,
    int exact_letters = -1);
```

The result vector is index-parallel to `DfsClassList::classes()`.

This operation should:

1. initialize the full-bag hot-class representation;
2. prepare the requested score-bound cache once;
3. test every class remainder against that shared state;
4. return exact booleans, not projected "maybe" answers.

Refactor the phase-2 preparation currently embedded in `run()` into a private
common setup path used by both ordinary ranked search and batch
completability. Keep score-bound selection, projected-action construction,
parallel preprocessing, cache fallback, and diagnostics in that common path
so the two tools cannot drift.

Do not run the ordinary full-bag DFS merely to trigger cache preparation.
`query-index` needs the preparation followed by its batch queries, not an
otherwise-unused root solution search.

### 2. Exact reachability search with shared memoization

Implement an existence-only recursion inside `DfsAnagramSearch` using the
same prepared class data as production phase 2:

- select the rarest remaining symbol;
- scan that symbol's class bucket;
- use the existing hot support and multiplicity tests;
- subtract and restore the existing packed class requirements;
- return immediately on the first successful child.

Memoize the exact boolean result by the full exact remainder bag, not by the
projected score key. Different exact bags can intentionally share one
projected key.

Prefer a compact exact mixed-radix key when the full bag is encodable in
`uint64_t`. If that key cannot be represented, fall back to a hashable exact
bag representation rather than disabling correctness. Store both positive
and negative results.

The existence recursion does not need score accumulation, a score floor,
spellings, or permutation enumeration. State-only memoization also removes
the need for an entry-point-dependent dedup key: each exact remaining bag is
solved once by its forced-rarest-letter transitions.

Before expanding an exact state:

1. return its memoized result when present;
2. query the shared bound when the state is covered;
3. return false immediately for `-HUGE_VAL`;
4. when the bound is exact rather than projected, return true for a finite
   value;
5. otherwise run the exact existence recursion.

An empty remainder is immediately completable.

### 3. Make floor-independent reachability pruning reusable

Separate "the bound proves this state unreachable" from score-floor pruning.
Today `should_prune()` only uses `-HUGE_VAL` specially at the original root;
other cached states are normally consulted only after a sink exposes a score
floor.

Introduce a private helper that safely queries reachability for the current
state, accounting for:

- the omitted original root/root-plane states;
- exact dense, dense-prefix, and projected modes;
- unseen or out-of-capacity states;
- projected finite values being inconclusive.

Use the helper from both ordinary `dfs-anagrams` traversal and the new exact
completability recursion. This keeps the optimization in the search engine
rather than duplicating cache interpretation in `query-index`.

The `DfsSolutionSink::should_stop()` hook may remain useful for ordinary
first-solution searches and should keep its checks in both optimized walk
loops, but batch completability should not construct a new search or sink per
class.

### 4. Build one complete phase-2 class list

For `--require-completable`, construct one `DfsClassList` for the full bag
with phrases enabled, exactly as `dfs-anagrams` does. The completion search
must not lose phrase classes merely because `query-index --words-only` limits
what may be printed.

When `--words-only` is active, filter candidate members by
`member.word_count == 1` while retaining all classes for completion search.
Classes containing both a single-word spelling and phrase spellings should
be tested once and only their allowed members flattened for ranking.

Without `--require-completable`, preserve the existing phase-1-only behavior
and avoid phase-2 cache setup.

Dictionary filtering applies to both displayed candidates and completion
classes, as it does now.

### 5. Match `dfs-anagrams` cache controls

Add these `dfs-anagrams`-compatible options to `query-index`:

- `-C`, `--cache-size MiB`
- `-T`, `--preprocess-threads N`
- `-S`, `--search-threads N`
- `-d`, `--projection-depth N`
- `-D`, `--dense-cache`
- `-F`, `--allow-cache-fallback`

Use the same defaults:

- 64 MiB score cache;
- projected dense cache;
- automatic projection depth (largest that fits);
- automatic preprocessing for bags of 26 or more letters, capped the same
  way as `dfs-anagrams`;
- one exact-validation thread unless `-S` requests more;
- cache fallback disabled unless `-F` is supplied;
- minimum word length `4`, adjusted down only when the remaining input bag is
  shorter and the user did not explicitly supply `-m`.

Move shared defaults, MiB parsing, automatic preprocessing selection, and
minimum-length finalization into `dfs-cli-args` helpers where practical.
Avoid maintaining equivalent parsing logic independently in the two
executables.

Use `-S` to distribute candidate-class exact validations across workers.
The workers share a synchronized, sharded exact-reachability memo so parallel
validation retains reuse between candidate remainders.

Update usage text so `--require-completable` describes one shared phase-2
preparation plus exact validation, rather than "a phase-2 search per
candidate."

### 6. Filter all classes, then rank all surviving members

After the batch operation returns:

1. iterate every class;
2. discard classes marked non-completable;
3. flatten all allowed members of surviving classes;
4. apply the existing single- versus multi-word bonus calculation;
5. run the existing partial sort and `-n` truncation.

Do not use an output heap or its current floor to skip class validation.

Add diagnostics for:

- total classes checked;
- classes rejected directly by a conclusive bound;
- classes accepted directly by an exact bound;
- classes requiring exact validation;
- exact memo states computed and memo hits;
- phase-2 setup and validation wall time.

These counters are needed to distinguish cache-preparation cost from exact
fallback cost on heavily projected inputs.

## Correctness checks

Keep tests minimal but cover the cache distinction that caused the design
change.

1. Preserve the existing `query-index` output tests when
   `--require-completable` is absent.
2. Keep the synthetic `wxyz` smoke case:
   `wx` and `yz` are completable; `xy` leaves `wz` and is not.
3. Run that case with `-d 0` so every letter is projected. The projected
   length-only state can look reachable, but exact validation must still
   reject `xy`.
4. Run the same fixture with `-D` and confirm dense and projected modes return
   identical exact output.
5. Include a phrase/word-bonus fixture where a lower raw-frequency
   multi-word member rises above the eventual `-n` cutoff. Confirm that the
   entire vocabulary was filtered before ranking.
6. Include `--words-only --require-completable` coverage showing that phrases
   may still complete a displayed single-word candidate but are not
   themselves printed.
7. Verify dictionary filtering affects both candidate entries and completion
   paths.
8. Run the existing `dfs-search`, `dfs-cli`, and `query-index-cli` smoke
   tests.

## Performance verification

Use `IDX=~/code/nutrimatic/idx/wiki-merged.5.index` and source `setup.sh` for
S6 inputs. Before collecting timing data, confirm that no other
`dfs-anagrams` process is running.

Compare total wall time and diagnostics for matching `dfs-anagrams` and
`query-index --require-completable` invocations at several bag sizes:

- a roughly 24-letter bag whose exact projected table fits;
- a larger bag that genuinely merges wildcard letters;
- a dead-end-heavy input;
- a dictionary-filtered input.

Success criteria:

- phase 1 and score-bound preparation occur exactly once;
- the 24-letter regression finishes near the shared cache-setup time and
  normally before `dfs-anagrams`;
- projected inputs return exactly the same feasibility decisions as dense
  mode on cases where dense mode fits;
- there is no per-class trie extraction or per-class cache allocation;
- large projected cases show shared exact memo reuse rather than repeated
  DFS work.

This is an empirical performance target, not a strict proof that every
possible `query-index` invocation beats `dfs-anagrams`.

## Cleanup and review

- Remove the current `subtract_class_key()`, per-class `DfsClassList`,
  per-class `DfsAnagramSearch`, and `unordered_map<class_key, bool>` path from
  `query-index.cpp`.
- Retain or revise the existing synthetic-index additions only as needed by
  the final smoke tests.
- Update `plans/query-first-solution.md` or mark it superseded so its
  cache-disabled per-class design is not mistaken for the final approach.
- Run `/review` before committing, per `AGENTS.md`, and address all correctness
  or performance findings.
