#ifndef NUTRIMATIC_BAG_STATS_CACHE_H
#define NUTRIMATIC_BAG_STATS_CACHE_H

// The remainder counts cache bag-stats and bag-stats-cuda share.

#include <stdint.h>
#include <stdio.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "bag-stats-impl.h"
#include "mix-bits.h"
#include "pairs-impl.h"

// A full mixed-radix index makes a remainder's number cheap to compute.
// Unlike SubBagIndex it does not restrict the length of the sub-bag.
// total() is 0 when `bag` has more than `bound` sub-bags.
class RemainderIndex {
  public:
    RemainderIndex(PairsCounts const& bag, size_t bound);

    size_t total() const { return total_; }
    size_t number(PairsCounts const& bag) const {
        size_t out = 0;
        for (auto const& [letter, stride] : digits_)
            out += bag.values[letter] * stride;
        return out;
    }

  private:
    size_t total_ = 1;
    std::vector<std::pair<size_t, size_t>> digits_;
};

// Lookups of a cache and how many of them hit.
struct CacheCounts {
    uint64_t hits = 0;
    uint64_t lookups = 0;
};

// CacheCounts summed across workers.
struct SharedCacheCounts {
    std::atomic<uint64_t> hits{0};
    std::atomic<uint64_t> lookups{0};

    void add(CacheCounts const& counts) {
        hits.fetch_add(counts.hits, std::memory_order_relaxed);
        lookups.fetch_add(counts.lookups, std::memory_order_relaxed);
    }

    // ", NAME hits N/M (P%)".
    std::string text(char const* name) const;
};

// The bytes one rest cache entry takes.
inline constexpr size_t REST_CACHE_ENTRY_BYTES =
    sizeof(BagStatsCounts) + 2 * (sizeof(uint64_t) + sizeof(uint32_t));

// The entries a rest cache of `megabytes` MB holds.
inline size_t rest_cache_entries(size_t megabytes) {
    return megabytes * 1024 * 1024 / REST_CACHE_ENTRY_BYTES;
}

// Statistics depend only on the remainder, not on which pair picks made
// it. Keep immutable records shared across workers and ranking passes, in
// at most `megabytes` MB: open addressing over remainder numbers + 1, 0
// while empty, each with its row index + 1, 0 until the row is written.
// With `report`, says on stderr, prefixed by `program`, when it fills.
class RestTable {
  public:
    RestTable(char const* program, PairsCounts const& bag, bool enabled,
              size_t megabytes, bool report);

    bool enabled() const { return keys_ != nullptr; }
    size_t capacity() const { return capacity_; }
    size_t remainders() const { return index_.total(); }

    bool get(PairsCounts const& rest, BagStatsCounts* out) const {
        if (!keys_) return false;
        uint64_t const key = index_.number(rest) + 1;
        for (size_t i = home(key);; i = i + 1 == slots_ ? 0 : i + 1) {
            uint64_t const found = std::atomic_ref<uint64_t>(keys_[i])
                .load(std::memory_order_relaxed);
            if (found == 0) return false;
            if (found != key) continue;
            uint32_t const row = std::atomic_ref<uint32_t>(rows_index_[i])
                .load(std::memory_order_acquire);
            if (row == 0) return false;
            *out = rows_[row - 1];
            return true;
        }
    }

    void put(PairsCounts const& rest, BagStatsCounts const& counts) const {
        if (!keys_ || next_.load(std::memory_order_relaxed) >= capacity_)
            return;
        uint64_t const key = index_.number(rest) + 1;
        for (size_t i = home(key);; i = i + 1 == slots_ ? 0 : i + 1) {
            uint64_t found = 0;
            if (!std::atomic_ref<uint64_t>(keys_[i]).compare_exchange_strong(
                    found, key, std::memory_order_relaxed)) {
                if (found == key) return;
                continue;
            }
            size_t const row = next_.fetch_add(1, std::memory_order_relaxed);
            if (row >= capacity_) return;
            if (row + 1 == capacity_ && report_)
                fprintf(stderr, "%s: cache full at %zu entries; later "
                        "remainders go uncached\n", program_, capacity_);
            rows_[row] = counts;
            std::atomic_ref<uint32_t>(rows_index_[i])
                .store(uint32_t(row + 1), std::memory_order_release);
            return;
        }
    }

  private:
    struct Free {
        template <typename T> void operator()(T* p) const { std::free(p); }
    };

    size_t home(uint64_t key) const {
        return size_t(__uint128_t(mix_bits(key)) * slots_ >> 64);
    }

    char const* program_;
    RemainderIndex index_;
    bool const report_;
    size_t capacity_ = 0;
    size_t slots_ = 0;
    std::unique_ptr<uint64_t[], Free> keys_;
    std::unique_ptr<uint32_t[], Free> rows_index_;
    std::unique_ptr<BagStatsCounts[], Free> rows_;
    mutable std::atomic<size_t> next_{0};
};

// Prints "PROGRAM: cache holds N entries (M MB) of R remainders" when
// `table` is enabled.
void report_rest_table(char const* program, RestTable const& table);

#endif
