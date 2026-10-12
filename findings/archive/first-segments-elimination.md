# Folding `first-segments` into `top-segments`

## Summary

There is no fundamental capability in `first-segments` that requires a
separate implementation. Its distinctive behavior can be expressed as an
encounter-order mode of `top-segments`.

The implementation should not, however, model that mode as merely stopping
when the total segment count reaches `-n N`. `first-segments` stops after
finding `N` distinct eligible output values. Duplicates, ignored segments,
unselected segment types, and rejected rows do not advance that limit, while
a projected multi-word segment can contribute more than one output word.

There are three independent concepts:

| Concept | Current `top-segments` | Current `first-segments` |
|---|---|---|
| Input horizon | All result rows | Until enough candidates are found |
| Candidate ordering | Count descending, then lexical | First encounter |
| Output limit | `-n N`; zero means unlimited | `-n N`; zero means none |

A unified interface should represent these concepts separately.

## Current behavior

### `top-segments`

`top-segments` reads the complete supplied result set. After row rejection and
per-segment ignores, it counts eligible occurrences and then ranks candidates.
In ordinary mode the ordering is:

1. occurrence count, descending;
2. segment text, ascending.

`-n N` is applied only while printing. It does not limit input consumption.
`-n 0` means unlimited output.

### `first-segments`

`first-segments` processes rows and segments in encounter order. It keeps a
set of output values already found and stops when that set has supplied `N`
results. More precisely:

- every complete row is parsed and checked for row-level rejection first;
- ignored or unselected segments do not count toward the limit;
- duplicate output values do not count toward the limit;
- segment projection can produce multiple distinct output words;
- collection can stop partway through the selected values of a valid row;
- if fewer than `N` eligible values exist, input is read to EOF.

The result is normally printed in encounter order. With `--by-length`, only
the collected prefix is stably sorted by descending non-space length, so
encounter order remains the tie-breaker.

`first-segments` never prints occurrence counts. It also writes this summary
to standard error:

```text
found segment N on line L
```

## `-n 0` does not currently converge

The two commands currently give `-n 0` opposite meanings.

In `top-segments`, zero bypasses the output truncation and prints every
eligible candidate. In `first-segments`, the read-loop condition is false
before the first row is read, so it produces no candidates and reports line
zero. It does not validate malformed result rows beyond that point.

Consequently, the current commands do not return the same results with
`-n 0`.

If a unified encounter-order mode adopted the `top-segments` convention that
zero means unlimited, it would read all input and print every distinct
eligible candidate in encounter order. That would be a deliberate change to
the current `first-segments -n 0` contract.

Even with both commands made unlimited, they would normally have the same
candidate set rather than the same literal output: ordinary `top-segments`
orders by frequency and normally includes counts, whereas `first-segments`
orders by encounter and omits counts.

## Proposed command-line model

An encounter-order flag can express the current `first-segments` selection:

```text
top-segments --first -n 1000 RESULTS
```

Here `--first` selects encounter-order ranking and makes `-n` the number of
distinct eligible output candidates to collect. The input is consumed only
until that limit is satisfied. With `-n 0`, collection is unlimited and input
is read to EOF.

`--first` is clearer than `--first-n`: the latter sounds as though it takes a
number of its own, while `-n` already supplies the candidate limit.

The separately useful operation of ranking candidates from only an initial
result prefix should have its own option, for example:

```text
top-segments --first-results 5000 -n 1000 RESULTS
```

This would consume at most 5,000 result rows, count candidates in that prefix,
and print its top 1,000 candidates. `--first-results` or `--max-results` is
preferable to `--first N`, because `--first N` is easily confused with the
encounter-order mode and with `-n N`.

The prefix contract should specify which rows advance its counter. A simple,
predictable definition is the first `N` syntactically valid input rows before
row-level filtering, treating multiple input files as concatenated. Rejected
rows would therefore still consume part of the input horizon. If the intended
quantity is surviving rows instead, that is a different and
filter-dependent option.

## Implementation shape

The existing segment-counting engine can support both behaviors with a modest
generalization:

1. Add a ranking mode for count order versus first-encounter order.
2. Record a monotonically increasing first-seen ordinal when a projected
   candidate is inserted.
3. Apply selection and projection while reading when candidate-driven early
   termination is active. Ordinary counting currently defers some projection
   until printing.
4. Have the reader distinguish successful EOF, candidate-limit reached, and
   error. Reaching the limit must also stop the outer multiple-file loop.
5. In encounter mode, rank by first-seen ordinal. With `--by-length`, rank by
   length and then first-seen ordinal.
6. Suppress counts in encounter mode. Counts accumulated only until the Nth
   distinct candidate appears would describe an arbitrary partial prefix and
   would be misleading.

The stopping test must use the number of distinct projected candidates, not
the sum of occurrence counts. Row rejection must still examine the complete
current row before any of its candidates are admitted.

Some option relationships should be explicit:

- `--first` and `--elim` should be mutually exclusive. Candidate-driven early
  termination does not provide the complete row universe required by
  elimination statistics.
- A fixed `--first-results N` horizon can support `--elim`, because the prefix
  itself defines a complete, bounded row universe.
- `--counts` should either be rejected with `--first` or documented as a new
  partial-prefix behavior; rejecting it is the cleaner match for
  `first-segments`.
- Existing `first-segments --by-length` semantics require collecting the
  first `N` candidates and only then sorting that collected set by length.

The current `--pair-words --unique` distinction has no observable effect on
`first-segments`: output words are already deduplicated, and revisiting an
identical segment cannot introduce a word not introduced by its first
occurrence. It remains meaningful for frequency counting in `top-segments`.

## Exact compatibility

The unified mode can reproduce `first-segments` candidate output, including
pair formatting and encounter-order ties. Literal process compatibility would
also require:

- the `found segment N on line L` standard-error summary;
- the `first-segments:` diagnostic prefix;
- the existing one-input restriction;
- the current `-n 0` means-none behavior.

Those details need not become part of `top-segments --first`. They can remain
in a compatibility front end if existing scripts depend on them.

## Whether to retain `first-segments`

There is little value in retaining a separate implementation once the shared
engine supports encounter ordering and candidate-driven termination. There is
still value in temporarily retaining the command name:

- `select_segment.py` currently chooses between the two executables;
- repository documentation exposes `--top` versus `--first`;
- external scripts may invoke `first-segments` directly;
- `first-segments` is a clearer discoverable name than
  `top-segments --first`.

A low-risk transition is therefore:

1. add the shared encounter-order mode to `top-segments`;
2. reduce `first-segments` to a thin compatibility front end selecting that
   mode;
3. update repository callers and documentation;
4. remove the separate executable later if preserving the command name no
   longer has practical value.

The durable distinction is a ranking and input-horizon policy, not a separate
program architecture.
