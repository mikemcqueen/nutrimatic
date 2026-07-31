# Top-N heap contention during parallel search

## Observation

A `dfs-anagrams` run with 20 requested search threads and a top-N limit of
10,000,000 showed low total CPU utilization, equivalent to roughly two busy
logical processors.

Verbose scheduler diagnostics established that:

- task splitting completed immediately after producing 164,360 live tasks;
- all search workers 0 through 19 were created; and
- every worker claimed its first task.

The run therefore was not limited by serial task splitting, a shortage of
queued work, or failure to create worker threads.

Running the same workload with `-n 0` used all available processors. This
isolates the poor scaling to output collection rather than the concrete DFS
scheduler.

## Shared output bottleneck

Every completed class solution calls `DfsTopN::emit()`. It constructs a
`DfsSpelling` and its word-set key outside the mutex, but then takes the single
`DfsTopN::heap_mutex`.

The following work occurs while holding that mutex:

- checking the authoritative heap score floor;
- looking up the word-set key in the deduplication hash map;
- allocating and inserting hash-map nodes;
- copying or replacing spellings in the heap;
- inserting into or replacing an element of the heap;
- sifting the heap in `O(log N)` time;
- updating hash-map positions for every heap swap; and
- publishing a new score floor.

With `-n 10000000`, the heap remains below its result limit for a long time.
There is consequently no useful full-heap score floor, and nearly every unique
expanded spelling enters this serialized path. The heap and hash map reserve
for the configured limit, but progressively touching their storage and
allocating map nodes can still incur page faults and allocator work while the
mutex is held.

## Why only one or two threads appear busy

Mutex acquisition is not a fair random lottery. A normal `std::mutex` may
allow the current owner to release and reacquire the mutex before waiters that
were put to sleep through the futex path are scheduled. One or two already
runnable threads can therefore repeatedly barge ahead of the other workers.
The waiting workers consume little CPU because they are blocked rather than
spinning.

Windows Task Manager graphs logical processors rather than stable Linux worker
identities, and WSL may migrate workers between processors. Nevertheless, the
observed low aggregate CPU use is consistent with a mutex convoy around the
top-N heap.

The task cursor is not a plausible bottleneck: it performs one relaxed atomic
increment per coarse search task. The measured run also used complete
bottom-up projected bounds, so lazy score-bound compare/exchange loops were not
active during concrete search.

## Yielding after unlock

C++ provides `std::this_thread::yield()`, and it could be called immediately
after releasing `heap_mutex`. This might sometimes let a woken waiter acquire
the mutex before the previous owner returns.

It is not an expected performance fix:

- `yield()` is only a scheduling hint and provides no fairness guarantee;
- yielding for every spelling could cause millions of context switches;
- the waiter may not yet be runnable when the owner yields;
- rotating ownership moves the heap and hash-map cache lines between cores;
  and
- fairness does not parallelize the serialized critical section.

Unfair reacquisition may actually maximize throughput by preserving cache
locality, despite producing unattractive CPU-utilization graphs. Any yield
change should therefore be treated strictly as a benchmark experiment and
judged by wall time, not fairness or apparent core utilization.

## Better optimization directions

The fundamental improvement is to reduce or restructure work under the shared
mutex:

1. Accumulate worker-local spelling batches and merge one batch per lock
   acquisition.
2. Move all possible allocation and copying outside the critical section, then
   move prepared objects into shared storage.
3. Keep worker-local result structures and merge them periodically or after
   search.
4. Shard output and deduplication state, followed by an exact final merge.

These approaches must preserve global deduplication, the exact top-N result,
and timely publication of a valid score floor. Worker-local top-N heaps cannot
simply retain N results each without considering their potentially excessive
memory use and the interaction between the global score floor and pruning.

Useful comparisons when evaluating changes are:

- `-n 0`, which removes output collection and exposes scheduler scaling;
- a small top-N limit, which establishes a pruning floor early;
- the production top-N limit; and
- the unusually large 10,000,000-result case that exposes heap contention.

