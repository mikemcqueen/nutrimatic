# Plan: a rest cache for any bag, sized by --cache-size

## Outcome

`RestTable` caches a row's remainder counts for every LETTERS bag, however
long, in at most `--cache-size` MB:

```
bag-stats ... [--cache-size N] ... LETTERS PAIRS DICT
```

`--cache-size N`: hold at most N MB of cached remainder counts (default
1000); 0 turns the cache off.

Today `RestTable` numbers every sub-bag of R densely (`RemainderIndex`), and
disables itself when that count exceeds `PAIR_COUNT_TABLE_MAX` (2^28). The
55-letter `$S1TEST` bag has 1.49e11 sub-bags, so its cache is never
allocated: every pass-2 lookup misses ("cache hits 0/158638") and pass 2
recounts every remainder pass 1 already counted, about half the run.

## Key

A remainder's key is its mixed-radix number over R's letter counts, as
`RemainderIndex::number` computes it, held in a `uint64_t`. 1.49e11 needs
38 bits. `RemainderIndex` takes the bound as a parameter: `PairCountTable`
keeps `PAIR_COUNT_TABLE_MAX`, `RestTable` uses `UINT64_MAX - 1`. The
stored key is number + 1, so 0 marks an empty slot. A bag whose sub-bag
count passes that bound runs with the cache off.

## Table

Open addressing with linear probing over three `calloc`ed arrays:

- `keys_`: `uint64_t` per slot, number + 1, 0 while empty.
- `rows_index_`: `uint32_t` per slot, row index + 1, 0 until published.
- `rows_`: `BagStatsCounts` per entry, `capacity_` of them.

A key's home slot is `(hash(key) * slots) >> 64` (128-bit multiply), with
`hash` a 64-bit mixer (splitmix64's finalizer), so `slots` needn't be a
power of two. `slots = 2 * capacity_`: the table is never more than half
full, so a probe always reaches an empty slot or its key.

`get(rest)`: probe from the home slot. Empty slot → miss. Matching key →
load `rows_index_` with acquire; 0 → miss, else copy `rows_[i - 1]`.

`put(rest, counts)`: return if `next_ >= capacity_`. Probe; on an empty
slot CAS its key 0 → key; losing the CAS to the same key, or finding it,
returns (another thread is filling it); losing to another key keeps
probing. The winner takes `i = next_.fetch_add(1)`; `i >= capacity_`
returns, leaving the key with no row so `get` misses. Otherwise write
`rows_[i]`, then store i + 1 to `rows_index_` with release.

Both passes use the same `get`/`put`, lock-free. `RowWalker` doesn't
change. The dense `RestTable` path goes away; short bags use this table
too.

## Sizing

```
// The entries a rest cache of `megabytes` MB holds.
size_t rest_cache_entries(size_t megabytes);
```

Each entry costs `sizeof(BagStatsCounts)` plus two slots of
`sizeof(uint64_t) + sizeof(uint32_t)`: 56 + 2 * 12 = 80 bytes. 1000 MB
holds 13,107,200 entries. `capacity_` is
`min(rest_cache_entries(N), R's sub-bag count)`, so short bags allocate
only what they can use.

`RestTable(bag, enabled, megabytes)`: off when `!enabled`, `megabytes`
is 0, the bag passes the key bound, or an allocation fails. The comment
about staying under 4 GiB with the pair table gets replaced: the pair table
keeps its own bound and `--cache-size` bounds this table alone.

## Progress

- Startup: `cache holds N entries (M MB) of R remainders`, so a run shows
  whether `--cache-size` could cover the bag.
- Once, when the last entry is stored: `cache full at N entries; later
  remainders go uncached`.
- Each progress line: `, cache off`, or `, cache hits N/M (P%)` on every
  pass.
- Pass 2 always prints its `done in` line, however short, so its hits
  show.

All three stay quiet under `--progress 0`.

## Arguments

`OPT_CACHE_SIZE`, `--cache-size`, parsed with `parse_count` into
`Args::cache_size_mb` (default `DEFAULT_CACHE_SIZE_MB = 1000`). It goes in
the usage synopsis next to `--threads N` and gets a `dfs_help_option`
line.

## Test

Smoke test: the existing bag-stats tests pass, and one runs a two-pass
`-n` search with `--cache-size 0` and with the default, then checks the
output is the same.

## Not in this plan

`PairCountTable`'s prepared table hits the same 2^28 bound on this bag and
falls back to its bounded on-demand index. That stays as it is.
