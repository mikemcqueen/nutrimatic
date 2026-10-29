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

// The most pb steps the --leftovers walk takes.
inline constexpr int GPU_MAX_PAIR_STEPS = 16;

// What the --leftovers walk takes: the bag; its RemainderIndex number; the
// pair bags, in PAIRS order, each one's letter counts, GPU_LETTERS_BYTES
// apiece, number, PAIRS and letters; the pb steps; and the --kernel.
struct GpuLeftoverInputs {
    uint8_t bag[GPU_LETTERS_BYTES];
    uint64_t bag_number;
    std::vector<uint8_t> pair_letters;
    std::vector<uint64_t> numbers;
    std::vector<uint64_t> pairs;
    std::vector<uint8_t> lengths;
    int steps;
    int kernel;
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

// Walks every combination of one pair bag per step, in increasing order,
// that fits the bag, on the GPU, and writes what it counts to `out`. Calls
// progress(units) about every 100 ms while the walk runs, `units` the first
// picks finished so far. A CUDA call failing prints `program` and CUDA's
// error on stderr and exits 1.
void run_leftovers_gpu(std::string_view program,
                       GpuLeftoverInputs const& inputs,
                       std::function<void(size_t)> const& progress,
                       GpuLeftoverCounts* out);

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

// The pair-sum filter: the group pairs it was sized for, its device bytes
// and the seconds its two kernels took.
struct GpuFilterInfo {
    uint64_t pairs;
    size_t bytes;
    double seconds;
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
