# Plan: existing-index coherence experiment

> **Status (2026-08-03):** The shrunken-PMI, `--mu`, missing-count, and
> input-local calibration design in this plan was implemented and rejected by
> inspection of real results. It is retained as experiment history. The
> replacement implementation uses signed square-root likelihood-ratio
> evidence without those options.
>
> **Update (2026-08-04):** That replacement is also not an ordering term. It
> ranks `they were` above every content phrase measured. See
> [association-is-not-interestingness.md](../findings/association-is-not-interestingness.md).

## Goal

Build a standalone `measure-coherence` program that uses only a completed
`dfs-anagrams` output file and the existing Nutrimatic index to test whether
smoothed adjacent-word association improves the ranking of semantically
plausible answers.

The program will:

1. read a regular input file, never a pipe or standard input;
2. intern only the rendered words present in that file;
3. look up only the unique words and ordered pairs required by those answers;
4. calculate the approximate coherence quantities from
   `findings/q_coherence.md`;
5. optionally calibrate pair association against the candidate file;
6. score the printed order or find the exact best word order; and
7. report or rerank the input candidates.

The initial implementation has one path through each feature. It has no
streaming mode, beam search, approximate ordering, spill-to-disk mode,
index-wide preprocessing, or alternate cache implementation.

No corpus reprocessing, index rebuild, index-format change, DFS pruning
change, combined lexical/coherence score, or `dfs-anagrams` integration belongs
in this experiment.

## File-only workflow

Produce the candidate pool once:

```bash
./build/dfs-anagrams "$IDX" "$letters" -n 50000 > candidates.tsv
```

Run one coherence configuration against that file:

```bash
./build/measure-coherence "$IDX" candidates.tsv \
  --format dfs --mu 1000 --order best \
  --sort coherence --top 10000
```

Repeat `measure-coherence` on the same file to sweep `mu`, missing-pair count,
or ordering policy. One invocation evaluates one configuration. This keeps the
CLI, output schema, memory ownership, and calibration state simple while still
avoiding repeated DFS searches.

The tool must reject `-` as the input path. Standard input support is not part
of the initial implementation.

This file boundary also defines the process lifecycle: `dfs-anagrams` has
exited before `measure-coherence` starts. Their private heaps therefore do not
overlap. The OS may retain useful index pages in the shared file cache, but the
coherence process owns no DFS objects and performs no concurrent work with the
search.

## Fixed resource contract

The initial implementation has these fixed limits:

```cpp
inline constexpr size_t COHERENCE_MEMORY_LIMIT =
    size_t(3072) * 1024 * 1024;
inline constexpr uint64_t COHERENCE_ORDER_RELAXATION_LIMIT =
    UINT64_C(250000000);
inline constexpr size_t COHERENCE_MAX_WORDS = 63;
inline constexpr size_t COHERENCE_MAX_LINE_BYTES = 4096;
```

`COHERENCE_MEMORY_LIMIT` is a 3072 MiB ceiling for memory explicitly owned by
the coherence algorithm: input records, word interning, pair-key collection,
pair observations, calibration work, answer measurements, exact-order DP, and
output ordering. The read-only index mapping is excluded because it is
file-backed, lazy, and shared through the OS page cache rather than allocated
by the algorithm.

Every large allocation must be preceded by checked size arithmetic and a
budget charge. If a phase would take the charged total over 3072 MiB, print the
phase, requested bytes, charged bytes, and limit, then exit with status 1.
Allocation failure also exits with status 1. Do not catch a limit failure and
switch algorithms.

Use one explicit accountant:

```cpp
class MemoryBudget {
 public:
  explicit MemoryBudget(size_t limit);
  bool acquire(size_t bytes, char const* owner);
  void release(size_t bytes);
  size_t current() const;
  size_t peak() const;
};
```

Each persistent owner records its own charged byte count. Charge before
`reserve()` or allocation, release the old charge immediately after storage is
destroyed, and abort on underflow or double release in debug builds. All size
calculations use checked addition and multiplication before `acquire()`.

The budget is intentionally conservative for the one node-based container
retained by this plan, the word-interning `unordered_map`. Charge:

```text
bucket_count * sizeof(void*)
+ 96 bytes per word-map element
+ owned word text bytes including NUL terminators
```

All other large structures are vectors or flat arrays and are charged by
`capacity() * sizeof(value_type)`. The initial implementation reports only the
conservative charged peak; it does not add platform-specific RSS/PSS readers.

For exact best ordering, preflight the conservative number of DP relaxations:

```text
R(0) = R(1) = 0
R(m) = m * (m - 1) * 2^(m - 2), for m >= 2
total_R = sum R(word_count(answer))
```

Use checked integer arithmetic. If any answer has more than 63 rendered words,
or `total_R` exceeds 250,000,000, exit with status 1 before index lookup. Do not
fall back to printed order or approximate search.

## Important statistical interpretation

This is a censored approximation, not an exact corpus bigram model:

- an absent `x y ` trie path can mean either zero observations or a pair lost
  to an index merge cutoff;
- aggregate `x ` count includes occurrences of `x` without a following word;
- the indexer's 40-character history window omits sufficiently long pairs;
- punctuation, title weighting, and corpus normalization are inherited from
  the existing index; and
- the candidate-local calibration is specific to the input file.

The experiment is meaningful only if results are stable across reasonable
values of `mu` and the assumed count for absent pairs. Those sensitivity runs
reuse the same candidate file but are separate program invocations.

## Statistic calculated from the existing index

For rendered adjacent words `x` and `y`, obtain:

```text
cx  = aggregate trie count at "x "
cy  = aggregate trie count at "y "
cxy = aggregate trie count at "x y ", or the configured missing count
N   = IndexReader::count()
```

Approximate the background probability and association as:

$$
\widetilde p_y = \frac{c_y}{N}
$$

$$
\widetilde a(x,y)
=
\log\left(
\frac{c_{xy}+\mu\widetilde p_y}
     {(c_x+\mu)\widetilde p_y}
\right).
$$

Retain the independence expectation as a diagnostic:

$$
E_{xy}=c_x\widetilde p_y.
$$

Without pair calibration, an answer's edge value is
`association = a(x,y)`. With pair calibration, its edge value is the
frequency-conditioned percentile `q_pair(x,y)` defined below.

For the selected word order, report both association summaries:

$$
A_{\mathrm{mean}}
=
\frac{1}{m-1}\sum_{i=1}^{m-1}
\widetilde a(w_i,w_{i+1}),
$$

$$
A_{\min}=\min_i\widetilde a(w_i,w_{i+1}).
$$

When calibration is enabled, also report:

$$
Q_{\mathrm{coherence}}
=
\frac{1}{m-1}\sum_{i=1}^{m-1}
q_{\mathrm{pair}}(w_i,w_{i+1}),
$$

$$
Q_{\min}=\min_i q_{\mathrm{pair}}(w_i,w_{i+1}).
$$

Coherence is undefined for a one-word answer. Do not manufacture a neutral
value.

## Stage 1: allocation-free aggregate index cursors

### Public API

Add to `source/index.h`:

```cpp
#include <string_view>

class IndexReader {
 public:
  struct EntryPosition {
    Node continuation;
    int64_t aggregate_count;
  };

  // Looks up an entry including its implicit trailing space. The returned
  // position is immediately after that space.
  bool aggregate_entry_position(
      std::string_view entry, EntryPosition* result) const;

  // Resolves an entry after a previously resolved entry's trailing space.
  bool continuation_entry_position(
      EntryPosition const& prefix,
      std::string_view entry,
      EntryPosition* result) const;

  bool aggregate_entry_count(
      std::string_view entry, int64_t* count) const;

 private:
  bool find_child(Node parent, int64_t parent_count,
                  unsigned char ch, Choice* result) const;
  bool traverse_entry(EntryPosition start,
                      std::string_view entry,
                      EntryPosition* result) const;
};
```

`find_child()` is a scalar counterpart to `children()`:

1. decode the node header exactly as `children()` does;
2. scan child character bytes until `ch` is found;
3. decode only the selected child's count and offset;
4. return immediately after finding the unique matching child; and
5. return false without allocating when it is absent.

Do not implement it with a one-element `std::vector<Choice>`. Aggregate word
and pair preprocessing may perform millions of successful traversals, so one
heap allocation per lookup is not acceptable.

`traverse_entry()` starts from the supplied `(continuation, aggregate_count)`,
advances through every byte in `entry`, then advances through one implicit
space. The root form starts from `{root(), count()}`. A continuation node of
`-1` has no child and returns false.

Both fields of `EntryPosition` are required. Some compact node encodings
inherit their count from the parent, so a node offset alone is incomplete.

Refactor `exact_entry_count()` to call `aggregate_entry_position()` and then
subtract descendants at the returned trailing-space position. Preserve its
existing signature and residual semantics.

Do not sum longer phrase entries. The selected trailing-space choice's count
is already the aggregate frequency.

### Focused index test

Add `test-index-counts` with a tiny `IndexWriter` fixture containing:

```text
red tea x 3
red tea cup x 2
red tree x 1
```

Verify:

```text
aggregate("red")      == 6
aggregate("red tea")  == 5
exact("red tea")      == 3
aggregate("red tree") == 1
aggregate("tea red")  is absent
```

Also resolve `red`, resolve `tea` relative to its position, and verify the
result equals resolving `red tea` from the root.

## Stage 2: file parser and compact candidate storage

### Command line

Add `source/measure-coherence.cpp` and register it as a non-installed
measurement executable linked with `optparse_lib`, `index_lib`, and the new
coherence-score library.

Use these Meson targets in `source/meson.build`:

```meson
coherence_score_lib = static_library(
  'coherence-score', 'coherence-score.cpp',
  install: false)

measure_coherence = executable(
  'measure-coherence', 'measure-coherence.cpp',
  link_with: [optparse_lib, index_lib, coherence_score_lib],
  install: false)
```

The focused tests below are separate executables named `test-index-counts`,
`test-coherence-score`, and `test-measure-coherence`, registered as Meson tests
`index-counts`, `coherence-score`, and `measure-coherence` respectively.

The initial CLI is:

```text
measure-coherence INDEX INPUT --format dfs|text --mu MU
  --order printed|best
  [--missing-count N]
  [--calibrate]
  [--sort input|coherence]
  [--top N]
  [--pairs]
```

Rules:

- `INDEX`, `INPUT`, `--format`, `--mu`, and `--order` are required.
- `INPUT` must name a regular readable file and must not be `-`.
- `--missing-count` defaults to zero.
- `--order best` and `--sort coherence` each enable pair calibration
  automatically.
- `--sort` defaults to `input`.
- `--top` defaults to all input rows and must be positive when supplied.
- `--pairs` emits prefixed pair-detail TSV rows to standard error after the
  corresponding selected answer is written to standard output.
- One invocation accepts exactly one `mu` and one missing-count value.
- Strictly reject unknown or repeated singleton options.

`--format dfs` parses current `dfs-anagrams` stdout: the first token is a
positive finite displayed score and the rest is the rendered answer. Store the
displayed score and its natural logarithm. This is a rounded legacy baseline,
not an exact internal DFS score.

`--format text` treats the complete line as the answer and has no legacy
score.

For both formats, rendered text must be canonical:

- no blank lines;
- no leading or trailing spaces;
- exactly one ASCII space between words;
- no tabs, carriage returns inside the line, NULs, or other ASCII controls;
- at least one nonempty word; and
- at most 63 rendered words; and
- at most 4096 bytes before the line ending.

Accept a final CR before LF so CRLF files work, but remove it before canonical
checks. Use `fgets()` with a fixed 4098-byte buffer and reject a full buffer
that has neither newline nor EOF; do not let an unbounded `getline()` allocation
bypass the memory budget. Preserve all other word bytes exactly for index
lookup.

### Two parsing passes

Open `INPUT` once and use the same line parser in both passes. After pass 1,
clear the stream state and seek back to offset zero. At the end of pass 2,
verify its answer and word-occurrence totals still match pass 1; fail if they
differ.

Pass 1 performs no persistent interning. It validates every row and computes,
with checked arithmetic:

```cpp
struct InputSummary {
  uint64_t answer_count;
  uint64_t word_occurrence_count;
  uint64_t required_pair_occurrence_count;
  uint64_t order_relaxation_count;
  size_t maximum_word_count;
  size_t total_word_text_bytes;
};
```

For `--order printed` without calibration, required pairs are the `m - 1`
printed adjacencies. For `--order best`, or whenever calibration is enabled,
required pairs are all `m * (m - 1)` directed occurrence pairs with distinct
positions. Equal spellings at distinct positions remain separate calibration
opportunities.

Pass 1 enforces the exact-order work limit and charges known candidate,
word-ID, pair-occurrence, and worst-case word-interning storage before pass 2.

Pass 2 interns words and stores candidates.

### Query-local word IDs

Use:

```cpp
using WordId = uint32_t;

struct WordInfo {
  std::string const* text;
  uint64_t first_input_rank;
  int64_t aggregate_count;
  IndexReader::EntryPosition position;
};

struct Candidate {
  uint64_t input_rank;
  uint32_t word_begin;
  uint8_t word_count;
  bool has_legacy_score;
  double displayed_legacy_score;
  double legacy_log_score;
};

std::unordered_map<std::string, WordId> word_ids;
std::vector<WordInfo> words;
std::vector<Candidate> candidates;
std::vector<WordId> candidate_words;
```

The map owns each unique word exactly once. Assign IDs in first-occurrence
order and retain that row in `first_input_rank`. Before pass 2, call
`word_ids.reserve(summary.word_occurrence_count)` after charging the resulting
worst-case bucket array and element count. This deliberately over-reserves when
many words repeat, but avoids rehash peaks and keeps the initial implementation
predictable. After pass 2, populate `words[id].text` with pointers to map keys.
No entries are erased, so those pointers remain valid.

Do not store complete rendered answers. Reconstruct one by joining its interned
words with one ASCII space, identical to canonical input.

Reject more than `UINT32_MAX - 1` unique words, more than `UINT32_MAX` total
word occurrences, or any count that cannot fit the selected field widths.

## Stage 3: collect and resolve unique directed pairs

### Packed pair keys

Represent a directed pair as:

```cpp
using PairKey = uint64_t;

PairKey pair_key(WordId left, WordId right) {
  return (uint64_t(left) << 32) | uint64_t(right);
}
```

This key is query-local. It is not an index-wide word ID and is never persisted
outside the invocation.

Enumerate required pair occurrences after pass 2 into:

```cpp
std::vector<PairKey> pair_occurrences;
```

Reserve the exact count from pass 1 and sort numerically. The high 32-bit left
ID groups every pair with the same left word for continuation lookup locality.

Run-length encode the sorted vector into:

```cpp
struct PairRecord {
  PairKey key;
  uint64_t calibration_weight;
  int64_t observed_count;
  bool present;
  bool exceeds_history_window;
  double background_probability;
  double expected_count;
  double association;
  double percentile;
};

std::vector<PairRecord> pairs;
```

`calibration_weight` is the number of directed occurrence opportunities with
that key. It preserves the exact candidate-local empirical population without
storing one CDF sample per repetition.

Count unique runs first, charge and reserve exact `pairs` capacity, then build
the records. Release `pair_occurrences` immediately afterward and update the
budget before allocating calibration or DP scratch.

Find a pair during scoring with `std::lower_bound` over sorted `pairs`. Do not
add a second hash index in the initial implementation.

### Word and pair index resolution

Construct `IndexReader` only after parsing and pair-record construction.
Do not construct an `IndexWalker` and do not scan the full index. Startup work
must remain proportional to unique input words and unique required pairs, not
to the roughly 110 million stored word and multi-word entries.

Resolve each unique word exactly once with
`aggregate_entry_position(word)`. Store its count and position in `WordInfo`.
A missing word is a fatal input error naming the word and its first input row.

Resolve every pair in sorted order:

```cpp
WordInfo const& left = words[left_id(pair.key)];
WordInfo const& right = words[right_id(pair.key)];
IndexReader::EntryPosition position;
pair.present = reader.continuation_entry_position(
    left.position, *right.text, &position);
pair.observed_count = pair.present ? position.aggregate_count : 0;
pair.exceeds_history_window =
    left.text->size() + 1 + right.text->size() + 1 > 40;
```

Starting from `left.position` avoids retraversing the left word for every pair.
An absent pair is not an error. Do not retain its final node after reading the
count; no later stage traverses below it.

## Stage 4: pure coherence scoring

Add `source/coherence-score.{h,cpp}`. Keep it independent of input parsing,
interning, index traversal, DFS types, and cache ownership.

```cpp
struct CoherenceOptions {
  double mu;
  int64_t missing_pair_count;
};

struct PairObservation {
  int64_t left_count;
  int64_t right_count;
  int64_t observed_pair_count;
  bool pair_present;
};

struct PairScore {
  int64_t effective_pair_count;
  double background_probability;
  double expected_count;
  double association;
};

PairScore score_pair(PairObservation const& observation,
                     int64_t corpus_total,
                     CoherenceOptions const& options);
```

Use the stored pair count when present and `missing_pair_count` otherwise.
Reject invalid state before arithmetic:

- non-positive or non-finite `mu`;
- negative missing count;
- non-positive corpus total;
- non-positive standalone word counts;
- negative observed pair count; and
- a present pair with a non-positive observed count.

Verify all calculated doubles are finite, the background probability is
positive, and the smoothed conditional probability is positive.

Populate every pair's background probability, expected count, and association
once after index resolution. `mu` and missing count are fixed per invocation.

### Focused scoring test

Use direct synthetic observations. Verify:

1. above-expectation association is positive;
2. below-expectation association is negative;
3. increasing `mu` pulls low evidence toward zero;
4. present and missing pairs select the correct effective count;
5. invalid counts and non-finite options fail; and
6. diagnostics match the formulas.

## Stage 5: exact word ordering

For one candidate with `m` occurrences, build one row-major `m * m` edge
matrix. Diagonal entries are unused. Resolve off-diagonal pairs by packed key
and `lower_bound`.

The optimization edge is:

```text
association             without --calibrate
PairRecord::percentile  with --calibrate
```

Calibration changes the objective before ordering. Do not run separate raw and
calibrated best-order DPs.

For `--order printed`, allocate no DP state; select the input occurrence order.

For `--order best`, find the exact maximum-weight directed Hamiltonian path:

```cpp
struct OrderingResult {
  double objective_sum;
  uint8_t size;
  std::array<uint8_t, COHERENCE_MAX_WORDS> order;
};

std::vector<double> dp;     // dp[mask * m + last]
std::vector<uint8_t> parent;
```

Use a 64-bit mask and checked `m * 2^m` sizing. Initialize singleton states to
zero and all other scores to negative infinity. Relax every unused next
occurrence from every reachable `(mask,last)`.

Duplicate spellings are separate occurrences, but skip equivalent occurrence
permutations exactly: an occurrence may be chosen only when every earlier
occurrence with the same `WordId` has already been chosen. This changes neither
rendered output nor score.

Use `uint8_t` parents because `m <= 63`; `UINT8_MAX` is the unset sentinel.
Reuse DP, parent, edge, and reconstruction vectors across candidates. Charge
capacity based on the largest candidate.

Ties use occurrence indexes only: on an exactly equal relaxation, retain the
smaller predecessor occurrence index; across equal final endpoints, retain the
smaller last occurrence index. Masks, `last`, and `next` are all visited in
ascending numeric order. This is deterministic, constant-time, and independent
of hash iteration order. Lexicographic answer text remains the tie-break only
when sorting distinct final candidate rows.

After reconstruction, calculate association mean/minimum along the selected
order and percentile mean/minimum when calibrated.

If sizing exceeds the memory budget or pass 1 exceeds the work limit, exit
status 1. There is no alternate ordering path.

## Stage 6: exact weighted pair calibration

`--calibrate` builds a query-local frequency-conditioned CDF from all directed
opportunities represented by `calibration_weight`.

Assign each pair to:

```cpp
int raw_bucket(double expected_count) {
  int exponent;
  std::frexp(expected_count, &exponent);
  return std::clamp(exponent - 1, -64, 63);
}
```

This is `floor(log2(expected_count))`, clamped to 128 buckets.

Merge nonempty raw buckets deterministically from low to high expectation:

1. accumulate consecutive buckets until total calibration weight is at least
   1000, then close the final bucket;
2. continue with the next raw bucket; and
3. merge a final bucket below 1000 into its predecessor. If the entire
   population is below 1000, use one bucket.

Never split a raw bucket. Print each final bucket's exponent range,
unique-pair count, and opportunity weight.

For one final bucket, build:

```cpp
struct WeightedAssociation {
  double association;
  PairKey key;
  uint64_t weight;
  size_t pair_index;
};
```

Sort by association then key. For each run of exactly equal association, give
all members the upper empirical percentile:

```text
cumulative weight through the equal-value run / total bucket weight
```

Store it in the corresponding `PairRecord`. This exactly matches expanding
each pair `calibration_weight` times without expanded memory.

Process one final bucket at a time: scan `pairs`, append only records belonging
to that bucket, sort and assign percentiles, then clear and reuse the temporary
vector. Release it before exact ordering. Calibration failure is fatal; do not
substitute raw association.

## Stage 7: answer measurement and output

Store:

```cpp
struct MeasuredAnswer {
  size_t candidate_index;
  bool coherence_defined;
  uint32_t order_begin;
  uint8_t word_count;
  size_t present_pair_count;
  double association_mean;
  double association_minimum;
  double pair_percentile_mean;
  double pair_percentile_minimum;
  double shape_percentile;
};

std::vector<MeasuredAnswer> measured;
std::vector<uint8_t> selected_orders;
```

Append each order to one flat vector. For printed order append `0..m-1`.
One-word answers set `coherence_defined=false` and coherence doubles to quiet
NaN. They enter neither pair nor shape calibration.

When `--sort coherence` is requested, calculate an answer-level upper empirical
CDF separately for each exact word count. Its input is percentile mean with
calibration and association mean without calibration. Use one unit of weight
per answer and store the result as `shape_percentile`.

Use an array of 64 vectors containing `(value, candidate_index)`. Charge their
combined reserved capacity, sort each nonempty vector by value then candidate
index, assign upper-CDF ties, and release all vectors before sorting final
answers.

`--sort input` preserves input rank exactly.

`--sort coherence` orders defined answers by:

1. descending shape percentile;
2. descending percentile minimum when calibrated, otherwise association
   minimum;
3. descending legacy log score when both rows have it;
4. lexicographically ascending reconstructed answer; and
5. ascending input rank.

Undefined one-word answers follow defined answers in input order. Apply
`--top` after sorting. Reconstruct text only for final tie comparisons and
output; do not retain `best_text` strings.

Write a space-aligned answer table to standard output. Use one space between
columns, left-align ordinary header cells, right-align numeric values, and
size each numeric column to its widest displayed value or header, whichever
is wider. Span the association and pair-percentile column pairs with centered
ASCII group headers. Reduce the group header's padding and dashes to fit that
data-determined span rather than widening either numeric column; abbreviate
`association` to `assoc` only if the full label cannot fit. Left-align `mean`
and right-align `min` under each span:

```text
input legacy word  stored |association| |pair %| shape
rank  score  count pairs  mean      min mean min %     text
```

Show the pair-percentile columns only with `--calibrate`. Show the shape
percentile column only with `--sort coherence`.

Use `nan` for undefined or disabled fields. Format legacy scores in scientific
notation with three zero-padded digits after the decimal point so small scores
retain their magnitude. Format other finite floating-point values in fixed
notation with three zero-padded digits after the decimal point.

With `--pairs`, emit to standard error after its answer:

```text
pair	input_rank	boundary	left	right	left_count	right_count	pair_count	present	window_exceeded	expected	association	percentile
```

Use the configured missing count in displayed `pair_count` for an absent pair.
All ordinary diagnostics begin with `measure-coherence:` so pair TSV is
unambiguous.

## Error handling

Exit status 2 is for malformed command-line options. Exit status 1 covers
input, index, resource, allocation, and scoring failures.

Input errors name the path and one-based line. Missing-word errors name the
word and first input row. Resource errors name the phase and both limits.

Reject:

- `mu <= 0`, NaN, or infinity;
- a negative missing count;
- unknown or repeated singleton options;
- `-` or a non-regular input file;
- malformed DFS scores;
- noncanonical or empty answers;
- input lines above 4096 bytes;
- answers above 63 words;
- words absent from the standalone-word index;
- checked-arithmetic overflow;
- charged memory above 3072 MiB;
- conservative ordering work above 250,000,000 relaxations; and
- non-finite score state.

An absent pair is expected censored data and is not an error.

## Required diagnostics and timing

Print:

```text
measure-coherence: input: A answers, T word occurrences, U unique words
measure-coherence: pairs: O opportunities, P unique directed pairs
measure-coherence: ordering: R estimated relaxations, M maximum words
measure-coherence: memory: B charged peak bytes of 3221225472
measure-coherence: timing: parse1 S, parse2 S, pair-build S
measure-coherence: timing: word-lookup S, pair-lookup S, calibration S, scoring S
```

Use `steady_clock`. For accurate manual timing, follow `AGENTS.md` and check the
host process table for both `query-index` and `dfs-anagrams` before every timed
run. Smoke tests do not require a quiet timing window.

## Minimal smoke tests

### `test-index-counts`

1. Aggregate and exact residual counts differ correctly.
2. Continuation and root pair traversal return the same count.
3. Missing continuation lookup is allocation-free and returns false.

### `test-coherence-score`

1. Stored and missing observations select distinct effective counts.
2. Association sign matches independence expectation.
3. Smoothing pulls low evidence toward zero.
4. Invalid and non-finite inputs fail.
5. One-word reduction remains undefined.

### `measure-coherence` CLI fixture

Use a tiny synthetic index and fixed files. Verify:

1. both file formats parse;
2. stdin path `-` is rejected;
3. canonical-text failures identify the line;
4. repeated spellings receive one word ID;
5. pair run weights match repeated opportunities;
6. stored and absent pairs remain distinct;
7. weighted calibration equals an explicitly expanded tiny CDF;
8. exact best ordering selects a known directed ordering;
9. duplicate words produce no equivalent alternate ordering;
10. exact ordering ties follow the specified occurrence-index rule;
11. input sorting preserves order;
12. coherence sorting is deterministic;
13. one-word rows stay undefined and sort after defined rows;
14. memory failure is deterministic with a test-injected small limit; and
15. ordering-work failure is deterministic with a test-injected small limit.

Inject smaller limits through functions or constructors in tests. Do not add
production environment variables or hidden CLI flags. Do not use the 1.2 GB
production index in the default suite.

## Manual experiment

Build once, produce one candidate file per query, and retain it for sensitivity
runs.

For each query:

1. produce a pool materially larger than the display count;
2. score printed and exact best order in separate invocations;
3. sweep `mu` logarithmically against the same file;
4. repeat with missing counts zero and one;
5. compare raw association and calibrated percentile;
6. record stored-edge fraction, mean, and minimum;
7. blindly compare baseline and coherence rankings;
8. record rank stability; and
9. record timing and charged peak memory.

A useful result must repeatedly promote plausible sequences, demote word
salad, remain stable across missing-pair assumptions, avoid dependence on one
edge, add information beyond lexical score, and stay within fixed limits.

## Implementation stages and commit boundaries

Implement and review in this order:

1. index position, scalar traversal, aggregate API, and index-count test;
2. pure score formulas and score test;
3. file CLI, two-pass summary, budget, interning, and candidate storage;
4. packed pair collection, run-length records, word positions, and pair lookup;
5. printed-order measurement and stable output;
6. exact subset-DP ordering with hard resource failures; and
7. weighted pair calibration, shape calibration, sorting, and pair diagnostics.

Do not combine adjacent stages merely because later code is small. Each stage
must leave one coherent ownership boundary and pass focused smoke tests.

Before every commit, run `/review` as required by `AGENTS.md`. Keep future DFS
integration and ranking-model changes out of these commits.

## Build and validation

```bash
source ~/code/nutrimatic/.env/bin/activate
conan build .
source build/dep-info/conanbuild.sh
meson compile -C build
meson test -C build index-counts coherence-score measure-coherence \
  --print-errorlogs
git diff --check
```

Focused smoke tests are the required validation. Run the full suite only when
a shared index or build-system change makes it proportionate, and report
focused and full-suite results separately.
