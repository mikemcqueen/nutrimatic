# Making long anagram searches tractable

Goal that prompted this: find 4-word anagrams of a ~26-letter bag, each word at
least 4 letters. `find-anagrams` handles the letter bag well but falls over
somewhere past 16 letters.

## Measurements

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

- `PrintAll` writes its `# <count>` progress lines to **stdout**
  (`source/search-printer.cpp:10-13`), so `| head -5` consumes progress markers,
  not results, and SIGPIPEs the search after 500k steps. Filter with
  `grep --line-buffered -v '^#'` first.
- `$?` after a pipeline is the last stage. Use `${PIPESTATUS[@]}` and read the
  first element: `124` = `timeout` expired, `141` = SIGPIPE from `head`,
  `134` = SIGABRT (uncaught `bad_alloc`), `137` = OOM killer, `0` = completed.
  The shell's bare "Terminated" message is bash reporting SIGTERM, not the tool.

## Why it blows up

`SearchDriver` is a best-first search ordered by `choice.count * scale`
(`Next::operator<` in `source/search.h`). Three compounding problems:

**Short frequent words dominate the queue.** `up`, `in`, `it`, `on`, `the` have
enormous counts, so they sort to the top. The search spends itself on exactly
the word lengths the goal excludes.

**Every k-word solution is explored in all k! orderings.** `AnagramFilter` only
tracks the consumed letter multiset, so word order is free. Visible directly in
the output — `pilot but when` and `but when pilot` come back at *identical*
scores, meaning they tie in the queue and both get expanded. At 6 words that is
720x redundant work.

**Nothing pulls toward completing the bag.** Priority is pure greedy on prefix
frequency. `scale` only ever shrinks (by `count/total * 1e-6` per restart,
`source/search-driver.cpp:78-88`), so deep near-complete paths always sort below
shallow frequent ones and never get expanded.

Memory then follows from frontier size: `Next` is ~48 bytes (`int crumb`,
`double scale`, `Choice {char ch; int64_t count; Node next;}` from
`source/index.h:70`, `State state`, plus padding), and `priority_queue` sits on
a `vector`, so the fatal moment is a **doubling** needing old and new buffers
live at once — effectively OOM at a third of available RAM.

## Ideas, by expected leverage

### 1. Encode the real constraints into the filter

Minimum word length 4 and a maximum word count. These aren't just output
filters — they prune the highest-priority junk in the queue, and 25 letters at
>=4 each caps the search at 6 words / 5 restarts. Nothing currently stops the
search splitting into fifteen two-letter words, and those paths sort near the
top.

Fits as extra state multiplied into `AnagramFilter`'s existing mixed-radix
arithmetic — no allocation. Expected to be the difference between "times out at
20" and "finishes at 26". **Do this first and re-measure.**

### 2. Canonical word ordering

Require each word's first letter to be >= the previous word's. Collapses k!
orderings to 1, and is *sound* for finding word-sets: `listen` and `silent`
start with different letters and both survive; only reorderings of the same set
are cut.

Costs a 26x state multiplier. For 26 distinct letters that's ~1.8e9 — inside
`int` (`SearchFilter::State`) but uncomfortably close to the 2.1e9 ceiling.

Loses phrasing quality in ranking: `pen built` scored **7** as a contiguous
corpus phrase (`scale == 1.0`) while `built pen` needed a restart at 2.1e-05.
Negligible for long bags, visible for short ones — so make it a flag, not the
default.

**Rejected alternative:** dedup on `(root, anagram_state)` at restart points
(one bit per state, 4 MB for 2^25) is *not* sound — `listen ` and `silent `
reach an identical state and one would be lost. Only viable as a deliberate
approximation, or as "keep top-M paths per state".

### 3. Goal-directed priority

- Normalize by letters consumed: order by `log(count*scale) / letters_used`
  rather than the raw product, so deep efficient paths compete with shallow ones.
- Crude A*: add `letters_remaining * (average log-frequency per letter)` as an
  optimistic completion estimate. Even a constant estimate helps.

### 4. Bound the frontier

- **Beam / queue cap**: drop the worst tail when `nexts` exceeds N. Bounds
  memory directly, makes results approximate. Useless alone — it discards
  exactly the deep entries needed — so only after #3 fixes the ordering.
- **Score floor**: discard entries below an absolute threshold. One line, no
  correctness cost if the floor sits below target scores.

### 5. Shrink per-entry cost

`Next` at ~48 bytes can reach ~24: `float` log-score instead of `double` scale,
`int32` count, field reordering to kill padding. Straight 2x on frontier
capacity for mechanical work. Also `reserve()` the queue up front, or move to a
deque-backed or bucketed/radix heap, to avoid the doubling cliff.

### 6. Free the crumbs

`crumbs` grows one 8-byte entry per step and is never released
(`source/search-driver.cpp:38-51`) — ~290 MB at 36M steps — and
`new_next.crumb` is an `int`, so it hard-breaks at 2.1e9 regardless. Refcounting
or an arena with periodic compaction. Secondary to the frontier.

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

`perf record` on the 16-letter run to confirm time goes where assumed (queue
operations and `children()` decoding) rather than somewhere unexpected. None of
the above has been measured — it is all reasoning from the source.

## Related

- `findings/how-to-limit-runtime.md` — the `# <count>` progress-line protocol
  and the CGI's computation/CPU limits.
