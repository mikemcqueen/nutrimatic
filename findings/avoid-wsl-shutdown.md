# Why a big find-anagrams run kills every WSL session

Running `find-anagrams` on a large search space takes down **all** WSL sessions a
few seconds later, including ones in unrelated terminals. Newly opened sessions
then exit immediately, and only `wsl --shutdown` from PowerShell recovers.

This is two separate problems stacked: a WSL/systemd configuration that turns one
OOM kill into a distro-wide teardown, and a change on the `optimizing` branch
that removed the accidental memory ceiling which used to keep us away from it.

## What the journal actually records

Reproduced twice on 2026-07-23 (boots `-2` and `-1`). From `journalctl -b -1`:

```
15:17:28 kernel: find-anagrams invoked oom-killer: gfp_mask=0x140dca, order=0, oom_score_adj=0
15:17:28 kernel: oom-kill:constraint=CONSTRAINT_NONE, global_oom,
                 task_memcg=/init.scope, task=find-anagrams, pid=3524, uid=1000
15:17:28 kernel: Out of memory: Killed process 3524 (find-anagrams)
                 total-vm:26637256kB anon-rss:15429480kB   [swapents 1005813 pages = 4.0 GB]
15:17:28 systemd[1]: init.scope: A process of this unit has been killed by the OOM killer.
   ... 85 seconds ...
15:18:53 systemd[1]: init.scope: Stopping timed out. Killing.
15:18:53 systemd[1]: init.scope: Killing process 2 (init-systemd(Ub) with signal SIGKILL.
15:18:53 systemd[1]: init.scope: Killing process 7 (init) with signal SIGKILL.
15:18:53 systemd[1]: init.scope: Killing process 384/2928/2957 (SessionLeader) ...
15:18:53 systemd[1]: init.scope: Killing process 386/2930/2958 (Relay(...)) ...
15:18:53 systemd[1]: init.scope: Killing process 387/2932/2959 (bash) ...
15:18:53 systemd[1]: init.scope: Killing process 3158 (claude) ...
15:18:53 systemd[1]: init.scope: Failed with result 'oom-kill'.
15:18:53 systemd[1]: init.scope: Consumed 6min 38.198s CPU time, 14.9G memory peak, 3.9G memory swap peak.
```

The VM has 15.8 GB RAM + 4 GB swap. `find-anagrams` reached 15.4 GB resident plus
4.0 GB swapped — 19.4 of 19.8 GB. Nothing was left for anyone.

## Part 1: one cgroup holds the entire distro

WSL puts everything in a single cgroup, and systemd's default policy is to tear
down a whole unit when any process in it is OOM-killed.

```
$ cat /proc/self/cgroup
0::/init.scope

$ systemd-cgls /init.scope
├─   1 /sbin/init
├─   2 /init                          <- forks a SessionLeader per wsl.exe launch
├─   7 plan9 --control-socket 7 ...   <- backs drvfs / \\wsl$
├─ 405 /init
├─ 407 -bash
├─ 525 -bash
├─ 548 -bash
└─ ... every SessionLeader, Relay, bash, and child on the box

$ systemctl show init.scope -p OOMPolicy
OOMPolicy=stop                        # from DefaultOOMPolicy=stop
```

So the chain is:

1. Global OOM fires and kills `find-anagrams` — correctly; it was the hog.
2. `find-anagrams` was in `/init.scope`, so systemd sees an OOM kill in that unit
   and, per `OOMPolicy=stop`, begins stopping **the entire scope**. SIGTERM goes
   out within a second or two. That is when the terminals drop — the "few
   seconds" delay.
3. Stopping the unit that contains PID 1 and the plan9 server cannot complete, so
   after `TimeoutStopSec` (~90 s, matching the observed 85 s gap) systemd SIGKILLs
   the survivors, including PID 2 `/init` and PID 7 `plan9`.
4. `init.scope` is now `failed`, `/init` is dead, and `plan9` is dead. New
   `wsl.exe` invocations have nothing to fork a `SessionLeader` from and no file
   server, so they exit immediately. Only `wsl --shutdown`, which rebuilds the
   VM and a fresh `init.scope`, recovers.

**Any** process that triggers global OOM from a WSL shell does this. It is not
specific to Nutrimatic.

Worth naming the fix that does *not* work: raising `oom_score_adj` on
`find-anagrams`. The killer already chose it. Victim selection was never the
problem; the policy reaction to it is.

## Part 2: the branch removed the accidental memory ceiling

`vm.overcommit_memory` is `0` (heuristic). In that mode the kernel refuses a
**single** mapping larger than total RAM + swap, but otherwise hands out
whatever is asked for.

Before commit `7edb4de`, the frontier was `std::priority_queue<Next>` over one
`std::vector`. Growth meant one contiguous request for 2x the current capacity.
Once the frontier passed roughly half of RAM+swap, the next doubling was a single
request larger than 19.8 GB — refused outright, giving a clean `bad_alloc` with
gigabytes still free. That was never a designed limit. It was the doubling
schedule accidentally capping the frontier at ~50% of memory, and failing *by
refused reservation* rather than *by touching pages*.

`NextQueue` (`source/search.h:177`) has neither property:

- `buckets` is `std::vector<std::vector<Next> >`. The frontier is spread over
  thousands of per-bucket vectors that each double independently, so no single
  request ever approaches the heuristic threshold. `push()` cannot fail; it just
  faults in more pages.
- Nothing bounds `count`. `settle()` recycles exhausted buckets but caps nothing.

The failure mode therefore moved from "refused allocation at ~50% of memory" to
"page-fault to 100% of RAM *and* 100% of swap, then global OOM." The
`swapents 1005813` (4.0 GB, i.e. all of it) in the OOM report is the tell — the
old code never got far enough to fill swap.

Two smaller contributors, both real:

- `queue_median_score()` used to build a `std::vector<float>` sized to the whole
  frontier, explicitly "at the moment memory is tightest." That was a *second*
  large-contiguous-allocation site that could fail cleanly, and commit `7edb4de`
  removed it too.
- `allowed_chars` (`70cb11d`), `-march=x86-64-v2` (`99f3e37`) and static LTO
  (`4b88d25`) make the search substantially faster, so exhaustion arrives sooner
  in wall-clock time. Not causal, but it is why this surfaced now.

### Ruled out

- `bucket_of()` cannot run away. `log_score` stays finite: counts are always >= 1
  so `log2f` never sees zero, and depth only adds a bounded negative per restart.
  The bucket count grows linearly with search depth, and empty bucket headers are
  24 bytes each — megabytes, not gigabytes.
- `MIN_CRUMBS` is 30M crumbs x 8 bytes = 240 MB. Noise against 19 GB.

### One comment to correct

`settle()` (`source/search.h:214`) says "the memory behind exhausted scores is
released as the search descends past them." It is released to glibc's free list,
not to the OS. After the first few multi-MB bucket vectors are freed, glibc
raises its dynamic mmap threshold to the 32 MB cap, so later buckets come from
the brk heap and freed ones return to the kernel only if they sit at the top and
exceed the 64 MB trim threshold. RSS is a high-water mark here.

## Fixes

### 1. Stop WSL amplifying one OOM into a distro death

Do this regardless of the code, because anything can trigger it:

```bash
sudo mkdir -p /etc/systemd/system/init.scope.d
printf '[Scope]\nOOMPolicy=continue\n' | sudo tee /etc/systemd/system/init.scope.d/oom.conf
sudo systemctl daemon-reload    # takes effect after the next wsl --shutdown
```

An OOM kill inside `init.scope` is then logged and ignored; only the offending
process dies and other sessions survive.

### 2. Never reach global OOM in the first place

Run the search in its own memory-capped cgroup, which makes a system-wide event
local:

```bash
systemd-run --user --scope -p MemoryMax=10G -p MemorySwapMax=0 \
  build/find-anagrams idx/wiki-merged.5.index ...
```

### 3. Give the frontier a real ceiling in code

The bucket queue makes this *easier* than the heap did: the highest-index buckets
hold exactly the lowest-scoring entries, so a budget can be enforced by
discarding whole buckets off the back of `buckets` — O(1) per bucket, no
selection pass, and it drops precisely what a best-first search would have
explored last. The heap could not offer that. Pair it with the `crumbs` GC, which
reclaims the discarded history for free on the next `collect()`.

A cruder alternative that restores the old behaviour exactly, without touching
search semantics: `setrlimit(RLIMIT_DATA, ...)` in `find-anagrams`' main.
`RLIMIT_DATA` covers brk and anonymous mmap but **not** the file-backed index
mapping, so it caps the heap without having to account for index size — and
`bad_alloc` comes back at a bound we choose rather than one the allocator's
doubling schedule hands us. (Note `cgi-search.py` uses `RLIMIT_AS` instead, which
*does* count the index mapping; see `findings/how-to-limit-runtime.md`.)

Neither of these is implemented yet.

## Environment this was observed on

| | |
|---|---|
| WSL | 2.7.3.0, kernel 6.6.114.1-1 |
| Windows | 10.0.26200.8894 |
| Distro | Ubuntu, systemd 255 (255.4-1ubuntu8.16) |
| VM memory | 15.8 GB RAM, 4 GB swap (no `memory=` in `.wslconfig`) |
| `vm.overcommit_memory` | 0 (heuristic) |
| `systemd-oomd` | inactive |
