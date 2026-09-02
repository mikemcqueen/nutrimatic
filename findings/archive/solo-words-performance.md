# Solo-word scoring performance

Measured 2026-08-15 from the release build in `/home/mike/code/nutrimatic`.
Every timed run used `IDX=/home/mike/code/nutrimatic/idx/wiki-merged.5.index`
and a bag derived after `source ./setup.sh`. Before and after every run, both
`pgrep -ax query-index` and `pgrep -ax dfs-anagrams` returned no other process.

## Phase-1 compatibility

The alternating command pair used the bounded 28-letter bag
`${S6:0:28}` (`sowhyisitthatimustgoandleave`):

```bash
build/query-index "$IDX" "${S6:0:28}" -m 4 -n 100 >/dev/null
build/query-index "$IDX" "${S6:0:28}" -m 4 -n 100 \
  --solo-words example --word-bonus 0 --pair-bonus 0 >/dev/null
```

Both forms extracted 500,604 entries in 204,617 classes while visiting
3,326,933 trie nodes.

| Run | Baseline wall / RSS | Score-inert wall / RSS |
|---:|---:|---:|
| 1 | 0.59 s / 282,328 KiB | 0.60 s / 282,724 KiB |
| 2 | 0.58 s / 282,564 KiB | 0.60 s / 282,724 KiB |
| 3 | 0.60 s / 282,568 KiB | 0.61 s / 282,460 KiB |
| Median | 0.59 s / 282,564 KiB | 0.60 s / 282,724 KiB |

The score-inert path was 1.7% slower at the observed timer resolution and used
160 KiB (0.06%) more peak RSS. This is below the 2% investigation threshold,
is not repeatably outside run noise, and confirms that no context or profile
work is constructed when both effective bonuses are zero.

## Active profile scaling

These runs used the same query and `--word-bonus 1`:

| Solo words | Profiles | Word edges | Wall | Peak RSS |
|---:|---:|---:|---:|---:|
| 1 | 36,496 | 36,496 | 0.73 s | 312,140 KiB |
| 4 | 57,739 | 119,106 | 0.82 s | 348,004 KiB |
| 16 | 67,835 | 267,928 | 1.12 s | 393,036 KiB |

The lists were `the`; `the,and,of,to`; and
`the,and,of,to,in,a,is,that,for,it,on,with,as,was,be,by`. Runtime scales with
candidate/solo probes and edge count, while RSS also reflects additional pages
of the mmap index touched by the continuation probes. The packed member arena
does not grow: its compile-time 16-byte member and 24-byte intermediate checks
remain active.

## Bounded DFS

The real-index DFS used the 20-letter bag `${S6:0:20}`
(`sowhyisitthatimustgo`):

```bash
build/dfs-anagrams "$IDX" "${S6:0:20}" -m 4 -n 100 >/dev/null
build/dfs-anagrams "$IDX" "${S6:0:20}" -m 4 -n 100 \
  --solo-words the,and,of,to --word-bonus 0.000000001 >/dev/null
build/dfs-anagrams "$IDX" "${S6:0:20}" -m 4 -n 100 \
  --solo-words the,and,of,to --word-bonus 1 >/dev/null
```

| Mode | Phase-2 nodes | Solutions | Spellings | Wall | Peak RSS |
|---|---:|---:|---:|---:|---:|
| Baseline | 24,154 | 295 | 1,121 | 0.08 s | 69,352 KiB |
| Four solo, tiny bonus | 24,154 | 295 | 1,122 | 0.09 s | 102,980 KiB |
| Four solo, bonus 1 | 32,269 | 406 | 1,396 | 0.09 s | 102,976 KiB |

All three runs extracted 21,330 entries in 5,931 classes and performed
1,752,964 successful projected-bound transitions. Active runs resolved 4,794
profiles and 9,967 word edges. The tiny-bonus control preserves the baseline
search tree and adds only one concrete spelling, so the independent local
upper bound does not create a material capacity-looseness multiplier here.
The 1.34x node and 1.25x spelling increases at bonus 1 come with a materially
different objective and stronger promoted candidates, rather than appearing
in the near-inert control. Exact matching was exercised by the active spelling
expansions; the focused CLI tests separately cover actual shared-partner
scarcity and an augmenting-path reroute.
