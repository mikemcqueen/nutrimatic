#ifndef NUTRIMATIC_BAG_STATS_GPU_H
#define NUTRIMATIC_BAG_STATS_GPU_H

// The interface between bag-stats-cuda.cpp and bag-stats-gpu.cu. Plain C++
// over standard headers, so nvcc compiles no other project header.

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Letter counts take 32 bytes: the a-z counts, then zeros, as PairsCounts
// holds them.
inline constexpr size_t GPU_LETTERS_BYTES = 32;

// The most rests GpuRowCounter::count() takes at once.
inline constexpr size_t GPU_BATCH_ROWS = 65536;

// The most wb steps the kernel walks.
inline constexpr int GPU_MAX_WORDS = 8;

// The most letters a bag may have.
inline constexpr int GPU_MAX_LETTERS = 126;

// The HyperLogLog register bits, as LeftoverSketch has them.
inline constexpr int GPU_HLL_BITS = 14;

// The most pick steps the --leftovers and --unique walks take.
inline constexpr int GPU_MAX_PAIR_STEPS = 16;

// The most prefix letters the walks index in unary, and the most bits of a
// prefix cell's number.
inline constexpr int GPU_MAX_PREFIX_LETTERS = 20;

// What the --leftovers walk takes: the bag; its RemainderIndex number and
// how many numbers the index has (for run_pair_usage_gpu's memo); the pair
// bags, the PAIRS bags then the --pp bags, each list sorted by prefix
// cell, each one's letter counts, GPU_LETTERS_BYTES apiece, number (none
// for run_pair_usage_gpu without a memo), PAIRS, letters and run; the
// prefix letters a run codes in unary, each with its place in the run; the
// last prefix letter, or -1, and the bits of a cell's number its count
// takes below the run; for run_distinct_rests_gpu, each pair bag's place
// in its list and the first step's list's pair bags by place; for the
// PAIRS bags and then the --pp bags, the first
// pair bag of each prefix cell, then the list's end; how many are PAIRS
// bags; and the steps, the pb steps first.
struct GpuLeftoverInputs {
    uint8_t bag[GPU_LETTERS_BYTES];
    uint64_t bag_number = 0;
    uint64_t number_count = 0;
    std::vector<uint8_t> pair_letters;
    std::vector<uint64_t> numbers;
    std::vector<uint64_t> pairs;
    std::vector<uint8_t> lengths;
    std::vector<uint32_t> runs;
    std::vector<uint32_t> places;
    std::vector<int> units;
    std::vector<std::pair<int, int>> prefix;
    int last_letter = -1;
    int last_bits = 0;
    std::vector<int> offsets;
    std::vector<int> prospect_offsets;
    int pair_bags = 0;
    int steps;
    int pair_steps;
};

// What the --leftovers walk counts: combinations by how many picks they
// make, with the last entry left 0; and by how many letters they leave,
// their combinations, PAIRS rows and HyperLogLog registers, 2^GPU_HLL_BITS
// a length.
struct GpuLeftoverCounts {
    std::vector<uint64_t> by_picks;
    std::vector<uint64_t> combinations;
    std::vector<uint64_t> rows;
    std::vector<uint32_t> registers;
};

// What a GPU pick walk reports to while it runs: about every 100 ms it asks
// due(), and when true, calls report(done, units, share), `done` of its
// `units` first picks finished so far and `share` its estimate of the share
// of its work finished, from the candidates finished units scanned; exact
// with 2 steps, negative when it has none.
struct GpuWalkProgress {
    std::function<bool()> due;
    std::function<void(size_t, size_t, double)> report;
};

// Walks every combination of one pair bag per step, in increasing order,
// that fits the bag, on the GPU, and writes what it counts to `out`,
// reporting to `progress`. A CUDA call failing prints `program` and CUDA's
// error on stderr and exits 1.
void run_leftovers_gpu(std::string_view program,
                       GpuLeftoverInputs const& inputs,
                       GpuWalkProgress const& progress,
                       GpuLeftoverCounts* out);

// Walks the combinations run_leftovers_gpu walks and writes to `out`, one
// per pair bag in `inputs` order, how many PAIRS rows of them each of the
// bag's rows is in, reporting to `progress`. A CUDA call failing prints
// `program` and CUDA's error on stderr and exits 1.
void run_pair_usage_gpu(std::string_view program,
                        GpuLeftoverInputs const& inputs,
                        GpuWalkProgress const& progress,
                        std::vector<uint64_t>* out);

// Walks the combinations of 2 or more steps whose first pick is one of
// `units` units from `first_unit`, in `inputs`' places' order, and writes to
// `out` the distinct numbers of what they leave, as many as a set sized
// for `rests` of them holds; true when that is all of them and no more
// than `rests`. A CUDA call failing prints `program` and CUDA's error on
// stderr and exits 1. Unless `rows` is null, also writes to it how many of
// the combinations leave each. With `out` null, only counts the distinct
// numbers. Unless `found` is null, writes how many there are to it.
bool run_distinct_rests_gpu(std::string_view program,
                            GpuLeftoverInputs const& inputs,
                            size_t first_unit, size_t units, size_t rests,
                            std::vector<uint64_t>* out,
                            std::vector<uint64_t>* rows = nullptr,
                            size_t* found = nullptr);

// The most rests a run_distinct_rests_gpu() set that fits the device's free
// memory holds, with `rows` one that counts each rest's rows.
size_t gpu_rest_set_capacity(bool rows);

// When `on`, run_pair_usage_gpu() reports on stderr how each table it sizes
// to the device's free memory compares with the table it would build with
// room for all of it.
void set_gpu_table_reports(bool on);

// `bytes` in KB, in MB from 1 MB, or in GB from 1 GB.
std::string gpu_bytes_text(double bytes);

// `bytes`, and `optimal` when the room for the optimal table was short.
std::string gpu_fit_text(double bytes, double optimal);

// Walks the rows of `inputs` as run_distinct_rests_gpu() does, those of
// `units` units from `first_unit`, for the rows whose rest is in a table:
// `keys`, a power of two of slots, open addressed from mix64(number + 1),
// each a rest's number + 1 or 0, and `values`, each slot's value, whose low
// two bits are 1 for a rest whose rows are all wanted and 2 for one whose
// rows are wanted only in units up to `last_unit`; with `keys` empty every
// row is of the second kind. With `rows` null, counts each unit's rows of
// the second kind into `unit_ties`, an entry for every unit of the list.
// Else writes the wanted rows to `rows`, each its picks' places in their
// lists and then its rest's value, in no order; false when there are more
// than `capacity`.
bool run_top_rows_gpu(std::string_view program,
                      GpuLeftoverInputs const& inputs, size_t first_unit,
                      size_t units, std::vector<uint64_t> const& keys,
                      std::vector<uint32_t> const& values, uint32_t last_unit,
                      size_t capacity, std::vector<uint32_t>* rows,
                      std::vector<uint64_t>* unit_ties);

// What the device holds for every row, as flat arrays.
struct GpuInputs {
    // The bag's letter counts.
    uint8_t bag[GPU_LETTERS_BYTES];
    // The inner dictionary groups, shortest first: each one's letter
    // counts, GPU_LETTERS_BYTES apiece, its length and its size.
    std::vector<uint8_t> group_letters;
    std::vector<uint8_t> group_lengths;
    std::vector<uint32_t> group_sizes;
    // The word bags: each one's letter counts and length.
    std::vector<uint8_t> word_letters;
    std::vector<uint8_t> word_lengths;
    // The groups' shortest length.
    int min_group_length;
    // The wb steps.
    int words;
    // A word bag combination counts in with_min_pairs from this many exact
    // pairs.
    uint64_t min_pairs;
};

// A row's counts, in BagStatsCounts order.
struct GpuCounts {
    uint64_t word_bags;
    uint64_t with_pairs;
    uint64_t total_pairs;
    uint64_t max_pairs;
    uint64_t solo;
    uint64_t with_min_pairs;
};

// The pair-sum filter: the group pairs it was sized for, the letters in the
// longest of their sums, its device bytes, the bytes it wanted, and the
// seconds its two kernels took.
struct GpuFilterInfo {
    uint64_t pairs;
    int longest_sum;
    size_t bytes;
    size_t optimal_bytes;
    double seconds;
    // With the exact sum table in the filter's place: its distinct sums.
    uint64_t distinct_sums;
    bool exact_table;
};

// The leftovers checked against the pair-sum filter and those it let
// through to a scan.
struct GpuFilterCounts {
    uint64_t leftovers;
    uint64_t scanned;
};

// Counts rows' remainders on the GPU over `streams` streams. A CUDA call
// failing prints the program name and CUDA's error on stderr and exits 1.
class GpuRowCounter {
  public:
    GpuRowCounter(std::string_view program, GpuInputs const& inputs,
                  size_t streams);
    ~GpuRowCounter();

    // The device's name.
    std::string const& device() const;

    // The bag's key bits: 64 or 128.
    int key_bits() const;

    // The device memory taken.
    size_t bytes() const;

    // The streams it counts on.
    size_t streams() const;

    // The pair-sum filter's size and build time.
    GpuFilterInfo filter_info() const;

    // The pair-sum filter's counts over every count() so far.
    GpuFilterCounts filter_counts() const;

    // Writes the counts of `rows` rests, at most GPU_BATCH_ROWS, each
    // GPU_LETTERS_BYTES of letter counts within the bag, to `out`. Any
    // thread may call it; it waits while every stream is busy.
    void count(uint8_t const* rests, size_t rows, GpuCounts* out) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif
