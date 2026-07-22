# How to limit Nutrimatic's runtime

## The tools impose no limits themselves

`find-expr` → `PrintAll` just loops `step()` until the priority queue is empty
(`source/search-printer.cpp:8-21`). It has **no** max-results, max-time, or
max-nodes cap. Left alone on a broad pattern it runs essentially forever,
streaming ever-lower-scored matches, while the `crumbs` deque and `seen` set
grow the whole time.

## All limiting lives in the CGI wrapper

Everything that stops a search is layered on top of the unbounded binary by
`cgi_scripts/cgi-search.py`:

| Limit | Value | Mechanism |
|---|---|---|
| **Computation / nodes** | `MAX_COMPUTATION = 1000000` (overridable via `?comp=`) | Reads the `# <count>` progress lines the tool prints every 100k steps; when `count >= max_computation` it stops reading and prints "Computation limit reached." |
| **Results per page** | `PER_PAGE = 100` (via `start`/`num`) | Stops after `start+num` results, emits a "next page »" link. |
| **CPU time** | 30 s | `resource.setrlimit(RLIMIT_CPU, 30)` before `exec` — kernel sends `SIGXCPU`. |
| **Address space** | 2 GB | `resource.setrlimit(RLIMIT_AS, 2GB)` — caps the unbounded `crumbs`/`seen` growth. |

Note: the `# <count>` progress line printed by `PrintAll` isn't cosmetic — it's
the **throttle signal the CGI depends on**. The binary never decides to quit;
the wrapper watches the counter and kills the pipe (see the `SIGPIPE` handling in
the `preexec_fn`).

## Command-line use gets none of that

Running `build/find-expr wiki-merged.index '<pattern>'` directly gives you none
of the above limits. Practical options:

- **Cap results:** pipe through `head`, e.g. `build/find-expr idx '...' | head -50`.
  The binary gets `SIGPIPE` when `head` closes and exits cleanly.
- **Cap time:** wrap with `timeout`, e.g. `timeout 30 build/find-expr idx '...'`.
- **Cap CPU / memory:** `ulimit -t 30` / `ulimit -v 2000000` in the shell first,
  mirroring the CGI.
- **Filter the noise:** the `# 100000` / `# 200000` progress lines go to
  **stdout** interleaved with results, so add `| grep -v '^#'` if you don't want
  them.

For interactive CLI use, `| head -N` is usually all you need — a broad or slow
pattern will otherwise churn indefinitely because nothing in the tool ever says
"that's enough."

### Caveat: buffered output

Because stdout is block-buffered to a pipe and results aren't flushed per line,
`| head` and `| grep` may show output in lumpy bursts rather than a smooth
stream. Use `stdbuf -oL build/find-expr ...` to force line buffering if that
matters.
