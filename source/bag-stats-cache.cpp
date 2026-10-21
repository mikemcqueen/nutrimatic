#include "bag-stats-cache.h"

#include <algorithm>

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

std::string SharedCacheCounts::text(char const* name) const {
    uint64_t const h = hits.load(std::memory_order_relaxed);
    uint64_t const l = lookups.load(std::memory_order_relaxed);
    char out[96];
    snprintf(out, sizeof out, ", %s hits %llu/%llu (%.1f%%)", name,
             static_cast<unsigned long long>(h),
             static_cast<unsigned long long>(l),
             l == 0 ? 0.0 : 100.0 * double(h) / double(l));
    return out;
}

RestTable::RestTable(char const* program, PairsCounts const& bag,
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
    rows_.reset(static_cast<BagStatsCounts*>(
        std::calloc(capacity_, sizeof(BagStatsCounts))));
    if (!keys_ || !rows_index_ || !rows_) {
        keys_.reset();
        rows_index_.reset();
        rows_.reset();
    }
}

void report_rest_table(char const* program, RestTable const& table) {
    if (!table.enabled()) return;
    fprintf(stderr, "%s: cache holds %zu entries (%.1f MB) of %zu "
            "remainders\n", program, table.capacity(),
            double(table.capacity() * REST_CACHE_ENTRY_BYTES) /
                (1024 * 1024),
            table.remainders());
}
