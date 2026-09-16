# Bundle the pair-filter outputs behind `PairFilterSupport`

## Summary

Replace the five separate out-parameters of `load_pair_filters` with one
`PairFilters` struct, and replace the positional support bools of
`parse_pair_filter_option` with one `PairFilterSupport` struct that each tool
states once. Both functions then take the same per-tool value, so a tool
declares how it uses the subsystem in a single place instead of spreading it
across unlabeled `true, false` arguments and NULL out-parameters.

No filtering behavior changes. Every tool selects the same rows before and
after.

## Interface

```cpp
// Which pair-filter options a tool accepts. Options a tool does not support
// are refused at parse time, so the loader never sees them.
struct PairFilterSupport {
  bool ignore = false;        // -i/--ignore
  bool allow = false;         // -a/--allow-pairs
  bool workflow_yes = false;  // -y/--yes
};

// Every filtering layer a segment tool applies. Empty members accept
// everything, so a tool consults only the layers it cares about.
struct PairFilters {
  DfsPairSet ignored;
  DfsPairSet rejected;
  DfsDictionary dictionary;
  std::optional<DfsPairSet> allowed;  // nullopt: no allowlist policy active
  PairFilterSources sources;          // empty without a workflow root
};

PairFilterOptionResult parse_pair_filter_option(
    int argc, char* const argv[], int* index, char const* program,
    PairFilterSupport support, PairFilterOptions* out);

bool load_pair_filters(
    PairFilterOptions const& options, char const* program,
    PairFilterSupport support, PairFilters* out);
```

## Loader behavior

- Take one `PairFilters*` and populate every member. Drop the `allowed` and
  `sources` NULL out-parameters; there is no longer a way to ask for a partial
  load.
- Keep `allowed` a `std::optional`. Its nullopt continues to mean "no
  allowlist policy active", which `is_allowed_segment` consumes directly, and
  is distinct from an active empty set that rejects every multi-word segment.
- Populate `allowed` unconditionally. A tool with `support.allow` false has its
  `-a/--allow-pairs` refused at parse time, so `options.allow_paths` is
  necessarily empty and `allowed` resolves to nullopt on its own.
- Populate `sources` unconditionally, so `load_workflow_pair_file` and
  `load_target_pair_file` always record their per-layer sets. A workflow root
  that contributes nothing leaves the sets empty, which every consumer already
  treats as matching nothing.
- Assert rather than branch where support gates an input: the loader may rely
  on `support` agreeing with `options`.

## Why `sources` is unconditional

`sources` holds the un-merged pieces of the `rejected` and `ignored` unions so
`top-segments` can attribute each dropped row to a layer; the union alone
cannot say which file an entry came from. Recording it costs a second copy of
the workflow pair strings, because `load_workflow_pair_file` parses into the
source set and then inserts those elements into the aggregate.

That copy is too small to be worth a knob. In the current workflow root the
files involved are `.wf/classified/no/no.pairs` at 61,287 bytes,
`.wf/classified/yes/yes.pairs` at 1,478 bytes, and a target `no.pairs` of 46
bytes, against the 3.8 MB `words.filtered` that every one of these tools loads
unconditionally.

Keep the union. Consulting the three sets in turn would avoid the copy but
turns one hash lookup into three on the hot path, for every segment of every
result line.

## Tool changes

Each tool declares its support once and passes it to both functions:

```cpp
// top-segments.cpp
static constexpr PairFilterSupport kSupport = {
    .ignore = true, .allow = true, .workflow_yes = true};

// filter-segments.cpp
static constexpr PairFilterSupport kSupport = {.allow = true};

// first-segments.cpp
static constexpr PairFilterSupport kSupport = {
    .ignore = true, .workflow_yes = true};
```

- Replace each tool's four local filter variables with one `PairFilters`, and
  pass it by const reference to that tool's stream function. `count_stream`,
  `filter_stream`, and `first-segments`' stream function each take
  `PairFilters const&` in place of their present per-layer parameters.
- Leave the per-layer predicates (`is_rejected_segment`, `is_allowed_segment`,
  `all_words_in_dict`) as free functions. `top-segments` needs them separately
  to attribute rows to counters.
- `filter-segments` presently declares an `ignored` set it never reads, which
  its `support.ignore` of false now states directly.
- `rerank-anagrams` is unaffected. It loads its own rejections with
  `load_pair_file` and does not call `load_pair_filters`.

## Validation

Set up the repository and Meson environment, then compile the three affected
tools and run their existing focused tests:

```bash
source ./setup.sh
source .env/bin/activate
source build/dep-info/conanbuild.sh
meson compile -C build top-segments first-segments filter-segments
meson test -C build top-segments first-segments filter-segments \
  --print-errorlogs
git diff --check
```

## Not in scope

The precedence order that `top-segments` applies and `filter-segments`
collapses to a boolean stays duplicated between the two tools. Consolidating
it behind a single "which layer rejected this" query is a separate change.
