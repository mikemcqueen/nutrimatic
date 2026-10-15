#ifndef NUTRIMATIC_BAG_STATS_GPU_H
#define NUTRIMATIC_BAG_STATS_GPU_H

// The interface between bag-stats-cuda.cpp and bag-stats-gpu.cu. Plain C++
// over standard headers, so nvcc compiles no other project header.

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>
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
    int min_word_bag_length;
    // A MIN_WB word bag combination counts in min_with_pairs from this many
    // exact pairs.
    uint64_t min_word_bag_pairs;
};

// A row's counts, in BagStatsCounts order.
struct GpuCounts {
    uint64_t word_bags;
    uint64_t with_pairs;
    uint64_t total_pairs;
    uint64_t max_pairs;
    uint64_t solo;
    uint64_t min_word_bags;
    uint64_t min_with_pairs;
};

// Counts rows' remainders on the GPU over two streams. A CUDA call failing
// prints the program name and CUDA's error on stderr and exits 1.
class GpuRowCounter {
  public:
    GpuRowCounter(char const* program, GpuInputs const& inputs);
    ~GpuRowCounter();

    // The device's name.
    std::string const& device() const;

    // The bag's key bits: 64 or 128.
    int key_bits() const;

    // The device memory taken.
    size_t bytes() const;

    // Writes the counts of `rows` rests, at most GPU_BATCH_ROWS, each
    // GPU_LETTERS_BYTES of letter counts within the bag, to `out`. Any
    // thread may call it; it waits while both streams are busy.
    void count(uint8_t const* rests, size_t rows, GpuCounts* out) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif
