#include "bag-stats-cache.h"

#include <algorithm>
#include <cstdlib>
#include <string_view>

#include "log.h"

RemainderIndex::RemainderIndex(PairsCounts const& bag, size_t bound) {
    for (size_t i = 0; i < 26; ++i) {
        if (bag.values[i] == 0) continue;
        if (total_ > bound / (bag.values[i] + 1)) {
            total_ = 0;
            break;
        }
        digits_.push_back({i, total_});
        total_ *= bag.values[i] + 1;
    }
}

std::string SharedCacheCounts::text(std::string_view name) const {
    uint64_t const h = hits.load(std::memory_order_relaxed);
    uint64_t const l = lookups.load(std::memory_order_relaxed);
    char out[96];
    snprintf(out, sizeof out, ", %.*s hits %llu/%llu (%.1f%%)",
             SV_ARG(name),
             static_cast<unsigned long long>(h),
             static_cast<unsigned long long>(l),
             l == 0 ? 0.0 : 100.0 * double(h) / double(l));
    return out;
}

RestTable::RestTable(std::string_view program, PairsCounts const& bag,
                     bool enabled, size_t megabytes, bool report)
    : program_(program), index_(bag, UINT64_MAX - 1), report_(report) {
    if (!enabled || index_.total() == 0) return;
    capacity_ = std::min({rest_cache_entries(megabytes), index_.total(),
                          size_t(UINT32_MAX)});
    if (capacity_ == 0) return;
    slots_ = 2 * capacity_;
    keys_.reset(static_cast<uint64_t*>(
        std::calloc(slots_, sizeof(uint64_t))));
    rows_index_.reset(static_cast<uint32_t*>(
        std::calloc(slots_, sizeof(uint32_t))));
    rows_.reset(static_cast<CachedCounts*>(
        std::calloc(capacity_, sizeof(CachedCounts))));
    if (!keys_ || !rows_index_ || !rows_) {
        keys_.reset();
        rows_index_.reset();
        rows_.reset();
    }
}

CachedCounts RestTable::narrow(BagStatsCounts const& counts) const {
    uint64_t const wide[] = {counts.word_bags, counts.with_pairs,
                             counts.total_pairs, counts.max_pairs,
                             counts.solo, counts.with_min_pairs};
    for (uint64_t const count : wide) {
        if (count <= UINT32_MAX) continue;
        report(program_, "a cached count, %llu, doesn't fit 32 bits",
               static_cast<unsigned long long>(count));
        std::abort();
    }
    return {uint32_t(counts.word_bags), uint32_t(counts.with_pairs),
            uint32_t(counts.total_pairs), uint32_t(counts.max_pairs),
            uint32_t(counts.solo), uint32_t(counts.with_min_pairs)};
}

void report_rest_table(std::string_view program, RestTable const& table) {
    if (!table.enabled()) return;
    report(program, "cache holds %zu entries (%.1f MB) of %zu remainders",
           table.capacity(),
           double(table.capacity() * REST_CACHE_ENTRY_BYTES) /
                (1024 * 1024),
           table.remainders());
}
