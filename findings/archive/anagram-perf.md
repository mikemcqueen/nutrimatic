# Making long anagram searches tractable

Goal that prompted this: find 4-word anagrams of a ~26-letter bag, each word at
least 4 letters. `find-anagrams` handles the letter bag well but falls over
somewhere past 16 letters.

## Measurements

**These are the original baseline, taken before any of the ideas below were
built.** Ideas 1 (partly), 2, 5 and 6 have since landed, so the table describes
the problem as first found, not current behaviour. Nothing here has been
re-measured — see "Also worth doing".

Against `idx/wiki-merged.5.index`, prefixes of a letter string, 30 s timeout:

| Letters | Result |
|---|---|
| 8 | 5 results immediately (`7 pen built`, `2.1e-05 built pen`, `7.1e-06 up in belt`) |
| 12 | 5 results immediately (`1.9e-06 pilot but when`) |
| 16 | 5 results immediately (`4.97e-08 put him on the below`) |
| 20 | nothing in 60 s (not an OOM — SIGTERM from `timeout`) |
| ~25 | `std::bad_alloc` after ~36M steps, zero results |

Score decay is gentle — roughly one order of magnitude per two letters, so
extrapolating gives ~1e-09 at 20 letters and ~1e-11 at 25. Float precision is
nowhere near the limit. **The problem is frontier size, not score underflow.**

### Two gotchas when measuring

- `PrintAll` takes its progress stream as an argument now. `find-anagrams`
  passes **stderr**, so `| head -5` there sees results only; `find-expr` still
  passes **stdout**, because `cgi-search.py` parses the `# <count>` lines out of
  it, so the original hazard stands for that tool — `| head -5` consumes
  progress markers rather than results and SIGPIPEs the search after 500k steps.
  Filter with `grep --line-buffered -v '^#'` first. The line carries more fields
  than it did (`seen`, `crumbs`, `queue`, `median`), and `-p` on `find-anagrams`
  spaces the lines out.
- `$?` after a pipeline is the last stage. Use `${PIPESTATUS[@]}` and read the
  first element: `124` = `timeout` expired, `141` = SIGPIPE from `head`,
  `134` = SIGABRT (uncaught `bad_alloc`), `137` = OOM killer, `0` = completed.
  The shell's bare "Terminated" message is bash reporting SIGTERM, not the tool.

## Why it blows up

`SearchDriver` is a best-first search ordered by the product of a path's prefix
count and its accumulated restart penalty (`Next::operator<` in
`source/search.h`, now comparing `log_score`, which holds log2 of that product).
Three compounding problems:

**Short frequent words dominate the queue.** `up`, `in`, `it`, `on`, `the` have
enormous counts, so they sort to the top. The search spends itself on exactly
the word lengths the goal excludes.

**Every k-word solution is explored in all k! orderings.** `AnagramFilter` only
tracks the consumed letter multiset, so word order is free. Visible directly in
the output — `pilot but when` and `but when pilot` come back at *identical*
scores, meaning they tie in the queue and both get expanded. At 6 words that is
720x redundant work. *Addressed by idea #2, which shipped as `-c`; duplicate
output is additionally suppressed by `seen`.*

**Nothing pulls toward completing the bag.** Priority is pure greedy on prefix
frequency. The scale only ever shrinks (by `count/total * 1e-6` per restart, the
restart block at the end of `SearchDriver::step()`), so deep near-complete paths
always sort below shallow frequent ones and never get expanded. *Still true —
this is idea #3, and it is now the largest untouched problem in this document.*

Memory then follows from frontier size: `Next` was 48 bytes (`int crumb`,
`double scale`, `Choice {char ch; int64_t count; Node next;}` from
`source/index.h:70`, `State state`, plus padding), and `priority_queue` sits on
a `vector`, so the fatal moment is a **doubling** needing old and new buffers
live at once — effectively OOM at a third of available RAM.

`Next` is now 28 bytes (idea #5 below), so the numbers above understate current
capacity by ~1.7x. The doubling cliff itself is untouched.

## Ideas, by expected leverage

### 1. Encode the real constraints into the filter

**Half done.** Minimum word length shipped as `-m`, multiplied into
`AnagramFilter`'s mixed-radix state as described (`source/find-anagrams.cpp`);
it normalises a minimum of 1 away so the state space is untouched when the flag
is unused. **A maximum word count is still not implemented**, and remains the
cheaper half of the original claim: 25 letters at >=4 each caps the search at 6
words / 5 restarts, and nothing currently stops the search splitting into
fifteen two-letter words, which sort near the top.

These aren't just output filters — they prune the highest-priority junk in the
queue. Expected to be the difference between "times out at 20" and "finishes at
26"; **that prediction has never been re-measured**, including for the half that
shipped.

### 2. Canonical word ordering

**Done, as `-c`** — but *not* by the first-letter scheme sketched below, which
`findings/reduce-permutations.md` superseded. What shipped orders segments by
the **trie node** each ends on, which is a free collision-free segment ID with
no state multiplier at all, and which names contiguous multi-word segments that
a first-letter rule cannot. Read that document rather than this section for the
design as built; the rest of this entry is kept for the reasoning it records.

The original sketch: require each word's first letter to be >= the previous
word's. Collapses k! orderings to 1, and is *sound* for finding word-sets:
`listen` and `silent` start with different letters and both survive; only
reorderings of the same set are cut.

Costs a 26x state multiplier. For 26 distinct letters that's ~1.8e9 — inside
`int` (`SearchFilter::State`) but uncomfortably close to the 2.1e9 ceiling.
*This cost is what the node-ID approach avoids entirely.*

Loses phrasing quality in ranking: `pen built` scored **7** as a contiguous
corpus phrase (`scale == 1.0`) while `built pen` needed a restart at 2.1e-05.
Negligible for long bags, visible for short ones — so make it a flag, not the
default. *This is why `-c` is opt-in.*

**Rejected alternative:** dedup on `(root, anagram_state)` at restart points
(one bit per state, 4 MB for 2^25) is *not* sound — `listen ` and `silent `
reach an identical state and one would be lost. Only viable as a deliberate
approximation, or as "keep top-M paths per state".

### 3. Goal-directed priority

**Not started, and now the highest-leverage item here.** Ideas 5 and 6 bought
capacity; this is the one that changes what the search spends that capacity on.
Note idea #4 is explicitly blocked on this.

- Normalize by letters consumed: order by `log(count*scale) / letters_used`
  rather than the raw product, so deep efficient paths compete with shallow ones.
- Crude A*: add `letters_remaining * (average log-frequency per letter)` as an
  optimistic completion estimate. Even a constant estimate helps.

### 4. Bound the frontier

**Not started.**

- **Beam / queue cap**: drop the worst tail when `nexts` exceeds N. Bounds
  memory directly, makes results approximate. Useless alone — it discards
  exactly the deep entries needed — so only after #3 fixes the ordering.
- **Score floor**: discard entries below an absolute threshold. One line, no
  correctness cost if the floor sits below target scores.

### 5. Shrink per-entry cost

**Done, 48 -> 28 bytes** (not the ~24 estimated here). `float` log-score
instead of `double` scale, 32-bit node and count fields, and `IndexReader::Choice`
unpacked into `Next` rather than embedded — its own layout spends 7 bytes
padding `ch` out to the alignment of a 64-bit count, which turned out to be the
single largest saving. Note that no one of these pays off alone: alignment
quantizes `sizeof(Next)` to a multiple of 8, so any lone 4-byte narrowing is
absorbed by padding.

The original ~24 estimate was arithmetic error — it assumed `count` could be
dropped rather than narrowed. Seven 4-byte fields plus `char ch` is 25, padded
to 28.

**Remaining route to 24**, if the frontier is still the binding constraint:
replace the `float` log-score with 16-bit fixed point. At 1/64 log2 resolution
over the ~1000-unit range paths actually reach, that is ~1.1% quantization of
scores. Viable where 8 bits is not — what needs precision is *absolute*
resolution in the log, uniformly, and 8 bits over that range gives 3-4 log2
units per step, i.e. scores differing 8-16x comparing equal, which would
degenerate `operator<` into heap-insertion order. Rounding is monotonic, so the
non-increasing pop order `seen` depends on survives quantization. This is a real
precision concession rather than free, hence not taken.

Two constraints found while doing this, both worth knowing before narrowing
anything further:

- `count` cannot go below 32 bits. `IndexReader::count()` is 3,586,472,603 on
  `wiki-merged.5.index` — 84% of `UINT32_MAX`, so the current width already has
  only ~1.2x headroom, and one more corpus merge overflows it.
- `SearchFilter::State` cannot be narrowed to pack `ch` into its spare bits,
  which is the only other route to 24. `AnagramFilter` states are a mixed-radix
  encoding of the letter bag and the constructor deliberately allows them up to
  `INT_MAX - 1` (`source/find-anagrams.cpp:48-52`), so the full 31 bits are in
  use by design.

See `findings/priority-invariant.md` for the one semantic cost incurred: the
pop-order invariant behind `seen`'s dedup is now approximate rather than exact.

Still open from this idea: `reserve()` the queue up front, or move to a
deque-backed or bucketed/radix heap, to avoid the doubling cliff. That is the
larger win now — 28 bytes still doubles into two live buffers.

### 6. Free the crumbs

**Done**, as the second of the two options: `SearchDriver::collect()` marks
crumbs reachable from the frontier and compacts the rest away, triggered on a
threshold that adapts to the yield of the previous collection. The frontier is
the root set, so it can only run with the queue whole — see the comment on
`collect()` in `source/search.h`.

The original note said `crumbs` grows one 8-byte entry per step and is never
released — ~290 MB at 36M steps — and that `new_next.crumb` is an `int`, so it
hard-breaks at 2.1e9 regardless. The `int` is unchanged, but the ceiling now
applies to *live* crumbs rather than total steps, which is a far weaker
constraint.

### 7. Consider not using this driver

For 25 letters / 4 words the conventional approach wins easily: dump a word list
with counts via `dump-index`, keep words >=4 letters whose multiset fits the
bag, then recursive bag-subtraction. Seconds in Python, exact, constraints
native.

The trie-walk architecture's real advantage is scoring **contiguous** phrases,
which a 4-word 29-character anagram can't rely on anyway — it's near the 40-char
`HISTORY_WINDOW_SIZE` limit in `make-index.cpp` and would need to occur >=5
times to survive the merge cutoff.

## Also worth doing

**Re-measure.** The table at the top predates ideas 1 (partly), 2, 5 and 6, and
none of them was measured going in or coming out — the whole document is still
reasoning from the source. The specific claims left unverified are that idea #1
moves the ceiling from 20 letters to 26, and that ideas 5 and 6 together buy
enough capacity to matter against a problem whose real defect is ordering
(idea #3) rather than size.

`perf record` on the 16-letter run to confirm time goes where assumed (queue
operations and `children()` decoding) rather than somewhere unexpected. Worth
redoing now that `operator<` is a float compare and `log2f` runs once per push.

## Status summary

| Idea | State |
|---|---|
| 1. Constraints in the filter | Half — `-m` shipped, max word count not |
| 2. Canonical word ordering | Done as `-c`, via node IDs (see `reduce-permutations.md`) |
| 3. Goal-directed priority | Not started — **highest leverage remaining** |
| 4. Bound the frontier | Not started, blocked on #3 |
| 5. Shrink per-entry cost | Done, 48 -> 28 bytes; 24 reachable at a precision cost |
| 6. Free the crumbs | Done, `collect()` |
| 7. Don't use this driver | Standing alternative, unchanged |

## Related

- `findings/reduce-permutations.md` — the design that idea #2 actually shipped
  as, and the node-ID segment identity it rests on.
- `findings/priority-invariant.md` — the one semantic cost of idea #5.
- `findings/how-to-limit-runtime.md` — the `# <count>` progress-line protocol
  and the CGI's computation/CPU limits.
