# Plan: `pairs --threads N`

## Outcome

`pairs --threads N` checks the pairs of letter-count groups on N threads.
The default is 1, which keeps the current single-threaded path. 0 means
all the hardware threads, and larger requests are capped at that number.
That is the same handling `bad-bags --threads` uses.

Output order is not preserved. Each run prints the same set of lines, but
the order can differ between thread counts and between runs.

## Work to split

In `Pairs::each_line` (`source/pairs-impl.cpp`), almost all the work is the
outer loop over first groups `a`. For each `a`, it:

1. computes `remaining = bag_ - keys_[a]`;
2. emits the pairs within `groups_[a]` when `keys_[a]` fits `remaining`
   (`==` under `--exact`);
3. finds every later group `b` (`b` in `[a+1, n)`) whose key fits
   `remaining`, or under `--exact` the one group whose key equals it, and
   emits `groups_[a] × groups_[b]`.

Each `a` reads only `bag_`, `keys_`, `groups_` and `group_indices_`, and
nothing changes them after `add()` has finished. So the iterations are
independent. Each unordered group pair {a, b} with a < b is checked only by
the iteration for `a`, so splitting the `a` range across threads never
duplicates or misses a pair.

## Algorithm

Shared:
- `bag_`, `keys_`, `groups_`, `group_indices_`: read-only.
- `std::atomic<size_t> next{0}`: the next unclaimed `a`.

Per worker:
- `remaining`, a `matching_groups` scratch vector, and an output buffer.

Each worker:

```
loop:
  a0 = next.fetch_add(CHUNK)
  if a0 >= n: done
  for a in [a0, min(a0 + CHUNK, n)):
    run the current loop body for a, emitting into the buffer
  hand off the buffer if it has passed the flush threshold
hand off the rest of the buffer
```

`CHUNK` is 64 groups. The inner loop for `a` does `n - a - 1` fit checks,
so the cost per chunk falls steadily as `a` grows. Small chunks claimed
from the atomic counter balance that load without any other scheduling.

The single-word lines (`--mx 1,…`) are emitted on the calling thread before
the workers start.

## Output

- `write(FILE*)`: each worker appends whole lines to a `std::string` and,
  once it passes 64 KiB, `fwrite`s it to `out`. glibc locks the stream for
  each `fwrite` call, so a buffer lands intact and lines from different
  workers never interleave.
- `lines()`: each worker collects its own `std::vector<std::string>`. These
  are concatenated after the join. pgui (`source/gui/find-pairs.cpp`)
  already sorts the result.

## Code changes

- `PairsOptions` gets `int threads = 1`, already resolved to a concrete
  count by the caller.
- `each_line` is split into the single-word part and a function that runs
  the pair loop over `[a0, a1)` with its own scratch state and emitter.
  `write()` and `lines()` call the single-word part, and then either call
  the range function over `[0, n)` once (1 thread) or start the workers.
- `source/pairs.cpp` adds `--threads N` using `parse_count`,
  `resolve_search_threads` and the `hardware_concurrency()` cap, as in
  `source/bad-bags.cpp`, plus a `dfs_help_option` line.

pgui keeps the default of 1 thread.

## Test

One smoke case in `source/test-pairs.sh`: the sorted output of
`--threads 4` matches the sorted output of `--threads 1` for a dictionary
large enough to span several chunks.
