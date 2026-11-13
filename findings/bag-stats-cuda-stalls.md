# bag-stats-cuda --unique: the walk's tail runs on a few warps

A `--ss ppb,ppb,pb,pb --uc` run on the s7s8 bag (21 distinct letters, 2^47
rest numbers; 4,985 prospect bags) reached 4983/4985 pair bags at about
1h01m and then sat there, "~2s left", for at least 50 minutes more.
`nvidia-smi` showed 100% utilization at 28 W.

```
bag-stats-cuda --threads 0 --no-nopairs --ss ppb,ppb,pb,pb --uc \
    --pp tmp/s7s8/pb3-dict/pp-1sq-5k/pairs BAG tmp/s7s8/pb3-dict/pairs \
    $DICT --progress 15
```

Its reports before the walk:

```
pair sums table of the PAIRS bags: no split of at most 4 letters into at
    most 65536 slabs fits (one slab needs 1054766.12 GB, 11.58 GB free)
fit tables: 11 of 21 letters dense, triple table built, 5.50 GB; optimal
    12 dense with the triple table needs 29.84 GB (constrained)
pair usage memo: 2^29 slots for 2^47 rest numbers, 4.29 GB of optimal
    562949.95 GB (constrained)
pair usage walk: fit tables (pilots: fit tables 4.233s, memo 7.846s)
```

Status: the diagnostic, the reproducer and the re-split (defer and
relaunch) are built and run; measurements are under "Measured". The old
walk's time on the sample and the heartbeat's cost are still to be
measured, and the changes have not been through `/code-review`.

## What is happening

1. **The pair-sums walks are out for this bag** (a slab is a million GB),
   so `choose_usage_walk` runs the task walk, with fit tables. That is the
   walk findings/bag-stats-cuda-unique.md measured at 4h21m on S1, where a
   lookup that misses sends its whole warp through bucket walks.
2. **A task is one warp's for its whole life.** `count_unit_tasks` splits
   each first pick into tasks of about 64 second-pick candidates
   (`TASK_CANDIDATES`); in `walk_pair_usage` each warp takes the next task
   and walks everything below it alone. What a task costs is the subtree
   below those candidates, which the split doesn't see and which varies by
   orders of magnitude.
3. **When the tasks run out, the other warps return.** The few tasks
   still running, here in 2 units, keep one warp each until the kernel
   ends. Nothing gives them more of the GPU.
4. **"Utilization" is not occupancy.** `nvidia-smi` reports the share of
   the sample window in which any kernel was executing; one busy warp
   reads as 100%. Power is the occupancy signal: 28 W on the RTX 4080 is
   close to idle.
5. **The estimate is blind to running tasks.** A task adds its work to
   `WalkProgress` when it finishes, so `finished_share` sees none of the
   work in flight and the time left stays at a second or two.
6. **The pilot is blind to the tail too.** `pilot()` stops when 90% of
   its 32,768 sampled tasks have finished (or at 0.4s) and extrapolates by
   tasks finished. It estimated 4.2s for a walk that takes over 1h50m, and
   picked fit tables over memo on the quick tasks alone.

## Can a long task be told in advance?

Not reliably. The only thing known before a task runs is its first-step
candidates; the cost is in the bucket walks below, which depend on how
each rest's sparse letters sit against the fit tables. It is cheap to tell
at run time: the warp already counts its work a task.

## Built: seeing the tail

In `source/bag-stats-gpu.cu`, on the `UsageWalk` task walks, with
`--progress`:

1. **A tasks line** with each progress report once every task of the
   launch has been taken: `tasks: launch L: E of T ended, R running on W
   warps`. R of W is the occupancy.
2. **A stall report** after 4 progress reports with no task ended, and
   again at 8, 16, ...: up to 32 running tasks, each with its number, which
   of its unit's tasks it is, the unit's pair, and as of the task's last
   pick below the first: that pick's pair, the candidates scanned below the
   first pick and the candidates scanned in all.
3. **How:** each warp keeps 4 words in a device array (`running`, 65,536
   warps, 2 MB): the task at its start, cleared at its end, and the other
   three written at each pick below the first. The host reads the array
   only for a stall report.

## Built: reproducing the tail without the hour

1. `BSC_TASK_STRIDE=N` (already there) walks every Nth task. The quick
   ones drain in seconds and what is left is the stall state.
2. `BSC_TASKS=T,T,...` (new) walks just those tasks. Task numbers are the
   same from run to run for the same inputs.
3. Either way the run reports `walked X of T tasks in Ns; the counts are
   of those tasks only`, and prints those partial counts, so two walks of
   the same tasks can be timed and their output compared.
4. `BSC_UC_MODE=t` keeps the pilots from choosing a different walk
   between runs. `BSC_TASK_BUDGET=0` turns the re-split off.

## Built: defer and relaunch

A task that has scanned `TASK_BUDGET` (2^20) candidates, once the launch
has no task left to take, stops at its next pick below the first and
writes the rest of its range to a list: the run it is in, in parts of an
eighth of the picks it made, and the runs after it in up to 8 parts. The
host relaunches the walk on the list, and again on what that launch
defers, until a launch defers nothing. Usage counts are sums, so the parts
of a task add up to the task whichever warp and launch walk them.

- While tasks are left to take nothing is deferred, so a walk with no
  tail runs as before, in one launch.
- Memory: two lists of 2^21 rests of 24 bytes, 96 MB. A task that finds
  the list full walks on.
- It splits below the first pick only. The lookup step under one second
  pick is not split, and one second pick is what the end of each launch
  waits on.
- Walks of 2 steps, and the distinct-rests and top-rows walks, don't
  defer.

## Measured

s7s8 command above, RTX 4080, nothing else on the GPU, `BSC_UC_MODE=t
BSC_TASK_STRIDE=200`: 839 of the walk's 167,653 tasks, on 928 warps.

| Walk | Sample time | Launches | Rests deferred |
|---|---|---|---|
| no re-split | not finished at 150s, 4 warps busy from about 2m05s | 1 | 0 |
| re-split, parts of the picks made | 220.8s, 218.7s | 5 | 7,139, 7,152 |
| re-split, parts of an eighth of them | 156.6s | 5 | 28,925 |

The three re-split runs print the same output, though they split in
different places.

What the sample shows:

1. **The stall is there within 20s.** 728 of the 839 tasks end in 5s;
   4 are still running at 2m.
2. **Long tasks hold many second picks.** The split is by prefix cell
   (chunk), and a cell can hold most of a unit's candidates: task 7400
   (the first of 74 of "train line") was at its 828th candidate and task
   14800 at its 780th, where a task is meant to have about 64.
3. **Each second pick is about 100k candidates of work**, and slow: one
   warp was at a single second pick for over 20s. This is the fit tables'
   lookups, not the split.
4. **Each launch still ends on a few warps**, for 20 to 40s, waiting on
   single second picks. Smaller parts shorten it (220s to 157s) but can't
   go below one pick.

For the full run this should take the tail from over 75 minutes to a few
minutes. It does nothing for the hour before the tail, when all 928 warps
were busy.

## Tried and dropped: taking deferred rests inside the kernel

One launch, with idle warps waiting for deferred rests instead of
returning. On the same sample it had not finished at 400s: a deferring
warp is the only one splitting, so the rests arrive a few at a time and 10
to 17 warps stay busy. It also left about 490 rests taken but never ended,
which was not tracked down. The source as it stood is not kept in the
tree.

## Proposed, not built

1. **Split the lookup step.** A deferred rest that names the second pick
   and a range of the last step's chunks would let one slow pick be
   shared out, and end the launches' tails.
2. **The pilot.** Extrapolate by work, not tasks finished, or at least
   report the sampled tasks left unfinished, so the choice of walk and the
   estimate stop ignoring the tail.
3. **The lookups themselves.** At 928 warps and about an hour of full
   occupancy, the walk is slow before any tail; the dense letters are one
   short of optimal (11 of 21, 12 needs 29.84 GB).

## To do

1. The old walk's time on the stride-200 sample, and its output against
   the re-split's.
2. Time the walk with and without the `running` writes (S1SMAL
   `pb,pb,pb --min-pb-len 13`).
3. `/code-review`.
