#include "bag-stats-gpu.h"
#include "log.h"

#include <cuda_runtime.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <string_view>
#include <thread>

namespace {

using Key64 = unsigned long long;
using Key128 = unsigned __int128;

constexpr int LEFTOVER_THREADS = 256;
constexpr int PAIR_THREADS = 512;
constexpr int PAIR_WARPS = PAIR_THREADS / 32;
constexpr int CANDIDATE_CAPACITY = 1024;
constexpr int MAX_HALF = GPU_MAX_LETTERS / 2;
constexpr size_t SHARED_BYTES_MAX = 101376;
constexpr int FILTER_BLOCK_WORDS = 8;
constexpr int FILTER_BITS_PER_PAIR = 12;
constexpr size_t FILTER_RESERVE = size_t(1536) << 20;
constexpr int BUILD_THREADS = 256;
// The device memory the exact sum table leaves free, its slots a distinct
// sum at its most and least full, and the claimed share past which an
// insert gives up.
constexpr size_t SUM_TABLE_RESERVE = size_t(3) << 30;
constexpr double SUM_TABLE_SLOTS = 2.0;
constexpr double SUM_TABLE_SLOTS_LEAST = 1.45;
constexpr double SUM_TABLE_FULL = 0.8;
constexpr size_t SUM_SLOT_BYTES = sizeof(uint64_t) + sizeof(uint32_t);

static_assert(CANDIDATE_CAPACITY == 2 * PAIR_THREADS);

__constant__ uint32_t SALT[FILTER_BLOCK_WORDS] = {
    0x47b6137bU, 0x44974d91U, 0x8824ad5bU, 0xa2b7289dU,
    0x705495c7U, 0x2df1424bU, 0x9efc4947U, 0x5c6bfb31U,
};

std::string program_name = "bag-stats-cuda";

void check(cudaError_t error) {
    if (error == cudaSuccess) return;
    report(program_name, "%s", cudaGetErrorString(error));
    exit(1);
}

template <typename Key>
constexpr int WORD_CAPACITY = sizeof(Key) == 8 ? 2048 : 1024;

__host__ __device__ inline uint64_t mix64(uint64_t value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

__host__ __device__ inline uint64_t mix_key(Key64 key) { return mix64(key); }

__host__ __device__ inline uint64_t mix_key(Key128 key) {
    return mix64(uint64_t(key) ^ mix64(uint64_t(key >> 64)));
}

__host__ __device__ inline size_t home_slot(uint64_t hash, size_t slots) {
#ifdef __CUDA_ARCH__
    return size_t(__umul64hi(hash, slots));
#else
    return size_t((unsigned __int128)hash * slots >> 64);
#endif
}

// A slot in the group hash: a group's key and index. Key 0 marks an empty
// slot.
template <typename Key>
struct GroupSlot {
    Key key;
    uint32_t index;
};

// What a block keeps in shared memory for its row's word list.
template <typename Key>
struct RowShared {
    Key word_keys[WORD_CAPACITY<Key>];
    uint8_t word_lengths[WORD_CAPACITY<Key>];
    uint8_t letters[GPU_LETTERS_BYTES];
    int row;
    int next_word;
    int words;
};

template <typename Key>
struct LeftoverShared {
    RowShared<Key> row;
    unsigned long long word_bags;
    unsigned long long solo;
    unsigned long long passed;
    // With the exact sum table: with_pairs, total_pairs, max_pairs and
    // with_min_pairs.
    unsigned long long counts[4];
};

template <typename Key>
struct PairShared {
    RowShared<Key> row;
    Key candidate_keys[CANDIDATE_CAPACITY];
    unsigned long long candidate_pairs[CANDIDATE_CAPACITY];
    int chunk_start[CANDIDATE_CAPACITY];
    uint8_t candidate_lengths[CANDIDATE_CAPACITY];
    // with_pairs, total_pairs, max_pairs and with_min_pairs.
    unsigned long long counts[4];
    int pos[MAX_HALF + 2];
    int warp_counts[PAIR_WARPS];
    int candidate_count;
};

template <typename Key>
struct Params {
    Key const* group_keys;
    uint8_t const* group_lengths;
    uint32_t const* group_sizes;
    int const* lenstart;
    GroupSlot<Key> const* group_hash;
    size_t group_slots;
    Key const* word_keys;
    uint8_t const* word_lengths;
    int word_count;
    Key bag_guards;
    uint8_t bag_shifts[26];
    int min_group_length;
    int longest_sum;
    int words;
    unsigned long long min_pairs;
    uint8_t const* rests;
    int rows;
    // The next row, the next flagged row and the flagged count.
    int* next;
    int* flagged;
    GpuCounts* out;
    Key* word_list_keys;
    uint8_t* word_list_lengths;
    Key* list_keys;
    uint32_t* list_indexes;
    size_t list_capacity;
    uint32_t const* filter;
    size_t filter_blocks;
    unsigned long long* filter_counts;
    // The exact sum table, or null: each slot's key, 0 while empty, and its
    // sum's pairs.
    uint64_t const* sum_keys;
    uint32_t const* sum_counts;
    size_t sum_slots;
};

// What pair_sums does with each sum: counts them and sketches the distinct
// ones, sets its filter bits, or adds its pairs to the exact sum table.
enum class PairSumMode { COUNT, FILTER, TABLE };

// What the pair-sum kernels take.
template <typename BagKey>
struct PairSumParams {
    BagKey const* group_keys;
    uint8_t const* group_lengths;
    uint32_t const* group_sizes;
    int const* lenstart;
    int group_count;
    BagKey bag;
    BagKey bag_guards;
    int bag_length;
    uint32_t* filter;
    size_t filter_blocks;
    unsigned long long* pairs;
    int* longest_sum;
    // With COUNT, the HyperLogLog registers of the sums, 2^GPU_HLL_BITS.
    uint32_t* registers;
    // With TABLE: the table, the slots claimed, and nonzero once an insert
    // found the table too full.
    uint64_t* sum_keys;
    uint32_t* sum_counts;
    size_t sum_slots;
    unsigned long long* claimed;
    int* full;
};

// Whether `key` fits `pool`, which carries its guard bits; writes what is
// left to `rest`.
template <typename Key>
__device__ inline bool fits(Key pool, Key key, Key guards, Key* rest) {
    Key const left = pool - key;
    *rest = left & ~guards;
    return (left & guards) == guards;
}

__device__ inline unsigned long long warp_sum(unsigned long long value) {
    for (int offset = 16; offset > 0; offset >>= 1)
        value += __shfl_xor_sync(~0u, value, offset);
    return value;
}

__device__ inline uint32_t filter_bit(uint64_t hash, int k) {
    return 1u << ((uint32_t(mix64(hash)) * SALT[k]) >> 27);
}

__device__ inline void filter_insert(uint32_t* filter, size_t blocks,
                                     uint64_t hash) {
    uint32_t* const block =
        filter + home_slot(hash, blocks) * FILTER_BLOCK_WORDS;
    for (int k = 0; k < FILTER_BLOCK_WORDS; ++k)
        atomicOr(&block[k], filter_bit(hash, k));
}

// Whether the pair-sum filter holds `key`.
template <typename Key>
__device__ inline bool passes(Params<Key> const& p, Key key) {
    uint64_t const hash = mix_key(key);
    uint4 const* block = reinterpret_cast<uint4 const*>(
        p.filter + home_slot(hash, p.filter_blocks) * FILTER_BLOCK_WORDS);
    uint4 const low = block[0];
    uint4 const high = block[1];
    uint32_t const words[FILTER_BLOCK_WORDS] = {
        low.x, low.y, low.z, low.w, high.x, high.y, high.z, high.w};
    bool out = true;
    for (int k = 0; k < FILTER_BLOCK_WORDS; ++k)
        out &= (words[k] & filter_bit(hash, k)) != 0;
    return out;
}

// A sum's key in the exact sum table: the sum itself, or its hash when it
// is wider than a slot's key. Never 0.
__device__ inline uint64_t sum_table_key(Key64 sum) { return sum; }

__device__ inline uint64_t sum_table_key(Key128 sum) {
    uint64_t const hash = mix_key(sum);
    return hash == 0 ? 1 : hash;
}

// The pairs of the sum `key` in the exact sum table of `p`.
template <typename Key>
__device__ inline uint32_t sum_pairs(Params<Key> const& p, Key key) {
    uint64_t const stored = sum_table_key(key);
    for (size_t i = home_slot(mix_key(key), p.sum_slots);;
         i = i + 1 == p.sum_slots ? 0 : i + 1) {
        uint64_t const at = __ldcg(&p.sum_keys[i]);
        if (at == stored) return __ldcg(&p.sum_counts[i]);
        if (at == 0) return 0;
    }
}

// One warp per group `a`, over the groups `b` from `a` on whose sum with `a`
// fits the bag. COUNT counts them, finds the longest such sum and sketches
// the distinct sums; FILTER sets the sums' bits in the filter; TABLE adds
// each pair's count to its sum's slot of the exact sum table.
template <typename BagKey, PairSumMode MODE>
__global__ void pair_sums(PairSumParams<BagKey> p) {
    int const a = int((size_t(blockIdx.x) * blockDim.x + threadIdx.x) >> 5);
    int const lane = threadIdx.x & 31;
    if (a >= p.group_count) return;
    BagKey const key = p.group_keys[a];
    BagKey left;
    if (!fits(p.bag | p.bag_guards, key, p.bag_guards, &left)) return;
    BagKey const pool = left | p.bag_guards;
    int const end = p.lenstart[p.bag_length - p.group_lengths[a] + 1];
    uint32_t const size = p.group_sizes[a];
    bool const twice = size >= 2;
    unsigned long long pairs = 0;
    unsigned long long claimed = 0;
    int longest = 0;
    if constexpr (MODE == PairSumMode::TABLE) {
        if (__ldcv(p.full) != 0) return;
    }
    for (int b = a + lane; b < end; b += 32) {
        if (b == a && !twice) continue;
        BagKey rest;
        if (!fits(pool, p.group_keys[b], p.bag_guards, &rest)) continue;
        BagKey const sum = key + p.group_keys[b];
        if constexpr (MODE == PairSumMode::FILTER) {
            filter_insert(p.filter, p.filter_blocks, mix_key(sum));
        } else if constexpr (MODE == PairSumMode::TABLE) {
            uint32_t const weight = b == a
                ? uint32_t(uint64_t(size) * (size - 1) / 2)
                : size * p.group_sizes[b];
            uint64_t const stored = sum_table_key(sum);
            size_t i = home_slot(mix_key(sum), p.sum_slots);
            size_t probes = 0;
            while (true) {
                uint64_t at = p.sum_keys[i];
                if (at == 0) {
                    at = atomicCAS(
                        reinterpret_cast<unsigned long long*>(&p.sum_keys[i]),
                        0ULL, (unsigned long long)stored);
                    if (at == 0) {
                        ++claimed;
                        at = stored;
                    }
                }
                if (at == stored) {
                    atomicAdd(&p.sum_counts[i], weight);
                    break;
                }
                i = i + 1 == p.sum_slots ? 0 : i + 1;
                if (++probes >= 4096 || probes >= p.sum_slots) {
                    atomicExch(p.full, 1);
                    break;
                }
            }
        } else {
            ++pairs;
            longest = max(longest, int(p.group_lengths[b]));
            uint64_t const mixed = mix_key(sum);
            uint64_t const after = (mixed << GPU_HLL_BITS) |
                (uint64_t(1) << (GPU_HLL_BITS - 1));
            uint32_t const rank = uint32_t(__clzll((long long)after) + 1);
            uint32_t* const slot =
                p.registers + (mixed >> (64 - GPU_HLL_BITS));
            if (rank > __ldcg(slot)) atomicMax(slot, rank);
        }
    }
    if constexpr (MODE == PairSumMode::COUNT) {
        if (longest != 0)
            atomicMax(p.longest_sum, int(p.group_lengths[a]) + longest);
        pairs = warp_sum(pairs);
        if (lane == 0 && pairs != 0) atomicAdd(p.pairs, pairs);
    } else if constexpr (MODE == PairSumMode::TABLE) {
        claimed = warp_sum(claimed);
        if (lane == 0 && claimed != 0) atomicAdd(p.claimed, claimed);
    }
}

// The index of the group with `key`, or -1.
template <typename Key>
__device__ inline int find_group(GroupSlot<Key> const* hash, size_t slots,
                                 Key key) {
    for (size_t i = home_slot(mix_key(key), slots);;
         i = i + 1 == slots ? 0 : i + 1) {
        GroupSlot<Key> const slot = hash[i];
        if (slot.key == key) return int(slot.index);
        if (slot.key == 0) return -1;
    }
}

// A row's letters packed for the bag, its length, and the word bags that
// fit it.
template <typename Key>
struct Row {
    Key y;
    int length;
    Key const* word_keys;
    uint8_t const* word_lengths;
    int words;
};

// Loads `row` into `s` and compacts its word list, into shared memory or,
// past WORD_CAPACITY, into `big_keys` and `big_lengths`. Every thread calls
// it, after s.next_word and s.words are zeroed.
template <typename Key>
__device__ Row<Key> load_row(Params<Key> const& p, RowShared<Key>& s, int row,
                             Key* big_keys, uint8_t* big_lengths) {
    int const tid = threadIdx.x;
    int const threads = blockDim.x;
    if (tid < GPU_LETTERS_BYTES)
        s.letters[tid] = p.rests[size_t(row) * GPU_LETTERS_BYTES + tid];
    __syncthreads();
    Row<Key> out{};
    for (int letter = 0; letter < 26; ++letter) {
        out.y |= Key(s.letters[letter]) << p.bag_shifts[letter];
        out.length += s.letters[letter];
    }
    Key const pool = out.y | p.bag_guards;
    for (int j = tid; j < p.word_count; j += threads) {
        Key const word = p.word_keys[j];
        Key left;
        if (!fits(pool, word, p.bag_guards, &left)) continue;
        int const k = atomicAdd(&s.words, 1);
        if (k < WORD_CAPACITY<Key>) {
            s.word_keys[k] = word;
            s.word_lengths[k] = p.word_lengths[j];
        } else {
            big_keys[k] = word;
            big_lengths[k] = p.word_lengths[j];
        }
    }
    __syncthreads();
    out.words = s.words;
    if (out.words <= WORD_CAPACITY<Key>) {
        out.word_keys = s.word_keys;
        out.word_lengths = s.word_lengths;
        return out;
    }
    for (int k = tid; k < WORD_CAPACITY<Key>; k += threads) {
        big_keys[k] = s.word_keys[k];
        big_lengths[k] = s.word_lengths[k];
    }
    __syncthreads();
    out.word_keys = big_keys;
    out.word_lengths = big_lengths;
    return out;
}

// A warp's walk over a row's word bag combinations: word bags j1 <= j2 <=
// ... <= jk from the row's word list, every depth's rest fitting. Each
// step() hands each lane at most one leftover.
template <typename Key>
struct LeftoverWalk {
    Row<Key> const& row;
    int* next_word;
    int words;
    Key guards;
    int lane;
    // The rests and lengths after picks[0..depth-1].
    Key rests[GPU_MAX_WORDS];
    int lengths[GPU_MAX_WORDS];
    int picks[GPU_MAX_WORDS];
    int depth = 0;
    // The next word bag at the last depth.
    int next = 0;

    // Sets `valid`, and for a valid lane its leftover's `key` and `length`,
    // for the warp's next chunk; false once the row has no work left for the
    // warp. Warp-uniform.
    __device__ bool step(bool* valid, Key* key, int* length) {
        while (true) {
            if (depth == 0) {
                int first = 0;
                if (lane == 0) first = atomicAdd(next_word, words == 1 ? 32 : 1);
                first = __shfl_sync(~0u, first, 0);
                if (first >= row.words) return false;
                if (words == 1) {
                    int const j = first + lane;
                    *valid = j < row.words;
                    if (*valid) {
                        *key = row.y - row.word_keys[j];
                        *length = row.length - row.word_lengths[j];
                    }
                    return true;
                }
                picks[0] = first;
                rests[0] = row.y - row.word_keys[first];
                lengths[0] = row.length - row.word_lengths[first];
                depth = 1;
                if (words == 2)
                    next = first;
                else
                    picks[1] = first;
                continue;
            }
            if (depth == words - 1) {
                if (next >= row.words) {
                    if (--depth > 0) ++picks[depth];
                    continue;
                }
                int const j = next + lane;
                next += 32;
                Key left;
                *valid = j < row.words &&
                    fits(rests[depth - 1] | guards, row.word_keys[j], guards,
                         &left);
                if (*valid) {
                    *key = left;
                    *length = lengths[depth - 1] - row.word_lengths[j];
                }
                return true;
            }
            int const j = picks[depth];
            if (j >= row.words) {
                if (--depth > 0) ++picks[depth];
                continue;
            }
            Key left;
            if (!fits(rests[depth - 1] | guards, row.word_keys[j], guards,
                      &left)) {
                ++picks[depth];
                continue;
            }
            rests[depth] = left;
            lengths[depth] = lengths[depth - 1] - row.word_lengths[j];
            ++depth;
            if (depth == words - 1)
                next = j;
            else
                picks[depth] = j;
        }
    }
};

// Counts each row's word bags and solo leftovers, and flags the rows with a
// leftover that passes the pair-sum filter.
template <typename Key>
__global__ void __launch_bounds__(LEFTOVER_THREADS)
count_leftovers(Params<Key> p) {
    extern __shared__ __align__(16) unsigned char shared_bytes[];
    LeftoverShared<Key>& s =
        *reinterpret_cast<LeftoverShared<Key>*>(shared_bytes);
    int const tid = threadIdx.x;
    int const lane = tid & 31;
    Key* const big_keys = p.word_list_keys + size_t(blockIdx.x) * p.word_count;
    uint8_t* const big_lengths =
        p.word_list_lengths + size_t(blockIdx.x) * p.word_count;

    unsigned long long leftovers = 0;
    unsigned long long scanned = 0;
    while (true) {
        if (tid == 0) {
            s.row.row = atomicAdd(&p.next[0], 1);
            s.row.next_word = 0;
            s.row.words = 0;
            s.word_bags = 0;
            s.solo = 0;
            s.passed = 0;
            for (int n = 0; n < 4; ++n) s.counts[n] = 0;
        }
        __syncthreads();
        int const row = s.row.row;
        if (row >= p.rows) {
            leftovers = warp_sum(leftovers);
            scanned = warp_sum(scanned);
            if (lane == 0) {
                atomicAdd(&p.filter_counts[0], leftovers);
                atomicAdd(&p.filter_counts[1], scanned);
            }
            return;
        }
        Row<Key> const r = load_row(p, s.row, row, big_keys, big_lengths);
        LeftoverWalk<Key> walk{r, &s.row.next_word, p.words, p.bag_guards,
                               lane};
        unsigned long long word_bags = 0;
        unsigned long long solo = 0;
        unsigned long long passed = 0;
        unsigned long long with_pairs = 0;
        unsigned long long total_pairs = 0;
        unsigned long long max_pairs = 0;
        unsigned long long with_min_pairs = 0;
        bool valid;
        Key key;
        int length;
        while (walk.step(&valid, &key, &length)) {
            if (!valid) continue;
            ++word_bags;
            if (length == 0) {
                ++solo;
                continue;
            }
            if (length < 2 * p.min_group_length || length > p.longest_sum)
                continue;
            ++leftovers;
            if (p.sum_keys != nullptr) {
                unsigned long long const pairs = sum_pairs(p, key);
                if (pairs == 0) continue;
                ++scanned;
                ++with_pairs;
                total_pairs += pairs;
                max_pairs = max(max_pairs, pairs);
                if (pairs >= p.min_pairs) ++with_min_pairs;
                continue;
            }
            if (!passes(p, key)) continue;
            ++scanned;
            ++passed;
        }
        word_bags = warp_sum(word_bags);
        solo = warp_sum(solo);
        passed = warp_sum(passed);
        if (lane == 0) {
            atomicAdd(&s.word_bags, word_bags);
            atomicAdd(&s.solo, solo);
            atomicAdd(&s.passed, passed);
        }
        if (p.sum_keys != nullptr) {
            with_pairs = warp_sum(with_pairs);
            total_pairs = warp_sum(total_pairs);
            with_min_pairs = warp_sum(with_min_pairs);
            for (int offset = 16; offset > 0; offset >>= 1)
                max_pairs =
                    max(max_pairs, __shfl_xor_sync(~0u, max_pairs, offset));
            if (lane == 0 && with_pairs != 0) {
                atomicAdd(&s.counts[0], with_pairs);
                atomicAdd(&s.counts[1], total_pairs);
                atomicMax(&s.counts[2], max_pairs);
                atomicAdd(&s.counts[3], with_min_pairs);
            }
        }
        __syncthreads();
        if (tid == 0) {
            p.out[row] = {s.word_bags, s.counts[0], s.counts[1], s.counts[2],
                          s.solo, s.counts[3]};
            if (s.passed != 0) p.flagged[atomicAdd(&p.next[2], 1)] = row;
        }
    }
}

// Counts the pairs of the block's candidates and adds them to the row's
// counts, building the row's group list first if it isn't built yet.
template <typename Key>
__device__ void flush(Params<Key> const& p, PairShared<Key>& s,
                      Row<Key> const& r, Key* list_keys,
                      uint32_t* list_indexes, bool* built) {
    int const tid = threadIdx.x;
    int const lane = tid & 31;
    int const warp = tid >> 5;
    if (!*built) {
        int const half = r.length / 2;
        Key const pool = r.y | p.bag_guards;
        int const end = p.lenstart[half + 1];
        int base = 0;
        for (int tile = 0; tile < end; tile += PAIR_THREADS) {
            int const i = tile + tid;
            Key left;
            bool const fit =
                i < end && fits(pool, p.group_keys[i], p.bag_guards, &left);
            unsigned const ballot = __ballot_sync(~0u, fit);
            if (lane == 0) s.warp_counts[warp] = __popc(ballot);
            __syncthreads();
            int offset = base;
            for (int w = 0; w < PAIR_WARPS; ++w) {
                int const n = s.warp_counts[w];
                if (w < warp) offset += n;
                base += n;
            }
            if (fit) {
                int const k = offset + __popc(ballot & ((1u << lane) - 1));
                list_keys[k] = p.group_keys[i];
                list_indexes[k] = uint32_t(i);
            }
            __syncthreads();
        }
        for (int l = tid; l <= half + 1; l += PAIR_THREADS) {
            int low = 0;
            int high = base;
            while (low < high) {
                int const mid = (low + high) / 2;
                if (p.group_lengths[list_indexes[mid]] < l)
                    low = mid + 1;
                else
                    high = mid;
            }
            s.pos[l] = low;
        }
        __syncthreads();
        *built = true;
    }

    int const count = s.candidate_count;
    int chunks[2];
    for (int e = 0; e < 2; ++e) {
        int const c = 2 * tid + e;
        chunks[e] = c < count
            ? (s.pos[s.candidate_lengths[c] / 2 + 1] + 31) / 32
            : 0;
    }
    int const sum = chunks[0] + chunks[1];
    int inclusive = sum;
    for (int offset = 1; offset < 32; offset <<= 1) {
        int const n = __shfl_up_sync(~0u, inclusive, offset);
        if (lane >= offset) inclusive += n;
    }
    if (lane == 31) s.warp_counts[warp] = inclusive;
    __syncthreads();
    int start = inclusive - sum;
    int total = 0;
    for (int w = 0; w < PAIR_WARPS; ++w) {
        int const n = s.warp_counts[w];
        if (w < warp) start += n;
        total += n;
    }
    s.chunk_start[2 * tid] = start;
    s.chunk_start[2 * tid + 1] = start + chunks[0];
    __syncthreads();

    for (int t = warp; t < total; t += PAIR_WARPS) {
        int low = 0;
        int high = count - 1;
        while (low < high) {
            int const mid = (low + high + 1) / 2;
            if (s.chunk_start[mid] <= t)
                low = mid;
            else
                high = mid - 1;
        }
        int const c = low;
        Key const leftover = s.candidate_keys[c];
        int const length = s.candidate_lengths[c];
        int const k = (t - s.chunk_start[c]) * 32 + lane;
        unsigned long long pairs = 0;
        Key rest;
        if (k < s.pos[length / 2 + 1] &&
            fits(leftover | p.bag_guards, list_keys[k], p.bag_guards, &rest)) {
            uint32_t const i = list_indexes[k];
            unsigned long long const size = p.group_sizes[i];
            if (rest == list_keys[k]) {
                pairs = size * (size - 1) / 2;
            } else {
                int const b = find_group(p.group_hash, p.group_slots, rest);
                if (b >= 0 &&
                    (2 * p.group_lengths[i] < length || uint32_t(b) > i))
                    pairs = size * p.group_sizes[b];
            }
        }
        pairs = warp_sum(pairs);
        if (lane == 0 && pairs != 0) atomicAdd(&s.candidate_pairs[c], pairs);
    }
    __syncthreads();

    for (int c = tid; c < count; c += PAIR_THREADS) {
        unsigned long long const pairs = s.candidate_pairs[c];
        if (pairs == 0) continue;
        s.candidate_pairs[c] = 0;
        atomicAdd(&s.counts[0], 1ULL);
        atomicAdd(&s.counts[1], pairs);
        atomicMax(&s.counts[2], pairs);
        if (pairs >= p.min_pairs) atomicAdd(&s.counts[3], 1ULL);
    }
    if (tid == 0) s.candidate_count = 0;
    __syncthreads();
}

// Counts the pairs of each flagged row's leftovers that pass the pair-sum
// filter.
template <typename Key>
__global__ void __launch_bounds__(PAIR_THREADS, 2)
count_pairs(Params<Key> p) {
    static_assert(2 * sizeof(PairShared<Key>) <= SHARED_BYTES_MAX);
    extern __shared__ __align__(16) unsigned char shared_bytes[];
    PairShared<Key>& s = *reinterpret_cast<PairShared<Key>*>(shared_bytes);
    int const tid = threadIdx.x;
    int const lane = tid & 31;
    Key* const big_keys = p.word_list_keys + size_t(blockIdx.x) * p.word_count;
    uint8_t* const big_lengths =
        p.word_list_lengths + size_t(blockIdx.x) * p.word_count;
    Key* const list_keys = p.list_keys + blockIdx.x * p.list_capacity;
    uint32_t* const list_indexes =
        p.list_indexes + blockIdx.x * p.list_capacity;

    for (int c = tid; c < CANDIDATE_CAPACITY; c += PAIR_THREADS)
        s.candidate_pairs[c] = 0;
    if (tid == 0) s.candidate_count = 0;
    while (true) {
        if (tid == 0) {
            int const k = atomicAdd(&p.next[1], 1);
            s.row.row = k < p.next[2] ? p.flagged[k] : -1;
            s.row.next_word = 0;
            s.row.words = 0;
            for (int n = 0; n < 4; ++n) s.counts[n] = 0;
        }
        __syncthreads();
        int const row = s.row.row;
        if (row < 0) return;
        Row<Key> const r = load_row(p, s.row, row, big_keys, big_lengths);
        LeftoverWalk<Key> walk{r, &s.row.next_word, p.words, p.bag_guards,
                               lane};
        bool built = false;
        bool more = true;
        while (__syncthreads_or(more)) {
            if (more) {
                bool valid;
                Key key;
                int length;
                more = walk.step(&valid, &key, &length);
                if (more && valid && length >= 2 * p.min_group_length &&
                    length <= p.longest_sum && passes(p, key)) {
                    int const c = atomicAdd(&s.candidate_count, 1);
                    s.candidate_keys[c] = key;
                    s.candidate_lengths[c] = uint8_t(length);
                }
            }
            __syncthreads();
            if (s.candidate_count > CANDIDATE_CAPACITY - PAIR_THREADS)
                flush(p, s, r, list_keys, list_indexes, &built);
        }
        if (s.candidate_count > 0)
            flush(p, s, r, list_keys, list_indexes, &built);
        if (tid == 0) {
            GpuCounts& out = p.out[row];
            out.with_pairs = s.counts[0];
            out.total_pairs = s.counts[1];
            out.max_pairs = s.counts[2];
            out.with_min_pairs = s.counts[3];
        }
    }
}

// A bag's packing, as PairsPacking makes it.
struct Packing {
    uint8_t shifts[26] = {};
    Key128 guards = 0;
    int bits = 0;

    explicit Packing(uint8_t const* bag) {
        for (int letter = 0; letter < 26; ++letter) {
            unsigned const value = bag[letter];
            if (value == 0) continue;
            int width = 1;
            while ((value >> width) != 0) ++width;
            ++width;
            shifts[letter] = uint8_t(bits);
            guards |= Key128(1) << (bits + width - 1);
            bits += width;
        }
    }

    template <typename Key>
    Key pack(uint8_t const* letters) const {
        Key out = 0;
        for (int letter = 0; letter < 26; ++letter)
            out |= Key(letters[letter]) << shifts[letter];
        return out;
    }
};

int rest_length(uint8_t const* letters) {
    int length = 0;
    for (int letter = 0; letter < 26; ++letter) length += letters[letter];
    return length;
}

}  // namespace

struct GpuRowCounter::Impl {
    struct Stream {
        cudaStream_t stream = nullptr;
        uint8_t* rests = nullptr;
        GpuCounts* out = nullptr;
        int* next = nullptr;
        int* flagged = nullptr;
        void* word_keys = nullptr;
        uint8_t* word_lengths = nullptr;
        void* list_keys = nullptr;
        uint32_t* list_indexes = nullptr;
        unsigned long long* filter_counts = nullptr;
        uint8_t* host_rests = nullptr;
        GpuCounts* host_out = nullptr;
        unsigned long long* host_filter_counts = nullptr;
        std::vector<uint32_t> order;
        bool busy = false;
    };

    std::string device;
    int key_bits = 0;
    size_t bytes = 0;
    int sms = 0;
    int leftover_blocks = 0;
    int pair_blocks = 0;
    int group_count = 0;
    int word_count = 0;
    int words = 0;
    int min_group_length = 0;
    unsigned long long min_pairs = 0;
    uint8_t bag_shifts[26] = {};
    Key128 bag_guards = 0;
    size_t group_slots = 0;
    size_t list_capacity = 0;
    void* group_keys = nullptr;
    void* group_hash = nullptr;
    uint8_t* group_lengths = nullptr;
    uint32_t* group_sizes = nullptr;
    int* lenstart = nullptr;
    void* word_keys = nullptr;
    uint8_t* word_lengths = nullptr;
    uint32_t* filter = nullptr;
    size_t filter_blocks = 0;
    uint64_t* sum_keys = nullptr;
    uint32_t* sum_counts = nullptr;
    size_t sum_slots = 0;
    GpuFilterInfo filter_info{};
    GpuFilterCounts filter_counts{};
    std::vector<Stream> streams;
    std::mutex mutex;
    std::condition_variable freed;
    // Every cudaMalloc buffer, for the destructor.
    std::vector<void*> buffers;

    ~Impl() {
        for (Stream& stream : streams) {
            if (stream.stream != nullptr)
                check(cudaStreamDestroy(stream.stream));
            if (stream.host_rests != nullptr)
                check(cudaFreeHost(stream.host_rests));
            if (stream.host_out != nullptr)
                check(cudaFreeHost(stream.host_out));
            if (stream.host_filter_counts != nullptr)
                check(cudaFreeHost(stream.host_filter_counts));
        }
        for (void* buffer : buffers) check(cudaFree(buffer));
    }

    template <typename T>
    T* allocate(size_t count) {
        void* out = nullptr;
        size_t const size = std::max<size_t>(count * sizeof(T), 16);
        check(cudaMalloc(&out, size));
        buffers.push_back(out);
        bytes += size;
        return static_cast<T*>(out);
    }

    template <typename T>
    T* upload(std::vector<T> const& from) {
        T* out = allocate<T>(from.size());
        check(cudaMemcpy(out, from.data(), from.size() * sizeof(T),
                         cudaMemcpyHostToDevice));
        return out;
    }

    template <typename Key>
    void upload_keys(GpuInputs const& inputs, Packing const& packing) {
        std::vector<Key> keys(group_count);
        for (int i = 0; i < group_count; ++i)
            keys[i] = packing.pack<Key>(&inputs.group_letters[size_t(i) *
                                                              GPU_LETTERS_BYTES]);
        group_keys = upload(keys);
        std::vector<GroupSlot<Key>> hash(group_slots, GroupSlot<Key>{0, 0});
        for (int i = 0; i < group_count; ++i) {
            size_t k = home_slot(mix_key(keys[i]), group_slots);
            while (hash[k].key != 0) k = k + 1 == group_slots ? 0 : k + 1;
            hash[k] = {keys[i], uint32_t(i)};
        }
        group_hash = upload(hash);
        keys.assign(word_count, 0);
        for (int j = 0; j < word_count; ++j)
            keys[j] = packing.pack<Key>(&inputs.word_letters[size_t(j) *
                                                             GPU_LETTERS_BYTES]);
        word_keys = upload(keys);
    }

    template <typename Key>
    void allocate_stream(Stream* stream) {
        check(cudaStreamCreateWithFlags(&stream->stream,
                                        cudaStreamNonBlocking));
        stream->rests = allocate<uint8_t>(GPU_BATCH_ROWS * GPU_LETTERS_BYTES);
        stream->out = allocate<GpuCounts>(GPU_BATCH_ROWS);
        stream->next = allocate<int>(3);
        stream->flagged = allocate<int>(GPU_BATCH_ROWS);
        int const scan_blocks = sum_keys == nullptr ? pair_blocks : 0;
        size_t const word_entries =
            size_t(leftover_blocks + scan_blocks) * word_count;
        stream->word_keys = allocate<Key>(word_entries);
        stream->word_lengths = allocate<uint8_t>(word_entries);
        stream->list_keys = allocate<Key>(size_t(scan_blocks) * list_capacity);
        stream->list_indexes =
            allocate<uint32_t>(size_t(scan_blocks) * list_capacity);
        stream->filter_counts = allocate<unsigned long long>(2);
        check(cudaMallocHost(&stream->host_rests,
                             GPU_BATCH_ROWS * GPU_LETTERS_BYTES));
        check(cudaMallocHost(&stream->host_out,
                             GPU_BATCH_ROWS * sizeof(GpuCounts)));
        check(cudaMallocHost(&stream->host_filter_counts,
                             2 * sizeof(unsigned long long)));
    }

    // Counts the sums of two groups that fit the bag, then builds the exact
    // sum table when the device has the room for it, else the pair-sum
    // filter.
    template <typename Key>
    void build_sums(uint8_t const* bag, Packing const& packing, int letters) {
        auto const start = std::chrono::steady_clock::now();
        PairSumParams<Key> p{};
        p.group_keys = static_cast<Key const*>(group_keys);
        p.group_lengths = group_lengths;
        p.group_sizes = group_sizes;
        p.lenstart = lenstart;
        p.group_count = group_count;
        p.bag = packing.pack<Key>(bag);
        p.bag_guards = Key(bag_guards);
        p.bag_length = letters;
        p.pairs = allocate<unsigned long long>(1);
        check(cudaMemset(p.pairs, 0, sizeof(unsigned long long)));
        p.longest_sum = allocate<int>(1);
        check(cudaMemset(p.longest_sum, 0, sizeof(int)));
        size_t const registers = size_t(1) << GPU_HLL_BITS;
        p.registers = allocate<uint32_t>(registers);
        check(cudaMemset(p.registers, 0, registers * sizeof(uint32_t)));
        int const grid = (group_count + BUILD_THREADS / 32 - 1) /
            (BUILD_THREADS / 32);
        if (grid > 0)
            pair_sums<Key, PairSumMode::COUNT><<<grid, BUILD_THREADS>>>(p);
        check(cudaGetLastError());
        unsigned long long pairs = 0;
        int longest_sum = 0;
        check(cudaMemcpy(&pairs, p.pairs, sizeof pairs,
                         cudaMemcpyDeviceToHost));
        check(cudaMemcpy(&longest_sum, p.longest_sum, sizeof longest_sum,
                         cudaMemcpyDeviceToHost));
        std::vector<uint32_t> ranks(registers);
        check(cudaMemcpy(ranks.data(), p.registers,
                         registers * sizeof(uint32_t),
                         cudaMemcpyDeviceToHost));
        double inverse = 0;
        size_t empty = 0;
        for (uint32_t const rank : ranks) {
            inverse += std::ldexp(1.0, -int(rank));
            if (rank == 0) ++empty;
        }
        double const m = double(registers);
        double distinct = 0.7213 / (1 + 1.079 / m) * m * m / inverse;
        if (distinct <= 2.5 * m && empty != 0)
            distinct = m * std::log(m / double(empty));
        distinct = std::min(distinct * 1.05, double(pairs));

        size_t free = 0;
        size_t total = 0;
        check(cudaMemGetInfo(&free, &total));
        size_t const table_room = free > SUM_TABLE_RESERVE
            ? (free - SUM_TABLE_RESERVE) / SUM_SLOT_BYTES
            : 0;
        size_t const slots = std::min(
            size_t(distinct * SUM_TABLE_SLOTS) + 1024, table_room);
        bool exact = getenv("BSC_NO_SUM_TABLE") == nullptr && pairs != 0 &&
            double(slots) >= distinct * SUM_TABLE_SLOTS_LEAST;
        unsigned long long claimed = 0;
        if (exact) {
            void* keys = nullptr;
            void* counts = nullptr;
            check(cudaMalloc(&keys, slots * sizeof(uint64_t)));
            check(cudaMalloc(&counts, slots * sizeof(uint32_t)));
            check(cudaMemset(keys, 0, slots * sizeof(uint64_t)));
            check(cudaMemset(counts, 0, slots * sizeof(uint32_t)));
            p.sum_keys = static_cast<uint64_t*>(keys);
            p.sum_counts = static_cast<uint32_t*>(counts);
            p.sum_slots = slots;
            p.claimed = allocate<unsigned long long>(1);
            check(cudaMemset(p.claimed, 0, sizeof(unsigned long long)));
            p.full = allocate<int>(1);
            check(cudaMemset(p.full, 0, sizeof(int)));
            pair_sums<Key, PairSumMode::TABLE><<<grid, BUILD_THREADS>>>(p);
            check(cudaGetLastError());
            int full = 0;
            check(cudaMemcpy(&claimed, p.claimed, sizeof claimed,
                             cudaMemcpyDeviceToHost));
            check(cudaMemcpy(&full, p.full, sizeof full,
                             cudaMemcpyDeviceToHost));
            exact = full == 0 && double(claimed) <= SUM_TABLE_FULL * slots;
            if (exact) {
                buffers.push_back(keys);
                buffers.push_back(counts);
                bytes += slots * SUM_SLOT_BYTES;
                sum_keys = p.sum_keys;
                sum_counts = p.sum_counts;
                sum_slots = slots;
            } else {
                check(cudaFree(keys));
                check(cudaFree(counts));
            }
        }
        if (exact) {
            filter_info = {
                pairs, longest_sum, slots * SUM_SLOT_BYTES,
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - start)
                    .count(),
                claimed, true};
            return;
        }
        if (pairs != 0 && getenv("BSC_NO_SUM_TABLE") == nullptr)
            alert(program_name,
                  "exact sum table needs %.1f GB for %.2fG sums, %.1f GB "
                  "free; counting with the slower pair-sum filter",
                  distinct * SUM_TABLE_SLOTS_LEAST * SUM_SLOT_BYTES / 1e9,
                  distinct / 1e9, double(free) / 1e9);

        size_t const room = free > FILTER_RESERVE
            ? (free - FILTER_RESERVE) / (FILTER_BLOCK_WORDS * sizeof(uint32_t))
            : 0;
        size_t const wanted = size_t(
            (pairs * FILTER_BITS_PER_PAIR + FILTER_BLOCK_WORDS * 32 - 1) /
            (FILTER_BLOCK_WORDS * 32));
        filter_blocks = std::max<size_t>(std::min(wanted, room), 1);
        size_t const filter_bytes =
            filter_blocks * FILTER_BLOCK_WORDS * sizeof(uint32_t);
        filter = allocate<uint32_t>(filter_blocks * FILTER_BLOCK_WORDS);
        check(cudaMemset(filter, 0, filter_bytes));
        p.filter = filter;
        p.filter_blocks = filter_blocks;
        if (grid > 0)
            pair_sums<Key, PairSumMode::FILTER><<<grid, BUILD_THREADS>>>(p);
        check(cudaGetLastError());
        check(cudaDeviceSynchronize());
        filter_info = {
            pairs, longest_sum, filter_bytes,
            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          start)
                .count(),
            0, false};
    }

    template <typename Key>
    void launch(Stream* stream, size_t rows) {
        Params<Key> p{};
        p.group_keys = static_cast<Key const*>(group_keys);
        p.group_lengths = group_lengths;
        p.group_sizes = group_sizes;
        p.lenstart = lenstart;
        p.group_hash = static_cast<GroupSlot<Key> const*>(group_hash);
        p.group_slots = group_slots;
        p.word_keys = static_cast<Key const*>(word_keys);
        p.word_lengths = word_lengths;
        p.word_count = word_count;
        p.bag_guards = Key(bag_guards);
        memcpy(p.bag_shifts, bag_shifts, sizeof bag_shifts);
        p.min_group_length = min_group_length;
        p.longest_sum = filter_info.longest_sum;
        p.words = words;
        p.min_pairs = min_pairs;
        p.rests = stream->rests;
        p.rows = int(rows);
        p.next = stream->next;
        p.flagged = stream->flagged;
        p.out = stream->out;
        p.word_list_keys = static_cast<Key*>(stream->word_keys);
        p.word_list_lengths = stream->word_lengths;
        p.list_keys = static_cast<Key*>(stream->list_keys);
        p.list_indexes = stream->list_indexes;
        p.list_capacity = list_capacity;
        p.filter = filter;
        p.filter_blocks = filter_blocks;
        p.filter_counts = stream->filter_counts;
        p.sum_keys = sum_keys;
        p.sum_counts = sum_counts;
        p.sum_slots = sum_slots;
        count_leftovers<Key><<<leftover_blocks, LEFTOVER_THREADS,
                               sizeof(LeftoverShared<Key>), stream->stream>>>(
            p);
        check(cudaGetLastError());
        if (sum_keys != nullptr) return;
        size_t const offset = size_t(leftover_blocks) * word_count;
        p.word_list_keys += offset;
        p.word_list_lengths += offset;
        count_pairs<Key><<<pair_blocks, PAIR_THREADS, sizeof(PairShared<Key>),
                           stream->stream>>>(p);
        check(cudaGetLastError());
    }

    // Lets the kernels take their shared memory and sets the grids.
    template <typename Key>
    void allow_shared() {
        check(cudaFuncSetAttribute(count_leftovers<Key>,
                                   cudaFuncAttributeMaxDynamicSharedMemorySize,
                                   int(sizeof(LeftoverShared<Key>))));
        check(cudaFuncSetAttribute(count_pairs<Key>,
                                   cudaFuncAttributeMaxDynamicSharedMemorySize,
                                   int(sizeof(PairShared<Key>))));
        int per_sm = 0;
        check(cudaOccupancyMaxActiveBlocksPerMultiprocessor(
            &per_sm, count_leftovers<Key>, LEFTOVER_THREADS,
            sizeof(LeftoverShared<Key>)));
        leftover_blocks = sms * std::max(per_sm, 1);
        pair_blocks = 2 * sms;
    }

    template <typename Key>
    void set_up(GpuInputs const& inputs, Packing const& packing, int letters) {
        upload_keys<Key>(inputs, packing);
        allow_shared<Key>();
        build_sums<Key>(inputs.bag, packing, letters);
        for (Stream& stream : streams) allocate_stream<Key>(&stream);
    }
};

GpuRowCounter::GpuRowCounter(std::string_view program, GpuInputs const& inputs,
                             size_t streams)
    : impl_(std::make_unique<Impl>()) {
    program_name = program;
    Impl& impl = *impl_;
    impl.streams.resize(streams);
    int device_count = 0;
    check(cudaGetDeviceCount(&device_count));
    check(cudaSetDevice(0));
    cudaDeviceProp properties;
    check(cudaGetDeviceProperties(&properties, 0));
    impl.device = properties.name;
    impl.sms = properties.multiProcessorCount;

    Packing const packing(inputs.bag);
    impl.key_bits = packing.bits <= 64 ? 64 : 128;
    memcpy(impl.bag_shifts, packing.shifts, sizeof impl.bag_shifts);
    impl.bag_guards = packing.guards;
    impl.group_count = int(inputs.group_sizes.size());
    impl.word_count = int(inputs.word_lengths.size());
    impl.words = inputs.words;
    impl.min_group_length = inputs.min_group_length;
    impl.min_pairs = inputs.min_pairs;

    int const letters = rest_length(inputs.bag);
    std::vector<int> lenstart(letters + 2);
    for (int length = 0; length <= letters + 1; ++length)
        lenstart[length] = int(
            std::lower_bound(inputs.group_lengths.begin(),
                             inputs.group_lengths.end(), length) -
            inputs.group_lengths.begin());
    impl.group_slots = 2 * size_t(impl.group_count) + 1;
    impl.list_capacity = size_t(lenstart[letters / 2 + 1]);

    impl.group_lengths = impl.upload(inputs.group_lengths);
    impl.group_sizes = impl.upload(inputs.group_sizes);
    impl.lenstart = impl.upload(lenstart);
    impl.word_lengths = impl.upload(inputs.word_lengths);
    if (impl.key_bits == 64)
        impl.set_up<Key64>(inputs, packing, letters);
    else
        impl.set_up<Key128>(inputs, packing, letters);
    check(cudaDeviceSynchronize());
}

GpuRowCounter::~GpuRowCounter() = default;

std::string const& GpuRowCounter::device() const { return impl_->device; }

int GpuRowCounter::key_bits() const { return impl_->key_bits; }

size_t GpuRowCounter::bytes() const { return impl_->bytes; }

size_t GpuRowCounter::streams() const { return impl_->streams.size(); }

GpuFilterInfo GpuRowCounter::filter_info() const {
    return impl_->filter_info;
}

GpuFilterCounts GpuRowCounter::filter_counts() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->filter_counts;
}

void GpuRowCounter::count(uint8_t const* rests, size_t rows,
                          GpuCounts* out) const {
    Impl& impl = *impl_;
    Impl::Stream* stream = nullptr;
    {
        std::unique_lock<std::mutex> lock(impl.mutex);
        impl.freed.wait(lock, [&] {
            for (Impl::Stream& free : impl.streams)
                if (!free.busy) {
                    stream = &free;
                    return true;
                }
            return false;
        });
        stream->busy = true;
    }

    std::vector<uint32_t>& order = stream->order;
    order.resize(rows);
    std::iota(order.begin(), order.end(), 0u);
    std::vector<int> sort_keys(rows);
    for (size_t row = 0; row < rows; ++row)
        sort_keys[row] =
            GPU_MAX_LETTERS - rest_length(rests + row * GPU_LETTERS_BYTES);
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return sort_keys[a] < sort_keys[b];
    });
    for (size_t k = 0; k < rows; ++k)
        memcpy(stream->host_rests + k * GPU_LETTERS_BYTES,
               rests + size_t(order[k]) * GPU_LETTERS_BYTES,
               GPU_LETTERS_BYTES);

    cudaStream_t const s = stream->stream;
    check(cudaMemcpyAsync(stream->rests, stream->host_rests,
                          rows * GPU_LETTERS_BYTES, cudaMemcpyHostToDevice,
                          s));
    check(cudaMemsetAsync(stream->next, 0, 3 * sizeof(int), s));
    check(cudaMemsetAsync(stream->filter_counts, 0,
                          2 * sizeof(unsigned long long), s));
    if (impl.key_bits == 64)
        impl.launch<Key64>(stream, rows);
    else
        impl.launch<Key128>(stream, rows);
    check(cudaMemcpyAsync(stream->host_out, stream->out,
                          rows * sizeof(GpuCounts), cudaMemcpyDeviceToHost,
                          s));
    check(cudaMemcpyAsync(stream->host_filter_counts, stream->filter_counts,
                          2 * sizeof(unsigned long long),
                          cudaMemcpyDeviceToHost, s));
    check(cudaStreamSynchronize(s));
    for (size_t k = 0; k < rows; ++k) out[order[k]] = stream->host_out[k];

    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.filter_counts.leftovers += stream->host_filter_counts[0];
        impl.filter_counts.scanned += stream->host_filter_counts[1];
        stream->busy = false;
    }
    impl.freed.notify_one();
}

namespace {

constexpr int WALK_THREADS = 256;
constexpr int WALK_WARPS = WALK_THREADS / 32;
constexpr int TASK_CANDIDATES = 64;
constexpr size_t MEMO_RESERVE = size_t(1) << 30;
constexpr size_t SUMS_RESERVE = size_t(1) << 30;
constexpr long double SLABS_MOST = 65536;
// A pilot walks a sample of a walk's tasks: how many, enough to keep every
// warp busy, and how long it may take.
constexpr unsigned long long PILOT_TASKS = 32768;
constexpr double PILOT_SECONDS = 0.4;
constexpr double PILOT_SHARE = 0.9;
// The candidates below the first picks from which a walk is worth a pilot.
constexpr unsigned long long PILOT_FROM = 100000000;
// The most rests the fit tables' dense letters number, and the most entries
// of their `two`.
constexpr uint64_t DENSE_MOST = 65536;
constexpr uint64_t TWO_MOST = uint64_t(1) << 29;
// The device memory the fit tables' third term leaves free.
constexpr size_t TRIPLE_RESERVE = size_t(1) << 30;
constexpr int SET_DRAWS = 50000;
constexpr size_t SET_KEPT = 5000;
// What a sums walk's slab costs: seconds a byte of its table, and besides.
constexpr double SUMS_SECONDS_PER_BYTE = 0.12e-9;
constexpr double SUMS_SECONDS_PER_SLAB = 0.1;
// The combinations a second a memo walk and a scanning walk get through.
constexpr double MEMO_RATE = 5e8;
constexpr double SCAN_RATE = 5e7;
constexpr int EVERY_CHUNK = 1 << 30;
constexpr int SET_PROBES = 256;
constexpr size_t SET_RESERVE = size_t(256) << 20;
constexpr uint32_t NO_RUN = ~0u;

// The work groups the walk's progress estimate puts units in.
constexpr int WORK_GROUPS = 64;

// The work group of a unit scanning `candidates` below its first pick:
// their bit length.
__device__ inline int work_group(unsigned long long candidates) {
    return min(64 - __clzll((long long)candidates), WORK_GROUPS - 1);
}

// The walk's progress: its finished units, and with 2 or more steps, by
// work group, the candidates they scanned below their first pick and in
// all.
struct WalkProgress {
    unsigned long long done;
    unsigned long long first[WORK_GROUPS];
    unsigned long long work[WORK_GROUPS];
    unsigned long long lookups;
    unsigned long long misses;
    unsigned long long miss_work;
    // The tasks finished.
    unsigned long long tasks;
};

// What a walk counts: the --leftovers counts; each pair bag's PAIRS rows of
// the combinations, by scanning the last step's candidates; the same from
// a memo of what fits each rest of the steps before the last; or nothing,
// collecting the distinct rests of the combinations.
enum class WalkCountType {
    LEFTOVERS, PAIR_USAGE, PAIR_USAGE_MEMO, RESTS,
    SUM_SLABS, SUM_SCATTER, PAIR_USAGE_SUMS, PAIR_USAGE_TABLES, TOP_ROWS
};

// A pair bag: its letters packed, its PAIRS, and its run above its letter
// count's 8 bits.
template <typename BagKey>
struct WalkBag {
    BagKey key;
    uint32_t pairs;
    uint32_t code;
};

template <typename BagKey>
struct LeftoverParams {
    WalkBag<BagKey> const* bags;
    uint64_t const* numbers;
    // Unless null, each pair bag's place in its list: picks from one list
    // are then in increasing place, not sorted position.
    uint32_t const* order;
    // Unless null, the units' pair bags, unit `first_unit` first.
    int const* units;
    // For the first steps' bags and then the other steps', the first pair
    // bag of each prefix cell, then the list's end.
    int const* offsets[2];
    // The units: how many, and the first one's pair bag.
    int count;
    int first_unit;
    // The steps, the first pair_steps from the first list.
    int steps;
    int pair_steps;
    // The prefix letters a run codes in unary: each one's key shift, count
    // mask and place in the run. Then the last prefix letter's key shift
    // and count mask, and the cell bits its count takes.
    int prefix;
    uint8_t shifts[GPU_MAX_PREFIX_LETTERS];
    uint8_t masks[GPU_MAX_PREFIX_LETTERS];
    uint8_t places[GPU_MAX_PREFIX_LETTERS];
    int last_shift;
    int last_mask;
    int last_bits;
    BagKey bag;
    BagKey bag_guards;
    uint64_t bag_number;
    int bag_letters;
    unsigned long long* next_unit;
    WalkProgress* progress;
    // With 2 or more steps, each unit's work group.
    uint8_t* groups;
    unsigned long long* by_picks;
    unsigned long long* combinations;
    unsigned long long* rows;
    uint32_t* registers;
    // With PAIR_USAGE, each pair bag's count, in sorted order.
    unsigned long long* usage;
};

// What the picks through a step leave: the rest, its number, the picks'
// PAIRS product and the rest's letters; and the step's pick, its PAIRS and,
// with the walk's order, its place.
template <typename BagKey>
struct Picked {
    BagKey rest;
    uint64_t number;
    unsigned long long rows;
    int letters;
    int index;
    uint32_t pairs;
    uint32_t place;
};

// The runs of prefix cells a pair bag fitting a rest can be in. A run is
// the cells that differ in the last prefix letter alone, and its number
// holds the other prefix letters' counts in unary, so the runs a bag can be
// in are the submasks of the rest's. `high` and `low` are that mask's bits,
// low its 5 lowest: a chunk is the runs with the same high bits, one per
// lane. Then a chunk's runs; the cells of a run a bag can be in; a run's
// shift to its first cell; the first sorted position past the rest's pick;
// the step's list's offsets; and whether the rest's pick is from that list
// and the walk is in sorted order.
struct Cells {
    uint32_t high;
    uint32_t low;
    int lanes;
    int width;
    int shift;
    int after;
    int const* offsets;
    bool same;
};

// Where a warp's walk of a rest's candidates is: its chunk, the chunks
// left with it, and a position in the chunk's ranges; the lane's low run
// bits; and once the chunk is `loaded`, the lane's run's first sorted
// position and length, the lengths summed through the lane, and the chunk's
// total.
struct Cursor {
    uint32_t chunk;
    int left;
    int position;
    uint32_t pattern;
    bool loaded;
    int begin;
    int length;
    int sum;
    int total;
};

// A warp's walk of a rest's candidates one at a time: the next run, the
// runs left, the next sorted position in the run it is in, and its end.
struct RunCursor {
    uint32_t run;
    int left;
    int next;
    int end;
};

// A block's counts in shared memory.
struct WalkCounts {
    unsigned long long* combinations;
    unsigned long long* rows;
    unsigned long long* by_picks;
};

__host__ __device__ inline size_t walk_counts_bytes(int letters, int steps) {
    size_t const bytes =
        (2 * size_t(letters + 1) + size_t(steps)) * sizeof(unsigned long long);
    return (bytes + 15) & ~size_t(15);
}

// Zeroes the block's counts at the start of `shared`. Every thread calls it.
template <typename BagKey>
__device__ WalkCounts start_walk_counts(LeftoverParams<BagKey> const& p,
                                        unsigned char* shared) {
    auto* const words = reinterpret_cast<unsigned long long*>(shared);
    int const lengths = p.bag_letters + 1;
    for (int k = threadIdx.x; k < 2 * lengths + p.steps; k += blockDim.x)
        words[k] = 0;
    __syncthreads();
    return {words, words + lengths, words + 2 * lengths};
}

// Adds the block's counts to the totals. Every thread calls it.
template <typename BagKey>
__device__ void flush_walk_counts(LeftoverParams<BagKey> const& p,
                                  WalkCounts const& c) {
    __syncthreads();
    for (int k = threadIdx.x; k <= p.bag_letters; k += blockDim.x) {
        if (c.combinations[k] == 0) continue;
        atomicAdd(&p.combinations[k], c.combinations[k]);
        atomicAdd(&p.rows[k], c.rows[k]);
    }
    for (int k = threadIdx.x; k < p.steps; k += blockDim.x)
        if (c.by_picks[k] != 0) atomicAdd(&p.by_picks[k], c.by_picks[k]);
}

// Whether pair bag `i` fits what `from` leaves; writes what picking it
// leaves to `out`.
template <WalkCountType COUNT_TYPE, typename BagKey>
__device__ inline bool pick(LeftoverParams<BagKey> const& p,
                            Picked<BagKey> const& from, int i,
                            Picked<BagKey>* out) {
    WalkBag<BagKey> const bag = p.bags[i];
    BagKey rest;
    if (!fits(from.rest | p.bag_guards, bag.key, p.bag_guards, &rest))
        return false;
    uint64_t number = 0;
    if constexpr (COUNT_TYPE != WalkCountType::PAIR_USAGE)
        number = from.number - p.numbers[i];
    *out = {rest, number, from.rows * bag.pairs,
            from.letters - int(bag.code & 0xff), i, bag.pairs,
            p.order != nullptr ? p.order[i] : 0};
    return true;
}

// Whether `next`, picked at `step` after `from`, comes after it in the
// walk's order.
template <typename BagKey>
__device__ inline bool in_order(LeftoverParams<BagKey> const& p,
                                Picked<BagKey> const& from,
                                Picked<BagKey> const& next, int step) {
    return p.order == nullptr ||
        (step >= p.pair_steps) != (step - 1 >= p.pair_steps) ||
        next.place > from.place;
}

// The pair bag of unit `unit`.
template <typename BagKey>
__device__ inline int unit_bag(LeftoverParams<BagKey> const& p, int unit) {
    return p.units != nullptr ? p.units[p.first_unit + unit]
                              : p.first_unit + unit;
}

// `value`'s low bits moved to the set bits of `mask`, lowest first.
__device__ inline uint32_t deposit(uint32_t value, uint32_t mask) {
    uint32_t out = 0;
    for (; mask != 0; mask &= mask - 1, value >>= 1)
        if ((value & 1) != 0) out |= mask & (0u - mask);
    return out;
}

// `value`'s bits at the set bits of `mask`, packed lowest first.
__device__ inline uint32_t extract(uint32_t value, uint32_t mask) {
    uint32_t out = 0;
    for (uint32_t bit = 1; mask != 0; mask &= mask - 1, bit <<= 1)
        if ((value & mask & (0u - mask)) != 0) out |= bit;
    return out;
}

// The submask of `mask` after `value`, 0 after the last.
__device__ inline uint32_t next_submask(uint32_t value, uint32_t mask) {
    return ((value | ~mask) + 1) & mask;
}

// The least submask of `mask` at or past `value`, or NO_RUN.
__device__ inline uint32_t submask_from(uint32_t value, uint32_t mask) {
    uint32_t const over = value & ~mask;
    if (over == 0) return value;
    int const shift = 32 - __clz(int(over));
    uint32_t const above = mask >> shift;
    uint32_t const next = next_submask(value >> shift, above);
    return next == 0 ? NO_RUN : next << shift;
}

// The runs a bag of the list at `offsets` that fits `rest` can be in, past
// sorted position `after` - 1.
template <typename BagKey>
__device__ inline Cells cells_for(LeftoverParams<BagKey> const& p,
                                  BagKey rest, int const* offsets,
                                  int after) {
    uint32_t allowed = 0;
#pragma unroll
    for (int k = 0; k < GPU_MAX_PREFIX_LETTERS; ++k) {
        if (k >= p.prefix) break;
        uint32_t const count = uint32_t(rest >> p.shifts[k]) & p.masks[k];
        allowed |= ((1u << count) - 1) << p.places[k];
    }
    uint32_t high = allowed;
#pragma unroll
    for (int n = 0; n < 5; ++n) high &= high - 1;
    Cells out;
    out.high = high;
    out.low = allowed ^ high;
    out.lanes = 1 << __popc(out.low);
    out.width = (int(uint32_t(rest >> p.last_shift)) & p.last_mask) + 1;
    out.shift = p.last_bits;
    out.after = after;
    out.offsets = offsets;
    out.same = false;
    return out;
}

// The runs of step `step`'s candidates past `from`, the pick of the step
// before it.
template <typename BagKey>
__device__ inline Cells cells_of(LeftoverParams<BagKey> const& p,
                                 Picked<BagKey> const& from, int step) {
    bool const prospects = step >= p.pair_steps;
    bool const same = p.order == nullptr &&
        prospects == (step - 1 >= p.pair_steps);
    Cells out = cells_for(p, from.rest, p.offsets[prospects],
                          same ? from.index + 1 : 0);
    out.same = same;
    return out;
}

// The first run of `cells` at or past the run of `from`'s pick, or NO_RUN.
template <typename BagKey>
__device__ inline uint32_t first_run(LeftoverParams<BagKey> const& p,
                                     Cells const& cells,
                                     Picked<BagKey> const& from) {
    if (!cells.same) return 0;
    return submask_from(p.bags[from.index].code >> 8, cells.high | cells.low);
}

// The sorted positions of run `run` past the rest's pick.
__device__ inline void run_range(Cells const& cells, uint32_t run, int* begin,
                                 int* end) {
    int const cell = int(run << cells.shift);
    *end = cells.offsets[cell + cells.width];
    *begin = min(max(cells.offsets[cell], cells.after), *end);
}

// A cursor at `chunk` of `cells` with `left` chunks left.
__device__ inline Cursor start_cursor(Cells const& cells, uint32_t chunk,
                                      int left) {
    return {chunk, left, 0, deposit(threadIdx.x & 31, cells.low), false,
            0, 0, 0, 0};
}

// A cursor at the chunk of the first run of `cells` at or past `from`'s
// pick's.
template <typename BagKey>
__device__ inline Cursor chunks_from(LeftoverParams<BagKey> const& p,
                                     Cells const& cells,
                                     Picked<BagKey> const& from) {
    uint32_t const run = first_run(p, cells, from);
    return start_cursor(cells, run & cells.high,
                        run == NO_RUN ? 0 : EVERY_CHUNK);
}

// A cursor at the first run of `cells` at or past `from`'s pick's.
template <typename BagKey>
__device__ inline RunCursor runs_from(LeftoverParams<BagKey> const& p,
                                      Cells const& cells,
                                      Picked<BagKey> const& from) {
    uint32_t const run = first_run(p, cells, from);
    return {run, run == NO_RUN ? 0 : EVERY_CHUNK, 0, 0};
}

// Moves `cursor` to the next chunk.
__device__ inline void next_chunk(Cells const& cells, Cursor* cursor) {
    cursor->chunk = next_submask(cursor->chunk, cells.high);
    cursor->left = cursor->chunk == 0 ? 0 : cursor->left - 1;
    cursor->position = 0;
    cursor->loaded = false;
}

// Gives each lane the next candidate past `cursor`, or -1 when the chunks
// run out first, and moves `cursor` past them. The warp calls it with the
// same `cells` and `cursor`.
__device__ int next_candidates(Cells const& cells, Cursor* cursor) {
    int const lane = threadIdx.x & 31;
    int out = -1;
    int filled = 0;
    while (filled < 32 && cursor->left > 0) {
        if (!cursor->loaded) {
            int begin = 0;
            int end = 0;
            if (lane < cells.lanes)
                run_range(cells, cursor->chunk | cursor->pattern, &begin,
                          &end);
            int sum = end - begin;
            for (int offset = 1; offset < 32; offset <<= 1) {
                int const below = __shfl_up_sync(~0u, sum, offset);
                if (lane >= offset) sum += below;
            }
            cursor->loaded = true;
            cursor->begin = begin;
            cursor->length = end - begin;
            cursor->sum = sum;
            cursor->total = __shfl_sync(~0u, sum, 31);
        }
        int const total = cursor->total;
        if (total != 0) {
            int const q = cursor->position + lane - filled;
            int at = 0;
            for (int step = 16; step > 0; step >>= 1)
                if (__shfl_sync(~0u, cursor->sum, at + step - 1) <= q)
                    at += step;
            int const at_begin = __shfl_sync(~0u, cursor->begin, at);
            int const at_start = __shfl_sync(~0u, cursor->sum, at) -
                __shfl_sync(~0u, cursor->length, at);
            if (lane >= filled && q < total) out = at_begin + q - at_start;
        }
        int const taken = min(total - cursor->position, 32 - filled);
        filled += taken;
        if (cursor->position + taken < total) {
            cursor->position += taken;
            break;
        }
        next_chunk(cells, cursor);
    }
    return out;
}

// Moves `cursor` to its next candidate; false when the runs run out.
__device__ inline bool next_candidate(Cells const& cells, RunCursor* cursor) {
    while (cursor->next >= cursor->end) {
        if (cursor->left == 0) return false;
        run_range(cells, cursor->run, &cursor->next, &cursor->end);
        cursor->run = next_submask(cursor->run, cells.high | cells.low);
        cursor->left = cursor->run == 0 ? 0 : cursor->left - 1;
    }
    return true;
}

// Adds the leftover numbered `number` to the register of `left` letters, as
// LeftoverSketch::add does.
template <typename BagKey>
__device__ inline void add_register(LeftoverParams<BagKey> const& p, int left,
                                    uint64_t number) {
    uint64_t const mixed = mix64(number + 1);
    uint64_t const rest =
        (mixed << GPU_HLL_BITS) | (uint64_t(1) << (GPU_HLL_BITS - 1));
    uint32_t const rank = uint32_t(__clzll((long long)rest) + 1);
    uint32_t* const slot = p.registers + (size_t(left) << GPU_HLL_BITS) +
        (mixed >> (64 - GPU_HLL_BITS));
    if (rank > __ldcg(slot)) atomicMax(slot, rank);
}

// Counts a combination for each lane of `group`, every one leaving `left`
// letters, each with its own `rows` and `number`. The group's lanes call
// it; its lowest adds the group's counts.
template <typename BagKey>
__device__ inline void add_combination(LeftoverParams<BagKey> const& p,
                                       WalkCounts const& c, unsigned group,
                                       int left, unsigned long long rows,
                                       uint64_t number) {
    unsigned long long sum = 0;
    for (unsigned rest = group; rest != 0; rest &= rest - 1)
        sum += __shfl_sync(group, rows, __ffs(int(rest)) - 1);
    if (int(threadIdx.x & 31) == __ffs(int(group)) - 1) {
        atomicAdd(&c.combinations[left], (unsigned long long)__popc(group));
        atomicAdd(&c.rows[left], sum);
    }
    add_register(p, left, number);
}

// The last step's candidates past `from`'s pick, one per lane, adding
// how many to every lane's `scanned`. With PAIR_USAGE, returns to every
// lane the sum of their PAIRS, w of `from`.
template <WalkCountType COUNT_TYPE, typename BagKey>
__device__ unsigned long long last_step(LeftoverParams<BagKey> const& p,
                                        WalkCounts const& c,
                                        Picked<BagKey> const& from,
                                        unsigned long long* scanned) {
    Cells const cells = cells_of(p, from, p.steps - 1);
    Cursor cursor = chunks_from(p, cells, from);
    Picked<BagKey> next;
    unsigned long long w = 0;
    while (cursor.left > 0) {
        int const i = next_candidates(cells, &cursor);
        *scanned += unsigned(__popc(__ballot_sync(~0u, i >= 0)));
        if (i < 0 || !pick<COUNT_TYPE>(p, from, i, &next)) continue;
        if constexpr (COUNT_TYPE == WalkCountType::LEFTOVERS) {
            add_combination(p, c, 1u << (threadIdx.x & 31), next.letters,
                            next.rows, next.number);
        } else {
            atomicAdd(&p.usage[i], from.rows);
            w += next.pairs;
        }
    }
    if constexpr (COUNT_TYPE == WalkCountType::PAIR_USAGE)
        for (int offset = 16; offset > 0; offset >>= 1)
            w += __shfl_xor_sync(~0u, w, offset);
    return w;
}

// The warp's next unit, or -1 past the last.
template <typename BagKey>
__device__ inline int take_unit(LeftoverParams<BagKey> const& p) {
    unsigned long long unit = 0;
    if ((threadIdx.x & 31) == 0) unit = atomicAdd(p.next_unit, 1ULL);
    unit = __shfl_sync(~0u, unit, 0);
    return unit < (unsigned long long)p.count ? int(unit) : -1;
}

// Counts unit `unit` finished, and adds the candidates the warp scanned in
// it, `first` of them below its first pick, to its work group; zeroes
// both. The warp calls it.
template <typename BagKey>
__device__ inline void finish_unit(LeftoverParams<BagKey> const& p, int unit,
                                   unsigned long long* first,
                                   unsigned long long* work) {
    if ((threadIdx.x & 31) == 0) {
        atomicAdd(&p.progress->done, 1ULL);
        if (*work != 0) {
            int const group = p.groups[unit];
            atomicAdd(&p.progress->first[group], *first);
            atomicAdd(&p.progress->work[group], *work);
        }
    }
    *first = 0;
    *work = 0;
}

// Picks the warp's unit, counting it; true when it fits and there are steps
// past it. With one step, counts its combination.
template <WalkCountType COUNT_TYPE, typename BagKey>
__device__ inline bool start_unit(LeftoverParams<BagKey> const& p,
                                  WalkCounts const& c, int unit,
                                  Picked<BagKey>* out) {
    Picked<BagKey> const root{p.bag, p.bag_number, 1, p.bag_letters, -1, 1,
                              0};
    if (!pick<COUNT_TYPE>(p, root, p.first_unit + unit, out)) return false;
    if ((threadIdx.x & 31) != 0) return p.steps > 1;
    if constexpr (COUNT_TYPE == WalkCountType::PAIR_USAGE) {
        if (p.steps == 1) atomicAdd(&p.usage[out->index], 1ULL);
        return p.steps > 1;
    }
    atomicAdd(&c.by_picks[0], 1ULL);
    if (p.steps > 1) return true;
    add_combination(p, c, 1u, out->letters, out->rows, out->number);
    return false;
}

template <WalkCountType COUNT_TYPE, typename BagKey>
__global__ void __launch_bounds__(WALK_THREADS)
walk_leftover_picks(LeftoverParams<BagKey> p) {
    constexpr bool LEFTOVERS = COUNT_TYPE == WalkCountType::LEFTOVERS;
    extern __shared__ __align__(16) unsigned char shared_bytes[];
    WalkCounts c{};
    if constexpr (LEFTOVERS) c = start_walk_counts(p, shared_bytes);
    int const lane = threadIdx.x & 31;
    int const last = p.steps - 1;
    Picked<BagKey> picked[GPU_MAX_PAIR_STEPS];
    Cells cells[GPU_MAX_PAIR_STEPS];
    RunCursor cursors[GPU_MAX_PAIR_STEPS];
    // With PAIR_USAGE, w of each pick so far, from its finished children.
    unsigned long long below[GPU_MAX_PAIR_STEPS] = {};
    auto const start = [&](int depth) {
        Picked<BagKey> const& from = picked[depth - 1];
        cells[depth] = cells_of(p, from, depth);
        cursors[depth] = runs_from(p, cells[depth], from);
    };
    auto const finish = [&](int depth, unsigned long long w) {
        if (w == 0) return;
        int const index = picked[depth].index;
        unsigned long long const above =
            depth > 0 ? picked[depth - 1].rows : 1;
        if (lane == 0) atomicAdd(&p.usage[index], above * w);
        if (depth > 0) below[depth - 1] += picked[depth].pairs * w;
    };
    // The candidates the warp scanned in its unit: below its first pick,
    // and in all.
    unsigned long long first = 0;
    unsigned long long work = 0;
    for (int unit; (unit = take_unit(p)) >= 0;
         finish_unit(p, unit, &first, &work)) {
        if (!start_unit<COUNT_TYPE>(p, c, unit, &picked[0])) continue;
        if constexpr (!LEFTOVERS) below[0] = 0;
        int depth = 1;
        if (depth < last) start(depth);
        while (depth > 0) {
            if (depth == last) {
                unsigned long long scanned = 0;
                unsigned long long const w = last_step<COUNT_TYPE>(
                    p, c, picked[depth - 1], &scanned);
                work += scanned;
                if (depth == 1) first += scanned;
                --depth;
                if constexpr (!LEFTOVERS) finish(depth, w);
                continue;
            }
            Picked<BagKey> const& from = picked[depth - 1];
            RunCursor& cursor = cursors[depth];
            bool found = false;
            while (next_candidate(cells[depth], &cursor)) {
                ++work;
                if (depth == 1) ++first;
                if (pick<COUNT_TYPE>(p, from, cursor.next++,
                                     &picked[depth])) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                --depth;
                if constexpr (!LEFTOVERS) finish(depth, below[depth]);
                continue;
            }
            if constexpr (LEFTOVERS) {
                if (lane == 0) atomicAdd(&c.by_picks[depth], 1ULL);
            } else {
                below[depth] = 0;
            }
            ++depth;
            if (depth < last) start(depth);
        }
    }
    if constexpr (LEFTOVERS) flush_walk_counts(p, c);
}

// The candidates below `first`, a unit's pick, and in `chunks` the chunks
// they are in, from `first_chunk` on; every lane gets the same.
template <typename BagKey>
__device__ unsigned long long first_candidates(LeftoverParams<BagKey> const& p,
                                               Picked<BagKey> const& first,
                                               int* first_chunk,
                                               int* chunks) {
    Cells const cells = cells_of(p, first, 1);
    Cursor cursor = chunks_from(p, cells, first);
    int const all = 1 << __popc(cells.high);
    *first_chunk = cursor.left > 0 ? int(extract(cursor.chunk, cells.high))
                                   : all;
    *chunks = all - *first_chunk;
    unsigned long long candidates = 0;
    for (; cursor.left > 0; next_chunk(cells, &cursor)) {
        if (int(threadIdx.x & 31) >= cells.lanes) continue;
        int begin = 0;
        int end = 0;
        run_range(cells, cursor.chunk | cursor.pattern, &begin, &end);
        candidates += (unsigned long long)(end - begin);
    }
    return warp_sum(candidates);
}

// Counts the candidates the walk scans below each unit's first pick, a
// warp a unit: writes the unit's work group to p.groups, and adds its
// count to totals[group].
template <typename BagKey>
__global__ void __launch_bounds__(WALK_THREADS)
count_first_candidates(LeftoverParams<BagKey> p, unsigned long long* totals) {
    int const lane = threadIdx.x & 31;
    int const warps = int(gridDim.x * blockDim.x / 32);
    Picked<BagKey> const root{p.bag, p.bag_number, 1, p.bag_letters, -1, 1,
                              0};
    for (int unit = int((blockIdx.x * blockDim.x + threadIdx.x) / 32);
         unit < p.count; unit += warps) {
        Picked<BagKey> first;
        unsigned long long candidates = 0;
        int first_chunk = 0;
        int chunks = 0;
        if (pick<WalkCountType::PAIR_USAGE>(p, root, p.first_unit + unit,
                                            &first))
            candidates = first_candidates(p, first, &first_chunk, &chunks);
        if (lane != 0) continue;
        int const group = work_group(candidates);
        p.groups[unit] = uint8_t(group);
        atomicAdd(&totals[group], candidates);
    }
}

// The memo of a list's PAIRS that fit each rest: 2^slot_bits slots of 4
// bytes when `narrow`, else 8, direct-mapped by the low bits of the rest's
// number mixed within its number_bits, each slot the mix's other bits above
// value_bits of the PAIRS + 1, 0 when empty; and the list's shortest bag.
struct Memo {
    void* slots;
    int slot_bits;
    int value_bits;
    int number_bits;
    bool narrow;
    int letters;
};

// The most letters a slab fixes.
constexpr int SLAB_LETTERS = 4;

// The table of pair sums a PAIR_USAGE_SUMS walk counts its last two picks
// from, a slab at a time: a slab is the rests with the same counts of the
// slab letters. `sums` is the slab's table, by a rest's number over the
// other letters: the PAIRS products of the left-out list's 2-sets that fit
// the rest. `counts` is, by slab, the walk's combinations in it. Then the
// slab letters' count and guard bits in a key, the slab's counts, each slab
// letter's key shift, count mask and stride in a slab's index, and the
// left-out list's shortest bag.
template <typename BagKey>
struct Slabs {
    unsigned long long* sums;
    unsigned long long* counts;
    BagKey mask;
    BagKey guards;
    BagKey slab;
    int letters;
    uint8_t shifts[SLAB_LETTERS];
    uint8_t masks[SLAB_LETTERS];
    uint32_t strides[SLAB_LETTERS];
    int shortest;
};

template <typename BagKey>
__device__ inline uint32_t slab_index(Slabs<BagKey> const& slabs, BagKey rest) {
    uint32_t index = 0;
#pragma unroll
    for (int k = 0; k < SLAB_LETTERS; ++k) {
        if (k >= slabs.letters) break;
        index += (uint32_t(rest >> slabs.shifts[k]) & slabs.masks[k]) *
            slabs.strides[k];
    }
    return index;
}

// The most letters the fit tables take one at a time.
constexpr int SPARSE_LETTERS = 16;

// Tables that give the PAIRS of a list's bags that fit a rest without a
// scan. The bag's letters are split in two: the dense letters, whose counts
// number a rest, and the sparse letters. A bag fits a rest when it fits in
// the dense letters and exceeds it in no sparse letter, so by inclusion and
// exclusion the PAIRS that fit are
//
//     fits[n] - sum over e of one(e) + sum over e1 < e2 of two(e1, e2) - ...
//
// with n the rest's number, one(e) the PAIRS of the bags that fit in the
// dense letters and exceed the rest in sparse letter e, and two(e1, e2) of
// those that exceed it in both. Each is one entry: `one` at one_base[e] +
// n * caps[e] + r, `two` at two_base[e2 * (e2 - 1) / 2 + e1] + (n *
// caps[e1] + r1) * caps[e2] + r2, r = caps[e] - 1 - the rest's count of e,
// and caps[e] the most of e a bag has. The sum stops at two; a bag that
// exceeds the rest in j >= 3 sparse letters is then counted (j-1)(j-2)/2
// times too many, and those bags are listed by three of their sparse
// letters: `three` holds, for letters e1 < e2 < e3 and counts u1, u2 of the
// first two, the bags with those counts and some of e3, most of e3 first,
// each with its count of e3 as its code, from three_starts[three_base[t] +
// (u1 - 1) * caps[e2] + (u2 - 1)], t the triple's number. Then each sparse
// letter's key shift and count mask, and the dense letters' guard bits.
template <typename BagKey>
struct FitTables {
    uint32_t const* fits;
    uint32_t const* one;
    uint32_t const* two;
    // Unless null, the sum's third term: the PAIRS of the bags that exceed
    // the rest in three sparse letters, at triple_base[t] + ((n * caps[e1]
    // + r1) * caps[e2] + r2) * caps[e3] + r3. The sum then stops at three,
    // a bag that exceeds the rest in j >= 4 sparse letters is counted
    // (j-1)(j-2)(j-3)/6 times too few, and `three` lists those.
    uint32_t const* triple;
    uint64_t const* triple_base;
    WalkBag<BagKey> const* three;
    int const* three_starts;
    uint64_t const* two_base;
    uint32_t const* three_base;
    int letters;
    uint8_t shifts[SPARSE_LETTERS];
    uint8_t masks[SPARSE_LETTERS];
    uint8_t caps[SPARSE_LETTERS];
    uint32_t one_base[SPARSE_LETTERS];
    BagKey dense_guards;
    // Nonzero once the walk is to stop: its counts are then dropped.
    int const* stop;
};

__device__ inline int key_bits_set(Key64 key) { return __popcll(key); }

__device__ inline int key_bits_set(Key128 key) {
    return __popcll(Key64(key)) + __popcll(Key64(key >> 64));
}

// What the bags that exceed `rest` in sparse letters e1 < e2 < e3 of `t` and
// more, and fit it in the dense letters, are miscounted by in the sum
// through `terms` terms, 2 or 3: the bags whose first three such letters
// these are. The warp calls it with the same arguments and walks each
// bucket together; every lane gets the sum.
template <typename BagKey>
__device__ unsigned long long excess_three(FitTables<BagKey> const& t,
                                           LeftoverParams<BagKey> const& p,
                                           BagKey rest, int e1, int e2,
                                           int e3, int terms,
                                           unsigned long long* visited) {
    int const lane = threadIdx.x & 31;
    int const r1 = int(uint32_t(rest >> t.shifts[e1]) & t.masks[e1]);
    int const r2 = int(uint32_t(rest >> t.shifts[e2]) & t.masks[e2]);
    int const r3 = int(uint32_t(rest >> t.shifts[e3]) & t.masks[e3]);
    int const cap1 = t.caps[e1];
    int const cap2 = t.caps[e2];
    BagKey const pool = rest | p.bag_guards;
    BagKey const guard1 = BagKey(1) << (t.shifts[e1] + __popc(t.masks[e1]));
    BagKey const guard2 = BagKey(1) << (t.shifts[e2] + __popc(t.masks[e2]));
    BagKey const guard3 = BagKey(1) << (t.shifts[e3] + __popc(t.masks[e3]));
    uint32_t const base =
        t.three_base[e3 * (e3 - 1) * (e3 - 2) / 6 + e2 * (e2 - 1) / 2 + e1];
    unsigned long long excess = 0;
    unsigned long long count = 0;
    for (int u1 = r1 + 1; u1 <= cap1; ++u1) {
        if (__any_sync(~0u, __ldcv(t.stop) != 0)) return 0;
        for (int u2 = r2 + 1; u2 <= cap2; ++u2) {
            int const* const starts =
                t.three_starts + base + (u1 - 1) * cap2 + (u2 - 1);
            int const end = starts[1];
            for (int b = starts[0] + lane;; b += 32) {
                bool live = b < end;
                WalkBag<BagKey> bag;
                if (live) {
                    bag = t.three[b];
                    live = int(bag.code) > r3;
                }
                if (live) {
                    ++count;
                    BagKey const over = ~(pool - bag.key) & p.bag_guards;
                    BagKey const first = over & (0 - over);
                    BagKey const second =
                        (over ^ first) & (0 - (over ^ first));
                    BagKey const third = (over ^ first ^ second) &
                        (0 - (over ^ first ^ second));
                    if ((over & t.dense_guards) == 0 && first == guard1 &&
                        second == guard2 && third == guard3) {
                        unsigned long long const j = key_bits_set(over);
                        unsigned long long const times = terms == 2
                            ? (j - 1) * (j - 2) / 2
                            : (j - 1) * (j - 2) * (j - 3) / 6;
                        excess += times * bag.pairs;
                    }
                }
                if (__ballot_sync(~0u, live) != ~0u) break;
            }
        }
    }
    *visited += count;
    return warp_sum(excess);
}

// The most bucket walks one lookup hands to the warp.
constexpr int WALKS_MOST = 5;

// `rest`'s entry of the third term of `t` for sparse letters e1 < e2 < e3.
template <typename BagKey>
__device__ inline uint32_t triple_sum(FitTables<BagKey> const& t, BagKey rest,
                                      uint64_t number, int e1, int e2,
                                      int e3) {
    uint64_t const cap1 = t.caps[e1];
    uint64_t const cap2 = t.caps[e2];
    uint64_t const cap3 = t.caps[e3];
    uint64_t const r1 =
        cap1 - 1 - (uint32_t(rest >> t.shifts[e1]) & t.masks[e1]);
    uint64_t const r2 =
        cap2 - 1 - (uint32_t(rest >> t.shifts[e2]) & t.masks[e2]);
    uint64_t const r3 =
        cap3 - 1 - (uint32_t(rest >> t.shifts[e3]) & t.masks[e3]);
    return t.triple[
        t.triple_base[e3 * (e3 - 1) * (e3 - 2) / 6 + e2 * (e2 - 1) / 2 + e1] +
        ((number * cap1 + r1) * cap2 + r2) * cap3 + r3];
}

// The PAIRS of the bags of `t` that fit `rest`, numbered `number`, but for
// the bucket walks it needs: those are added to `walks`, 12 bits each (e1,
// e2 and e3 from the low bits), and counted in `walk_count`; past
// WALKS_MOST the count is WALKS_MOST + 1 and the lookup is void.
template <typename BagKey>
__device__ long long table_fits(FitTables<BagKey> const& t, BagKey rest,
                                uint64_t number, uint64_t* walks,
                                int* walk_count) {
    long long fitting = t.fits[number];
    uint32_t exceeded = 0;
#pragma unroll 1
    for (int e = 0; e < t.letters; ++e) {
        int const count = int(uint32_t(rest >> t.shifts[e]) & t.masks[e]);
        int const cap = t.caps[e];
        if (count >= cap) continue;
        uint32_t const sum =
            t.one[t.one_base[e] + number * cap + (cap - 1 - count)];
        fitting -= sum;
        if (sum != 0) exceeded |= 1u << e;
    }
    if ((exceeded & (exceeded - 1)) == 0) return fitting;
    // above[e1]: the e2 past e1 some bag exceeds the rest in along with it.
    uint16_t above[SPARSE_LETTERS];
    bool three = false;
    for (uint32_t m1 = exceeded; m1 != 0; m1 &= m1 - 1) {
        int const e1 = __ffs(int(m1)) - 1;
        int const cap1 = t.caps[e1];
        uint64_t const row = number * cap1 + (cap1 - 1 -
            int(uint32_t(rest >> t.shifts[e1]) & t.masks[e1]));
        uint32_t both = 0;
        for (uint32_t m2 = m1 & (m1 - 1); m2 != 0; m2 &= m2 - 1) {
            int const e2 = __ffs(int(m2)) - 1;
            int const cap2 = t.caps[e2];
            uint32_t const sum = t.two[
                t.two_base[e2 * (e2 - 1) / 2 + e1] + row * cap2 +
                (cap2 - 1 - int(uint32_t(rest >> t.shifts[e2]) & t.masks[e2]))];
            fitting += sum;
            if (sum != 0) both |= 1u << e2;
        }
        above[e1] = uint16_t(both);
        three = three || (both & (both - 1)) != 0;
    }
    if (!three) return fitting;
    for (uint32_t m1 = exceeded; m1 != 0; m1 &= m1 - 1) {
        int const e1 = __ffs(int(m1)) - 1;
        for (uint32_t m2 = above[e1]; m2 != 0; m2 &= m2 - 1) {
            int const e2 = __ffs(int(m2)) - 1;
            for (uint32_t m3 = above[e1] & above[e2]; m3 != 0; m3 &= m3 - 1) {
                int const e3 = __ffs(int(m3)) - 1;
                uint64_t const walk = uint64_t(e1 | e2 << 4 | e3 << 8);
                if (t.triple == nullptr) {
                    if (*walk_count < WALKS_MOST)
                        *walks |= walk << (12 * *walk_count);
                    if (*walk_count <= WALKS_MOST) ++*walk_count;
                    continue;
                }
                uint32_t const sum = triple_sum(t, rest, number, e1, e2, e3);
                fitting -= sum;
                if (sum == 0) continue;
                // A bag exceeds the rest in a fourth letter too only if
                // every three of the four have a sum.
                bool four = false;
                for (uint32_t m4 = above[e1] & above[e2] & above[e3];
                     m4 != 0 && !four; m4 &= m4 - 1) {
                    int const e4 = __ffs(int(m4)) - 1;
                    four = triple_sum(t, rest, number, e1, e2, e4) != 0 &&
                        triple_sum(t, rest, number, e1, e3, e4) != 0 &&
                        triple_sum(t, rest, number, e2, e3, e4) != 0;
                }
                if (!four) continue;
                if (*walk_count < WALKS_MOST)
                    *walks |= walk << (12 * *walk_count);
                if (*walk_count <= WALKS_MOST) ++*walk_count;
            }
        }
    }
    return fitting;
}

// What walk_pair_usage takes beyond the walk's: the tasks, each a run of
// one unit's chunks below its first pick, as the next one, how many there
// are, each unit's first, then their count, and each unit's chunks a task;
// and with PAIR_USAGE_MEMO, the memo of the last step's list.
template <typename BagKey>
struct UsageParams {
    LeftoverParams<BagKey> walk;
    unsigned long long* next_task;
    unsigned long long tasks;
    unsigned long long task_stride;
    // Nonzero once the walk is to stop taking tasks.
    int const* stop;
    unsigned long long const* task_starts;
    int const* task_chunks;
    Memo memo;
    // With RESTS, the set of rest numbers + 1, open addressed, 0 while
    // empty; its slots less 1; and how many rests found no slot.
    unsigned long long* set;
    uint64_t set_mask;
    unsigned long long* set_dropped;
    // With RESTS, the words a slot of the set takes: 2 when the word after
    // a rest's number counts its rows.
    int set_stride;
    // With TOP_ROWS, `set` holds the wanted rests' numbers + 1 and
    // top_values each one's value: 1 in its low two bits for a rest whose
    // rows are all wanted, 2 for one whose rows are wanted only in units up
    // to top_last_unit. Without top_rows, the walk adds each unit's rows of
    // the second kind to unit_ties; with it, it writes each wanted row's
    // picks' places and then its rest's value to top_rows, top_capacity
    // rows at most, and counts them in top_count.
    uint32_t const* top_values;
    uint32_t* top_rows;
    unsigned long long top_capacity;
    unsigned long long* top_count;
    uint32_t top_last_unit;
    unsigned long long* unit_ties;
    // With the SUM types and PAIR_USAGE_SUMS.
    Slabs<BagKey> slabs;
    // With PAIR_USAGE_TABLES, the last step's list's.
    FitTables<BagKey> tables;
};

// Adds the rest numbered `number` to the walk's set.
template <typename BagKey>
__device__ inline void add_rest(UsageParams<BagKey> const& q,
                                uint64_t number) {
    unsigned long long const key = number + 1;
    uint64_t slot = mix64(key) & q.set_mask;
    for (int probe = 0; probe < SET_PROBES; ++probe) {
        unsigned long long* const at = q.set + slot * q.set_stride;
        unsigned long long found = *at;
        if (found == 0) found = atomicCAS(at, 0ULL, key);
        if (found == 0 || found == key) {
            if (q.set_stride == 2) atomicAdd(at + 1, 1ULL);
            return;
        }
        slot = (slot + 1) & q.set_mask;
    }
    atomicAdd(q.set_dropped, 1ULL);
}

// The value of the rest numbered `number` in the walk's wanted rests, or 0.
template <typename BagKey>
__device__ inline uint32_t top_value(UsageParams<BagKey> const& q,
                                     uint64_t number) {
    unsigned long long const key = number + 1;
    for (uint64_t slot = mix64(key) & q.set_mask;;
         slot = (slot + 1) & q.set_mask) {
        unsigned long long const found = __ldcg(&q.set[slot]);
        if (found == key) return __ldcg(&q.top_values[slot]);
        if (found == 0) return 0;
    }
}

// `number` mixed one to one within `bits` bits.
__device__ inline uint64_t mix_within(uint64_t number, int bits) {
    uint64_t const mask = bits == 64 ? ~uint64_t(0)
                                     : (uint64_t(1) << bits) - 1;
    int const half = (bits + 1) / 2;
    number ^= number >> half;
    number = (number * 0xbf58476d1ce4e5b9ULL) & mask;
    number ^= number >> half;
    number = (number * 0x94d049bb133111ebULL) & mask;
    return number ^ (number >> half);
}

// The memo's PAIRS for the rest mixed to `hash`; false when it has none.
__device__ inline bool memo_get(Memo const& memo, uint64_t hash,
                                unsigned long long* out) {
    uint64_t const slot = hash & ((uint64_t(1) << memo.slot_bits) - 1);
    uint64_t const entry = memo.narrow
        ? uint64_t(static_cast<uint32_t const*>(memo.slots)[slot])
        : static_cast<uint64_t const*>(memo.slots)[slot];
    uint64_t const value = entry & ((uint64_t(1) << memo.value_bits) - 1);
    *out = value - 1;
    return value != 0 &&
        entry >> memo.value_bits == hash >> memo.slot_bits;
}

__device__ inline void memo_put(Memo const& memo, uint64_t hash,
                                unsigned long long pairs) {
    uint64_t const slot = hash & ((uint64_t(1) << memo.slot_bits) - 1);
    uint64_t const entry =
        (hash >> memo.slot_bits) << memo.value_bits | (pairs + 1);
    if (memo.narrow)
        static_cast<uint32_t*>(memo.slots)[slot] = uint32_t(entry);
    else
        static_cast<uint64_t*>(memo.slots)[slot] = entry;
}

// A warp's walk at one depth, in shared memory: the depth's pick, the runs
// of the candidates below it, where the walk of the depth's own candidates
// is, and w of the pick so far.
template <typename BagKey>
struct Level {
    Picked<BagKey> picked;
    Cells cells;
    RunCursor cursor;
    unsigned long long below;
};

__device__ inline Key64 shuffle_key(Key64 key, int lane) {
    return __shfl_sync(~0u, key, lane);
}

__device__ inline Key128 shuffle_key(Key128 key, int lane) {
    Key64 const low = __shfl_sync(~0u, Key64(key), lane);
    Key64 const high = __shfl_sync(~0u, Key64(key >> 64), lane);
    return Key128(high) << 64 | low;
}

// The PAIRS of the bags of the list at `offsets` that fit `rest`, adding the
// candidates scanned to every lane's `scanned`. The warp calls it with the
// same `rest`.
template <typename BagKey>
__device__ unsigned long long fitting_pairs(LeftoverParams<BagKey> const& p,
                                            int const* offsets, BagKey rest,
                                            unsigned long long* scanned) {
    Cells const cells = cells_for(p, rest, offsets, 0);
    Cursor cursor = start_cursor(cells, 0, EVERY_CHUNK);
    BagKey const pool = rest | p.bag_guards;
    unsigned long long sum = 0;
    unsigned long long count = 0;
    while (cursor.left > 0) {
        int const i = next_candidates(cells, &cursor);
        if (i < 0) continue;
        ++count;
        WalkBag<BagKey> const bag = p.bags[i];
        BagKey left;
        if (fits(pool, bag.key, p.bag_guards, &left)) sum += bag.pairs;
    }
    *scanned += warp_sum(count);
    return warp_sum(sum);
}

// The walk's last step: the candidates of `cursor` in `cells`, below the
// picks of `levels` before `depth`, one per lane, adding how many to every
// lane's `scanned`. Adds to each one's usage the picks' rows times its
// count, and returns to every lane the sum of their PAIRS times their
// counts, w of the pick above. A candidate's count is 1, or with
// PAIR_USAGE_MEMO, the PAIRS of the last step's list that fit what it
// leaves and aren't the picks', whose scans add to `memo_scanned`, with
// the lookups and misses added to lane 0's `stats`.
template <WalkCountType COUNT_TYPE, typename BagKey>
__device__ unsigned long long usage_step(UsageParams<BagKey> const& q,
                                         Level<BagKey> const* levels,
                                         int depth, Cells const& cells,
                                         Cursor cursor,
                                         unsigned long long* scanned,
                                         unsigned long long* memo_scanned,
                                         unsigned long long* stats) {
    constexpr bool MEMO = COUNT_TYPE == WalkCountType::PAIR_USAGE_MEMO;
    constexpr bool SUMS = COUNT_TYPE == WalkCountType::PAIR_USAGE_SUMS;
    constexpr bool TABLES = COUNT_TYPE == WalkCountType::PAIR_USAGE_TABLES;
    LeftoverParams<BagKey> const& p = q.walk;
    int const lane = threadIdx.x & 31;
    Picked<BagKey> const from = levels[depth - 1].picked;
    bool const list = depth >= p.pair_steps;
    Picked<BagKey> next;
    unsigned long long w = 0;
    unsigned long long count = 0;
    unsigned long long ties = 0;
    while (cursor.left > 0) {
        int const i = next_candidates(cells, &cursor);
        if (i >= 0) ++count;
        bool const fit = i >= 0 && pick<COUNT_TYPE>(p, from, i, &next);
        if constexpr (COUNT_TYPE == WalkCountType::RESTS) {
            if (fit && in_order(p, from, next, depth))
                add_rest(q, next.number);
        } else if constexpr (COUNT_TYPE == WalkCountType::TOP_ROWS) {
            if (!fit || !in_order(p, from, next, depth)) continue;
            uint32_t const value =
                q.top_values == nullptr ? 2 : top_value(q, next.number);
            if (value == 0) continue;
            bool const tie = (value & 3) == 2;
            if (q.top_rows == nullptr) {
                if (tie) ++ties;
                continue;
            }
            if (tie && levels[0].picked.place > q.top_last_unit) continue;
            unsigned long long const row = atomicAdd(q.top_count, 1ULL);
            if (row >= q.top_capacity) continue;
            uint32_t* const out = q.top_rows + row * (p.steps + 1);
            for (int d = 0; d < depth; ++d) out[d] = levels[d].picked.place;
            out[depth] = next.place;
            out[p.steps] = value;
        } else if constexpr (COUNT_TYPE == WalkCountType::SUM_SLABS) {
            bool const wanted =
                fit && next.letters >= 2 * q.slabs.shortest;
            uint32_t const slab =
                wanted ? slab_index(q.slabs, next.rest) : ~0u;
            unsigned const same = __match_any_sync(~0u, slab);
            if (wanted && lane == __ffs(int(same)) - 1)
                atomicAdd(&q.slabs.counts[slab],
                          (unsigned long long)__popc(same));
        } else if constexpr (COUNT_TYPE == WalkCountType::SUM_SCATTER) {
            BagKey left;
            if (fit &&
                fits(q.slabs.slab | q.slabs.guards,
                     (p.bag - next.rest) & q.slabs.mask, q.slabs.guards,
                     &left))
                atomicAdd(&q.slabs.sums[p.bag_number - next.number],
                          next.rows);
        } else if constexpr (SUMS) {
            bool const wanted =
                fit && next.letters >= 2 * q.slabs.shortest &&
                (next.rest & q.slabs.mask) == q.slabs.slab;
            unsigned long long sets = 0;
            if (wanted) sets = __ldcg(&q.slabs.sums[next.number]);
            BagKey const pool = next.rest | p.bag_guards;
            BagKey above = p.bag;
            unsigned scans = 0;
            for (int m = 0; m <= depth; ++m) {
                BagKey const after =
                    m < depth ? levels[m].picked.rest : next.rest;
                unsigned long long const pairs =
                    m < depth ? levels[m].picked.pairs : next.pairs;
                BagKey const key = above - after;
                above = after;
                BagKey left = 0;
                bool const again = wanted && sets != 0 &&
                    (m >= p.pair_steps) == list &&
                    fits(pool, key, p.bag_guards, &left);
                unsigned const todo_all = __ballot_sync(~0u, again);
                scans += __popc(todo_all);
                unsigned long long fitting = 0;
                for (unsigned todo = todo_all; todo != 0; todo &= todo - 1) {
                    int const source = __ffs(int(todo)) - 1;
                    unsigned long long const all = fitting_pairs(
                        p, p.offsets[list], shuffle_key(left, source),
                        memo_scanned);
                    if (lane == source) fitting = all;
                }
                if (!again) continue;
                BagKey const pool_left = left | p.bag_guards;
                BagKey over = p.bag;
                for (int y = 0; y <= m; ++y) {
                    BagKey const under =
                        y < depth ? levels[y].picked.rest : next.rest;
                    unsigned long long const pairs_y =
                        y < depth ? levels[y].picked.pairs : next.pairs;
                    BagKey unused;
                    if ((y >= p.pair_steps) == list &&
                        fits(pool_left, over - under, p.bag_guards, &unused))
                        fitting -= pairs_y;
                    over = under;
                }
                sets -= pairs * fitting;
            }
            if (lane == 0) {
                stats[0] += __popc(__ballot_sync(~0u, wanted));
                stats[1] += scans;
            } else {
                __ballot_sync(~0u, wanted);
            }
            if (!wanted || sets == 0) continue;
            atomicAdd(&p.usage[i], from.rows * sets);
            w += next.pairs * sets;
        } else if constexpr (!MEMO && !TABLES) {
            if (!fit) continue;
            atomicAdd(&p.usage[i], from.rows);
            w += next.pairs;
        } else {
            unsigned long long fitting = 0;
            bool const wanted = fit && next.letters >= q.memo.letters;
            if constexpr (TABLES) {
                unsigned long long visited = 0;
                uint64_t walks = 0;
                int walk_count = 0;
                long long sum = 0;
                if (wanted)
                    sum = table_fits(q.tables, next.rest, next.number,
                                     &walks, &walk_count);
                int const terms = q.tables.triple == nullptr ? 2 : 3;
                unsigned triangles = 0;
                unsigned const walking = __ballot_sync(~0u, walk_count != 0);
                // A walk told to stop drops its counts: no more walks.
                if (walking != 0 && __any_sync(~0u, __ldcv(q.stop) != 0))
                    break;
                for (unsigned todo = walking; todo != 0; todo &= todo - 1) {
                    int const source = __ffs(int(todo)) - 1;
                    BagKey const rest = shuffle_key(next.rest, source);
                    uint64_t const from_walks =
                        __shfl_sync(~0u, walks, source);
                    int const from_count =
                        __shfl_sync(~0u, walk_count, source);
                    triangles += from_count;
                    if (from_count > WALKS_MOST) {
                        unsigned long long const all = fitting_pairs(
                            p, p.offsets[list], rest, memo_scanned);
                        if (lane == source) sum = (long long)all;
                        continue;
                    }
                    for (int k = 0; k < from_count; ++k) {
                        uint32_t const walk =
                            uint32_t(from_walks >> (12 * k));
                        long long const excess = (long long)excess_three(
                            q.tables, p, rest, walk & 15, walk >> 4 & 15,
                            walk >> 8 & 15, terms, &visited);
                        if (lane == source)
                            sum += terms == 2 ? -excess : excess;
                    }
                }
                fitting = (unsigned long long)sum;
                *memo_scanned += warp_sum(visited);
                if (lane == 0) {
                    stats[0] += __popc(__ballot_sync(~0u, wanted));
                    stats[1] += triangles;
                } else {
                    __ballot_sync(~0u, wanted);
                }
            } else {
                uint64_t const hash =
                    mix_within(next.number, q.memo.number_bits);
                bool const miss =
                    wanted && !memo_get(q.memo, hash, &fitting);
                unsigned const missed = __ballot_sync(~0u, miss);
                if (lane == 0) {
                    stats[0] += __popc(__ballot_sync(~0u, wanted));
                    stats[1] += __popc(missed);
                } else {
                    __ballot_sync(~0u, wanted);
                }
                // A walk told to stop drops its counts: no more scans.
                if (missed != 0 && __any_sync(~0u, __ldcv(q.stop) != 0))
                    break;
                for (unsigned todo = missed; todo != 0; todo &= todo - 1) {
                    int const source = __ffs(int(todo)) - 1;
                    unsigned long long const all = fitting_pairs(
                        p, p.offsets[list], shuffle_key(next.rest, source),
                        memo_scanned);
                    if (lane != source) continue;
                    fitting = all;
                    memo_put(q.memo, hash, all);
                }
            }
            if (!wanted || fitting == 0) continue;
            BagKey const pool = next.rest | p.bag_guards;
            BagKey above = p.bag;
            BagKey left;
            for (int d = 0; d < depth; ++d) {
                Picked<BagKey> const& at = levels[d].picked;
                if ((d >= p.pair_steps) == list &&
                    fits(pool, above - at.rest, p.bag_guards, &left))
                    fitting -= at.pairs;
                above = at.rest;
            }
            if (fits(pool, from.rest - next.rest, p.bag_guards, &left))
                fitting -= next.pairs;
            if (fitting == 0) continue;
            atomicAdd(&p.usage[i], from.rows * fitting);
            w += next.pairs * fitting;
        }
    }
    if constexpr (COUNT_TYPE == WalkCountType::TOP_ROWS) {
        ties = warp_sum(ties);
        if (lane == 0 && ties != 0)
            atomicAdd(&q.unit_ties[levels[0].picked.place], ties);
    }
    *scanned += warp_sum(count);
    return warp_sum(w);
}

// The unit of task `task`, at or past `unit`.
template <typename BagKey>
__device__ inline int task_unit(UsageParams<BagKey> const& q,
                                unsigned long long task, int unit) {
    int low = unit;
    int high = q.walk.count;
    while (high - low > 1) {
        int const mid = (low + high) / 2;
        if (q.task_starts[mid] <= task)
            low = mid;
        else
            high = mid;
    }
    return low;
}

// Walks the tasks, each warp taking the next until none are left: every
// combination of the walk's steps under the task's chunks, counted as
// usage_step counts its last pick.
template <WalkCountType COUNT_TYPE, typename BagKey>
__global__ void __launch_bounds__(WALK_THREADS)
walk_pair_usage(UsageParams<BagKey> q) {
    extern __shared__ __align__(16) unsigned char shared_bytes[];
    LeftoverParams<BagKey> const& p = q.walk;
    int const lane = threadIdx.x & 31;
    int const last = p.steps - 1;
    Level<BagKey>* const levels =
        reinterpret_cast<Level<BagKey>*>(shared_bytes) +
        size_t(threadIdx.x >> 5) * p.steps;
    Picked<BagKey> const root{p.bag, p.bag_number, 1, p.bag_letters, -1, 1,
                              0};
    auto const finish = [&](int depth, unsigned long long w) {
        if (w == 0 || lane != 0) return;
        Picked<BagKey> const& at = levels[depth].picked;
        unsigned long long const above =
            depth > 0 ? levels[depth - 1].picked.rows : 1;
        atomicAdd(&p.usage[at.index], above * w);
        if (depth > 0) levels[depth - 1].below += at.pairs * w;
    };
    int unit = -1;
    int unit_first = 0;
    unsigned long long unit_end = 0;
    while (true) {
        unsigned long long task = 0;
        if (lane == 0) {
            task = atomicAdd(q.next_task, 1ULL) * q.task_stride;
            if (__ldcv(q.stop) != 0) task = q.tasks;
        }
        task = __shfl_sync(~0u, task, 0);
        if (task >= q.tasks) return;
        if (task >= unit_end) {
            unit = task_unit(q, task, max(unit, 0));
            unit_end = q.task_starts[unit + 1];
            Picked<BagKey> first;
            pick<COUNT_TYPE>(p, root, unit_bag(p, unit), &first);
            Cells const cells = cells_of(p, first, 1);
            unit_first = int(extract(first_run(p, cells, first) & cells.high,
                                     cells.high));
            if (lane == 0) {
                levels[0].picked = first;
                levels[0].cells = cells;
            }
        }
        Cells const top = levels[0].cells;
        int const size = q.task_chunks[unit];
        int const begin =
            unit_first + int(task - q.task_starts[unit]) * size;
        int const chunks = min(size, (1 << __popc(top.high)) - begin);
        uint32_t const chunk = deposit(uint32_t(begin), top.high);
        unsigned long long first = 0;
        unsigned long long work = 0;
        unsigned long long memo_work = 0;
        unsigned long long stats[2] = {};
        if (lane == 0) {
            levels[0].below = 0;
            if (last > 1) levels[1].cursor = {chunk, chunks * top.lanes, 0, 0};
        }
        int depth = 1;
        while (depth > 0) {
            int stopped = 0;
            if (lane == 0) stopped = __ldcv(q.stop);
            if (__shfl_sync(~0u, stopped, 0) != 0) return;
            Cells const cells = levels[depth - 1].cells;
            if (depth == last) {
                unsigned long long scanned = 0;
                unsigned long long const w = usage_step<COUNT_TYPE>(
                    q, levels, depth, cells,
                    depth == 1 ? start_cursor(cells, chunk, chunks)
                               : chunks_from(p, cells,
                                             levels[depth - 1].picked),
                    &scanned, &memo_work, stats);
                work += scanned;
                if (depth == 1) first += scanned;
                --depth;
                finish(depth, w);
                continue;
            }
            Picked<BagKey> const from = levels[depth - 1].picked;
            RunCursor cursor = levels[depth].cursor;
            Picked<BagKey> next;
            bool found = false;
            while (next_candidate(cells, &cursor)) {
                ++work;
                if (depth == 1) ++first;
                if (pick<COUNT_TYPE>(p, from, cursor.next++, &next) &&
                    in_order(p, from, next, depth)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                --depth;
                finish(depth, levels[depth].below);
                continue;
            }
            Cells const under = cells_of(p, next, depth + 1);
            RunCursor const start = runs_from(p, under, next);
            if (lane == 0) {
                levels[depth].cursor = cursor;
                levels[depth].picked = next;
                levels[depth].cells = under;
                levels[depth].below = 0;
                if (depth + 1 < last) levels[depth + 1].cursor = start;
            }
            ++depth;
        }
        if (lane == 0) {
            atomicAdd(&p.progress->tasks, 1ULL);
            if (task + 1 == unit_end) atomicAdd(&p.progress->done, 1ULL);
            int const group = p.groups[unit];
            atomicAdd(&p.progress->first[group], first);
            atomicAdd(&p.progress->work[group], work + memo_work);
            if constexpr (COUNT_TYPE == WalkCountType::PAIR_USAGE_MEMO ||
                          COUNT_TYPE == WalkCountType::PAIR_USAGE_SUMS ||
                          COUNT_TYPE == WalkCountType::PAIR_USAGE_TABLES) {
                atomicAdd(&p.progress->lookups, stats[0]);
                atomicAdd(&p.progress->misses, stats[1]);
                atomicAdd(&p.progress->miss_work, memo_work);
            }
        }
    }
}

// Counts the candidates the walk scans below each unit's first pick, a
// warp a unit: writes the unit's work group to p.groups, adds its count to
// totals[group], and splits its chunks into tasks of about `target`
// candidates: its chunks a task to task_chunks and its tasks to task_counts.
template <typename BagKey>
__global__ void __launch_bounds__(WALK_THREADS)
count_unit_tasks(LeftoverParams<BagKey> p, unsigned long long* totals,
                 int* task_chunks, unsigned long long* task_counts,
                 int target) {
    int const lane = threadIdx.x & 31;
    int const warps = int(gridDim.x * blockDim.x / 32);
    Picked<BagKey> const root{p.bag, p.bag_number, 1, p.bag_letters, -1, 1,
                              0};
    for (int unit = int((blockIdx.x * blockDim.x + threadIdx.x) / 32);
         unit < p.count; unit += warps) {
        Picked<BagKey> first;
        unsigned long long candidates = 0;
        int first_chunk = 0;
        int chunks = 0;
        if (pick<WalkCountType::PAIR_USAGE>(p, root, unit_bag(p, unit),
                                            &first))
            candidates = first_candidates(p, first, &first_chunk, &chunks);
        if (lane != 0) continue;
        int const group = work_group(candidates);
        p.groups[unit] = uint8_t(group);
        atomicAdd(&totals[group], candidates);
        unsigned long long size = 0;
        unsigned long long tasks = 0;
        if (candidates != 0) {
            size = max(((unsigned long long)chunks * target + candidates -
                        1) / candidates,
                       1ULL);
            tasks = (chunks + size - 1) / size;
        }
        task_chunks[unit] = int(size);
        task_counts[unit] = tasks;
    }
}

// Sums `table` along one letter: each of `lines` runs of `size` entries
// `stride` apart becomes its running sums.
template <typename Entry>
__global__ void sum_along(Entry* table, unsigned long long lines,
                          unsigned long long stride, int size) {
    unsigned long long const line =
        (unsigned long long)blockIdx.x * blockDim.x + threadIdx.x;
    if (line >= lines) return;
    unsigned long long const high = line / stride;
    Entry* at = table + high * stride * size + (line - high * stride);
    Entry sum = *at;
    for (int v = 1; v < size; ++v) {
        at += stride;
        sum += *at;
        *at = sum;
    }
}

// Adds values[k] to table[indexes[k]] for each of `count` k.
__global__ void scatter_add(uint32_t* table, uint64_t const* indexes,
                            uint32_t const* values, unsigned long long count) {
    unsigned long long const k =
        (unsigned long long)blockIdx.x * blockDim.x + threadIdx.x;
    if (k < count) atomicAdd(&table[indexes[k]], values[k]);
}

// The device blocks a kernel of WALK_THREADS threads and `shared` bytes
// fills the GPU with.
template <typename Kernel>
int walk_grid(Kernel kernel, size_t shared) {
    int device = 0;
    int sms = 0;
    int per_sm = 0;
    check(cudaGetDevice(&device));
    check(cudaDeviceGetAttribute(&sms, cudaDevAttrMultiProcessorCount,
                                 device));
    check(cudaOccupancyMaxActiveBlocksPerMultiprocessor(
        &per_sm, kernel, WALK_THREADS, shared));
    return sms * std::max(per_sm, 1);
}

// Device buffers, zeroed, freed with it.
struct DeviceBuffers {
    std::vector<void*> buffers;

    ~DeviceBuffers() {
        for (void* buffer : buffers) check(cudaFree(buffer));
    }

    template <typename T>
    T* allocate(size_t count) {
        void* out = nullptr;
        size_t const size = std::max<size_t>(count * sizeof(T), 16);
        check(cudaMalloc(&out, size));
        buffers.push_back(out);
        check(cudaMemset(out, 0, size));
        return static_cast<T*>(out);
    }

    template <typename T>
    T* upload(std::vector<T> const& from) {
        T* out = allocate<T>(from.size());
        check(cudaMemcpy(out, from.data(), from.size() * sizeof(T),
                         cudaMemcpyHostToDevice));
        return out;
    }

    template <typename T>
    void download(T const* from, std::vector<T>* out) {
        check(cudaMemcpy(out->data(), from, out->size() * sizeof(T),
                         cudaMemcpyDeviceToHost));
    }
};

// Sets the walk's bags, prefix letters and bag from `inputs`, uploading to
// `d`.
template <typename BagKey>
void set_walk_inputs(GpuLeftoverInputs const& inputs, Packing const& packing,
                     DeviceBuffers* d, LeftoverParams<BagKey>* out) {
    LeftoverParams<BagKey>& p = *out;
    std::vector<WalkBag<BagKey>> bags(inputs.pairs.size());
    for (size_t i = 0; i < bags.size(); ++i) {
        if (inputs.pairs[i] > UINT32_MAX) {
            report(program_name, "a pair bag has more than %u PAIRS",
                   UINT32_MAX);
            exit(1);
        }
        bags[i] = {packing.pack<BagKey>(
                       &inputs.pair_letters[i * GPU_LETTERS_BYTES]),
                   uint32_t(inputs.pairs[i]),
                   inputs.runs[i] << 8 | inputs.lengths[i]};
    }
    p.bags = d->upload(bags);
    auto const mask = [&](int letter) {
        int bits = 0;
        while ((inputs.bag[letter] >> bits) != 0) ++bits;
        return (1 << bits) - 1;
    };
    p.prefix = int(inputs.prefix.size());
    for (int k = 0; k < p.prefix; ++k) {
        auto const [letter, place] = inputs.prefix[k];
        p.shifts[k] = packing.shifts[letter];
        p.masks[k] = uint8_t(mask(letter));
        p.places[k] = uint8_t(place);
    }
    if (inputs.last_letter >= 0) {
        p.last_shift = packing.shifts[inputs.last_letter];
        p.last_mask = mask(inputs.last_letter);
    }
    p.last_bits = inputs.last_bits;
    p.bag = packing.pack<BagKey>(inputs.bag);
    p.bag_guards = BagKey(packing.guards);
    p.bag_number = inputs.bag_number;
    p.bag_letters = rest_length(inputs.bag);
}

// The share of a walk's work finished: by work group, `totals` candidates
// below every unit's first pick, and `now`'s. A group's unfinished units
// scan as many in all for each below their first pick as its finished
// units did, or the nearest group's with finished units. Negative before
// any unit finishes.
double finished_share(std::vector<unsigned long long> const& totals,
                      WalkProgress const& now) {
    unsigned long long const* const first = now.first;
    unsigned long long const* const work = now.work;
    double finished = 0;
    double total = 0;
    for (int g = 0; g < WORK_GROUPS; ++g) {
        finished += double(work[g]);
        total += double(work[g]);
        if (totals[g] <= first[g]) continue;
        int near = -1;
        for (int d = 0; d < WORK_GROUPS && near < 0; ++d) {
            if (g >= d && first[g - d] != 0)
                near = g - d;
            else if (g + d < WORK_GROUPS && first[g + d] != 0)
                near = g + d;
        }
        if (near < 0) return -1;
        total += double(totals[g] - first[g]) * double(work[near]) /
                 double(first[near]);
    }
    return total > 0 ? finished / total : -1;
}

// Waits for the walk on `stream` to finish, reporting to `progress` from
// `device`, with `totals` as finished_share takes them when `shares`, and
// `skipped` units finished before it started.
void wait_for_walk(cudaStream_t stream, WalkProgress const* device,
                   std::vector<unsigned long long> const& totals, bool shares,
                   size_t skipped, GpuWalkProgress const& progress) {
    cudaStream_t poll = nullptr;
    check(cudaStreamCreateWithFlags(&poll, cudaStreamNonBlocking));
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        cudaError_t const status = cudaStreamQuery(stream);
        if (status != cudaErrorNotReady) {
            check(status);
            break;
        }
        if (!progress.due()) continue;
        WalkProgress now;
        check(cudaMemcpyAsync(&now, device, sizeof now,
                              cudaMemcpyDeviceToHost, poll));
        check(cudaStreamSynchronize(poll));
        progress.report(size_t(now.done) + skipped,
                        shares ? finished_share(totals, now) : -1);
    }
    check(cudaStreamDestroy(poll));
    check(cudaStreamSynchronize(stream));
    check(cudaGetLastError());
    if (getenv("BSC_DEBUG")) {
        WalkProgress now;
        check(cudaMemcpy(&now, device, sizeof now, cudaMemcpyDeviceToHost));
        unsigned long long f = 0, w = 0;
        for (int g = 0; g < WORK_GROUPS; ++g) { f += now.first[g]; w += now.work[g]; }
        fprintf(stderr, "DEBUG first %llu work %llu lookups %llu misses %llu miss_work %llu\n", f, w, now.lookups, now.misses, now.miss_work);
    }
}

// The order a pair usage walk takes its steps in and what it leaves out:
// whether the PAIRS bags' steps come first, the steps it walks of the first
// list and of the other, and with a memo, the divisors of the first and the
// other list's counts.
struct UsageOrder {
    bool pairs_first;
    int first_steps;
    int other_steps;
    uint64_t first_divisor = 1;
    uint64_t other_divisor = 1;
};

// The memo walk's order for `inputs`: the list with 2 or more steps last,
// the PAIRS bags' if both have, less one of its steps.
UsageOrder memo_order(GpuLeftoverInputs const& inputs) {
    int const pairs = inputs.pair_steps;
    int const prospects = inputs.steps - inputs.pair_steps;
    if (pairs >= 2) {
        if (prospects == 0)
            return {true, pairs - 1, 0, uint64_t(pairs - 1), 1};
        return {false, prospects, pairs - 1, uint64_t(pairs),
                uint64_t(pairs - 1)};
    }
    if (pairs == 0)
        return {false, prospects - 1, 0, uint64_t(prospects - 1), 1};
    return {true, pairs, prospects - 1, uint64_t(prospects),
            uint64_t(prospects - 1)};
}

// The memo for rests numbered below `numbers` and PAIRS of at most `most`
// that the device has room for, without its slots; false when it has none.
bool memo_layout(uint64_t numbers, uint64_t most, Memo* out) {
    size_t free = 0;
    size_t total = 0;
    check(cudaMemGetInfo(&free, &total));
    size_t const room = free > MEMO_RESERVE ? free - MEMO_RESERVE : 0;
    int wide = 0;
    while (wide < 40 && (size_t(16) << wide) <= room) ++wide;
    if (getenv("BSC_MEMO_BITS")) wide = atoi(getenv("BSC_MEMO_BITS"));
    Memo& memo = *out;
    memo.number_bits = 1;
    while (memo.number_bits < 64 && (numbers - 1) >> memo.number_bits != 0)
        ++memo.number_bits;
    memo.value_bits = 1;
    while ((most + 1) >> memo.value_bits != 0) ++memo.value_bits;
    int const narrow = std::min(wide + 1, memo.number_bits);
    memo.narrow = memo.number_bits - narrow + memo.value_bits <= 32;
    memo.slot_bits = memo.narrow ? narrow : std::min(wide, memo.number_bits);
    return wide > 0 && memo.number_bits - memo.slot_bits + memo.value_bits <= 64;
}

// Builds the fit tables of bags [begin, end) of `inputs` on the device into
// `out`, and writes every bag's number over the dense letters to `numbers`
// and the bag's to `bag_number`. False when the tables don't suit the bag.
template <typename BagKey>
bool build_fit_tables(GpuLeftoverInputs const& inputs, Packing const& packing,
                      int begin, int end, DeviceBuffers* d,
                      FitTables<BagKey>* out, std::vector<uint64_t>* numbers,
                      uint64_t* bag_number) {
    int const bags = int(inputs.pairs.size());
    auto const letters_of = [&](int i) {
        return &inputs.pair_letters[size_t(i) * GPU_LETTERS_BYTES];
    };
    // The dense letters: those the bag has fewest of, as many as leave the
    // tables, the third term's too, within the device's free memory; or
    // while their rests number at most DENSE_MOST when no split does.
    std::vector<int> by_count;
    for (int letter = 0; letter < 26; ++letter)
        if (inputs.bag[letter] != 0) by_count.push_back(letter);
    std::stable_sort(by_count.begin(), by_count.end(), [&](int a, int b) {
        return inputs.bag[a] < inputs.bag[b];
    });
    std::vector<uint64_t> most(26, 0);
    for (int i = begin; i < end; ++i)
        for (int letter = 0; letter < 26; ++letter)
            most[letter] =
                std::max<uint64_t>(most[letter], letters_of(i)[letter]);
    size_t free_bytes = 0;
    size_t device_bytes = 0;
    check(cudaMemGetInfo(&free_bytes, &device_bytes));
    uint64_t const dense_most = getenv("BSC_DENSE") ? strtoull(getenv("BSC_DENSE"), nullptr, 10) : 0;
    size_t dense_count = 0;
    {
        uint64_t rests = 1;
        size_t fallback = 0;
        for (size_t k = 0; k <= by_count.size(); ++k) {
            if (k > 0) rests *= inputs.bag[by_count[k - 1]] + 1;
            if (rests > (uint64_t(1) << 32)) break;
            if (rests <= DENSE_MOST) fallback = k;
            if (dense_most != 0 && rests > dense_most) break;
            if (by_count.size() - k > size_t(SPARSE_LETTERS)) continue;
            long double one = 0;
            long double two = 0;
            long double triple = 0;
            for (size_t a = k; a < by_count.size(); ++a) {
                long double const cap = most[by_count[a]];
                triple += two * cap;
                two += one * cap;
                one += cap;
            }
            if (rests * one > UINT32_MAX || rests * two > TWO_MOST) continue;
            if (free_bytes > TRIPLE_RESERVE &&
                rests * (1 + one + two + triple) * 4 <=
                    free_bytes - TRIPLE_RESERVE)
                dense_count = k;
        }
        if (dense_count == 0) dense_count = fallback;
    }
    uint64_t lattice = 1;
    std::vector<uint64_t> strides(26, 0);
    std::vector<std::pair<int, uint64_t>> dims;
    std::vector<int> sparse;
    BagKey dense_guards = 0;
    for (size_t k = 0; k < by_count.size(); ++k) {
        int const letter = by_count[k];
        uint64_t const size = inputs.bag[letter] + 1;
        if (k < dense_count) {
            strides[letter] = lattice;
            dims.push_back({int(size), lattice});
            lattice *= size;
            int bits = 0;
            while ((inputs.bag[letter] >> bits) != 0) ++bits;
            dense_guards |= BagKey(1) << (packing.shifts[letter] + bits);
        } else {
            sparse.push_back(letter);
        }
    }
    std::sort(sparse.begin(), sparse.end());
    int const count = int(sparse.size());
    if (count > SPARSE_LETTERS) return false;
    auto const number = [&](uint8_t const* counts) {
        uint64_t sum = 0;
        for (int letter = 0; letter < 26; ++letter)
            sum += counts[letter] * strides[letter];
        return sum;
    };
    numbers->resize(size_t(bags));
    for (int i = 0; i < bags; ++i) (*numbers)[i] = number(letters_of(i));
    *bag_number = number(inputs.bag);

    FitTables<BagKey>& t = *out;
    t.letters = count;
    t.dense_guards = dense_guards;
    std::vector<uint64_t> caps(size_t(count), 0);
    uint64_t all_pairs = 0;
    for (int i = begin; i < end; ++i) {
        all_pairs += inputs.pairs[i];
        for (int e = 0; e < count; ++e)
            caps[e] = std::max<uint64_t>(caps[e], letters_of(i)[sparse[e]]);
    }
    if (all_pairs > UINT32_MAX) return false;
    uint64_t one_entries = 0;
    for (int e = 0; e < count; ++e) {
        int bits = 0;
        while ((inputs.bag[sparse[e]] >> bits) != 0) ++bits;
        t.shifts[e] = packing.shifts[sparse[e]];
        t.masks[e] = uint8_t((1 << bits) - 1);
        t.caps[e] = uint8_t(caps[e]);
        t.one_base[e] = uint32_t(one_entries);
        one_entries += lattice * caps[e];
    }
    std::vector<uint64_t> two_base(size_t(count) * (count - 1) / 2 + 1, 0);
    uint64_t two_entries = 0;
    for (int e2 = 1; e2 < count; ++e2)
        for (int e1 = 0; e1 < e2; ++e1) {
            two_base[e2 * (e2 - 1) / 2 + e1] = two_entries;
            two_entries += lattice * caps[e1] * caps[e2];
        }
    if (one_entries > UINT32_MAX || two_entries > TWO_MOST) return false;
    std::vector<uint32_t> three_base(
        size_t(count) * (count - 1) * (count - 2) / 6 + 1, 0);
    std::vector<uint64_t> triple_base(three_base.size(), 0);
    uint64_t buckets = 0;
    uint64_t triple_entries = 0;
    for (int e3 = 2; e3 < count; ++e3)
        for (int e2 = 1; e2 < e3; ++e2)
            for (int e1 = 0; e1 < e2; ++e1) {
                size_t const triple =
                    e3 * (e3 - 1) * (e3 - 2) / 6 + e2 * (e2 - 1) / 2 + e1;
                three_base[triple] = uint32_t(buckets);
                buckets += caps[e1] * caps[e2];
                triple_base[triple] = triple_entries;
                triple_entries += lattice * caps[e1] * caps[e2] * caps[e3];
            }
    // The third term's table goes on the device if it has the room.
    size_t const before = (one_entries + two_entries + lattice) * 4;
    bool const with_triple = getenv("BSC_NO_TRIPLE") == nullptr &&
        free_bytes > before + TRIPLE_RESERVE &&
        triple_entries * 4 <= free_bytes - before - TRIPLE_RESERVE;
    std::vector<uint64_t> triple_indexes;
    std::vector<uint32_t> triple_values;

    std::vector<uint32_t> fits(lattice, 0);
    std::vector<uint32_t> one(one_entries, 0);
    std::vector<uint32_t> two(two_entries, 0);
    // A bag in a bucket of `three`: the bucket, its count of the third
    // letter, and the bag.
    struct Listed {
        uint32_t bucket;
        uint32_t count;
        int bag;
    };
    std::vector<Listed> listed;
    for (int i = begin; i < end; ++i) {
        uint8_t const* const counts = letters_of(i);
        uint64_t const n = (*numbers)[i];
        uint32_t const pairs = uint32_t(inputs.pairs[i]);
        fits[n] += pairs;
        for (int e1 = 0; e1 < count; ++e1) {
            uint64_t const u1 = counts[sparse[e1]];
            if (u1 == 0) continue;
            uint64_t const row = n * caps[e1] + (caps[e1] - u1);
            one[t.one_base[e1] + row] += pairs;
            for (int e2 = e1 + 1; e2 < count; ++e2) {
                uint64_t const u2 = counts[sparse[e2]];
                if (u2 == 0) continue;
                two[two_base[e2 * (e2 - 1) / 2 + e1] + row * caps[e2] +
                    (caps[e2] - u2)] += pairs;
                for (int e3 = e2 + 1; e3 < count; ++e3) {
                    uint32_t const u3 = counts[sparse[e3]];
                    if (u3 == 0) continue;
                    size_t const triple = e3 * (e3 - 1) * (e3 - 2) / 6 +
                        e2 * (e2 - 1) / 2 + e1;
                    listed.push_back(
                        {uint32_t(three_base[triple] + (u1 - 1) * caps[e2] +
                                  (u2 - 1)),
                         u3, i});
                    if (!with_triple) continue;
                    triple_indexes.push_back(
                        triple_base[triple] +
                        ((row * caps[e2] + (caps[e2] - u2)) * caps[e3]) +
                        (caps[e3] - u3));
                    triple_values.push_back(pairs);
                }
            }
        }
    }
    if (listed.size() > size_t(INT32_MAX)) return false;
    std::sort(listed.begin(), listed.end(),
              [](Listed const& a, Listed const& b) {
        return a.bucket != b.bucket ? a.bucket < b.bucket
            : a.count != b.count ? a.count > b.count : a.bag < b.bag;
    });
    std::vector<int> starts(buckets + 2, 0);
    std::vector<WalkBag<BagKey>> three(listed.size());
    for (size_t k = 0; k < listed.size(); ++k) {
        ++starts[listed[k].bucket + 1];
        three[k] = {packing.pack<BagKey>(letters_of(listed[k].bag)),
                    uint32_t(inputs.pairs[listed[k].bag]), listed[k].count};
    }
    std::partial_sum(starts.begin(), starts.end(), starts.begin());

    uint32_t* const device_fits = d->upload(fits);
    uint32_t* const device_one = d->upload(one);
    uint32_t* const device_two = d->upload(two);
    auto const sum = [&](uint32_t* table, uint64_t entries, int size,
                         uint64_t stride) {
        if (entries == 0 || size < 2) return;
        uint64_t const lines = entries / size;
        sum_along<uint32_t><<<unsigned((lines + 255) / 256), 256>>>(
            table, lines, stride, size);
        check(cudaGetLastError());
    };
    for (auto const& [size, at] : dims) sum(device_fits, lattice, size, at);
    for (int e = 0; e < count; ++e) {
        uint64_t const entries = lattice * caps[e];
        uint32_t* const table = device_one + t.one_base[e];
        sum(table, entries, int(caps[e]), 1);
        for (auto const& [size, at] : dims)
            sum(table, entries, size, at * caps[e]);
    }
    for (int e2 = 1; e2 < count; ++e2)
        for (int e1 = 0; e1 < e2; ++e1) {
            uint64_t const cells = caps[e1] * caps[e2];
            uint64_t const entries = lattice * cells;
            uint32_t* const table =
                device_two + two_base[e2 * (e2 - 1) / 2 + e1];
            sum(table, entries, int(caps[e2]), 1);
            sum(table, entries, int(caps[e1]), caps[e2]);
            for (auto const& [size, at] : dims)
                sum(table, entries, size, at * cells);
        }
    if (with_triple) {
        uint32_t* const device_triple = d->allocate<uint32_t>(triple_entries);
        {
            DeviceBuffers list;
            uint64_t const* const indexes = list.upload(triple_indexes);
            uint32_t const* const values = list.upload(triple_values);
            unsigned long long const added = triple_indexes.size();
            if (added != 0)
                scatter_add<<<unsigned((added + 255) / 256), 256>>>(
                    device_triple, indexes, values, added);
            check(cudaGetLastError());
            check(cudaDeviceSynchronize());
        }
        for (int e3 = 2; e3 < count; ++e3)
            for (int e2 = 1; e2 < e3; ++e2)
                for (int e1 = 0; e1 < e2; ++e1) {
                    uint64_t const cells = caps[e1] * caps[e2] * caps[e3];
                    uint64_t const entries = lattice * cells;
                    uint32_t* const table = device_triple + triple_base[
                        e3 * (e3 - 1) * (e3 - 2) / 6 + e2 * (e2 - 1) / 2 + e1];
                    sum(table, entries, int(caps[e3]), 1);
                    sum(table, entries, int(caps[e2]), caps[e3]);
                    sum(table, entries, int(caps[e1]), caps[e2] * caps[e3]);
                    for (auto const& [size, at] : dims)
                        sum(table, entries, size, at * cells);
                }
        t.triple = device_triple;
        t.triple_base = d->upload(triple_base);
    }
    check(cudaDeviceSynchronize());
    t.fits = device_fits;
    t.one = device_one;
    t.two = device_two;
    t.three = d->upload(three);
    t.three_starts = d->upload(starts);
    t.two_base = d->upload(two_base);
    t.three_base = d->upload(three_base);
    if (getenv("BSC_DEBUG"))
        fprintf(stderr, "DEBUG tables: %d dense letters, %llu rests, %d sparse letters, one %llu, two %llu entries, triple %llu entries%s, three %zu bags in %llu buckets\n",
                int(dims.size()), (unsigned long long)lattice, count, (unsigned long long)one_entries, (unsigned long long)two_entries, (unsigned long long)triple_entries, with_triple ? "" : " (not built)", listed.size(), (unsigned long long)buckets);
    return true;
}

// A walk's tasks: `totals` as finished_share takes them, and the units with
// no tasks.
struct TaskPlan {
    std::vector<unsigned long long> totals;
    size_t skipped = 0;
};

// Splits the walk of `q` into tasks, allocating from `d` its progress, work
// groups and task arrays.
template <typename BagKey>
TaskPlan plan_tasks(DeviceBuffers* d, UsageParams<BagKey>* q) {
    LeftoverParams<BagKey>& p = q->walk;
    p.progress = d->allocate<WalkProgress>(1);
    p.groups = d->allocate<uint8_t>(size_t(p.count));
    q->next_task = d->allocate<unsigned long long>(1);
    int* const task_chunks = d->allocate<int>(size_t(p.count));
    unsigned long long* const task_counts =
        d->allocate<unsigned long long>(size_t(p.count) + 1);
    unsigned long long* const group_totals =
        d->allocate<unsigned long long>(WORK_GROUPS);
    count_unit_tasks<BagKey>
        <<<walk_grid(count_unit_tasks<BagKey>, 0), WALK_THREADS>>>(
            p, group_totals, task_chunks, task_counts, TASK_CANDIDATES);
    check(cudaGetLastError());
    TaskPlan plan;
    plan.totals.resize(WORK_GROUPS);
    d->download(group_totals, &plan.totals);
    std::vector<unsigned long long> starts(size_t(p.count) + 1);
    d->download(task_counts, &starts);
    unsigned long long tasks = 0;
    for (size_t unit = 0; unit < size_t(p.count); ++unit) {
        unsigned long long const count = starts[unit];
        if (count == 0) ++plan.skipped;
        starts[unit] = tasks;
        tasks += count;
    }
    starts.back() = tasks;
    q->tasks = tasks;
    q->task_stride = getenv("BSC_TASK_STRIDE") ? strtoull(getenv("BSC_TASK_STRIDE"), nullptr, 10) : 1;
    q->task_starts = d->upload(starts);
    q->task_chunks = task_chunks;
    return plan;
}

// Launches the walk of `q` on `stream`.
template <WalkCountType COUNT_TYPE, typename BagKey>
void launch_usage(UsageParams<BagKey> const& q, cudaStream_t stream) {
    size_t const shared =
        size_t(WALK_WARPS) * size_t(q.walk.steps) * sizeof(Level<BagKey>);
    walk_pair_usage<COUNT_TYPE, BagKey>
        <<<walk_grid(walk_pair_usage<COUNT_TYPE, BagKey>, shared),
           WALK_THREADS, shared, stream>>>(q);
    check(cudaGetLastError());
}

// Walks `inputs` in `order`, in tasks, counting each pair bag's usage into
// `out`, with `memo` for PAIR_USAGE_MEMO; or with RESTS, collecting the
// distinct rest numbers a set of `slots` slots holds into `out` and
// returning how many found no slot.
template <WalkCountType COUNT_TYPE, typename BagKey>
struct UsageWalk {
    static constexpr bool MEMO = COUNT_TYPE == WalkCountType::PAIR_USAGE_MEMO;
    static constexpr bool RESTS = COUNT_TYPE == WalkCountType::RESTS;
    static constexpr bool TOP = COUNT_TYPE == WalkCountType::TOP_ROWS;
    static constexpr bool TABLES =
        COUNT_TYPE == WalkCountType::PAIR_USAGE_TABLES;

    DeviceBuffers d;
    UsageParams<BagKey> q{};
    TaskPlan plan;
    UsageOrder order;
    int bags = 0;
    int pair_bags = 0;
    size_t slots = 0;
    int* stop = nullptr;
    // With RESTS, set before prepare(): whether the set counts each rest's
    // rows, which finish() then writes to `rest_rows`.
    bool count_rows = false;
    std::vector<uint64_t> rest_rows;

    // Sets the walk up on the device; false when PAIR_USAGE_TABLES' tables
    // don't suit the bag.
    bool prepare(GpuLeftoverInputs const& inputs, Packing const& packing,
                 UsageOrder const& walk_order, Memo const& memo,
                 size_t set_slots, size_t first_unit, size_t units) {
        order = walk_order;
        slots = set_slots;
        bags = int(inputs.pairs.size());
        pair_bags = inputs.pair_bags;
        bool const two_lists = order.other_steps > 0;
        LeftoverParams<BagKey>& p = q.walk;
        set_walk_inputs(inputs, packing, &d, &p);
        if constexpr (MEMO || RESTS || TOP)
            p.numbers = d.upload(inputs.numbers);
        int const* const pair_offsets = d.upload(inputs.offsets);
        int const* const prospect_offsets = inputs.prospect_offsets.empty()
            ? pair_offsets : d.upload(inputs.prospect_offsets);
        p.offsets[0] = order.pairs_first ? pair_offsets : prospect_offsets;
        p.offsets[1] = !two_lists ? p.offsets[0]
            : order.pairs_first ? prospect_offsets : pair_offsets;
        p.first_unit = order.pairs_first ? 0 : pair_bags;
        p.count = order.pairs_first ? pair_bags : bags - pair_bags;
        p.steps = order.first_steps + order.other_steps;
        p.pair_steps = order.first_steps;
        if constexpr (RESTS || TOP) {
            p.order = d.upload(inputs.places);
            p.units = d.upload(inputs.units);
            p.first_unit = int(first_unit);
            p.count = int(units);
        }
        p.usage = d.allocate<unsigned long long>(size_t(bags));
        stop = d.allocate<int>(1);
        q.stop = stop;
        plan = plan_tasks(&d, &q);
        if constexpr (MEMO || TABLES) {
            bool const pairs_last = order.pairs_first != two_lists;
            int const begin = pairs_last ? 0 : pair_bags;
            int const end = pairs_last ? pair_bags : bags;
            q.memo = memo;
            q.memo.letters = GPU_MAX_LETTERS;
            for (int i = begin; i < end; ++i)
                q.memo.letters =
                    std::min(q.memo.letters, int(inputs.lengths[i]));
            if constexpr (MEMO) {
                q.memo.slots = d.allocate<uint32_t>(
                    (size_t(1) << memo.slot_bits) * (memo.narrow ? 1 : 2));
            } else {
                std::vector<uint64_t> numbers;
                if (!build_fit_tables(inputs, packing, begin, end, &d,
                                      &q.tables, &numbers, &p.bag_number))
                    return false;
                q.tables.stop = stop;
                p.numbers = d.upload(numbers);
            }
        }
        if constexpr (RESTS) {
            q.set_stride = count_rows ? 2 : 1;
            q.set = d.allocate<unsigned long long>(slots * q.set_stride);
            q.set_mask = slots - 1;
            q.set_dropped = d.allocate<unsigned long long>(1);
        }
        return true;
    }

    // The candidates below the units' first picks.
    unsigned long long first_candidates() const {
        unsigned long long sum = 0;
        for (unsigned long long const total : plan.totals) sum += total;
        return sum;
    }

    // Walks about `sample` of the tasks, evenly spaced, for at most `most`
    // seconds, and undoes what it counted; the seconds the sample takes, by
    // the share of it finished when it is cut short.
    double pilot(unsigned long long sample, double most) {
        LeftoverParams<BagKey>& p = q.walk;
        UsageParams<BagKey> some = q;
        some.task_stride = std::max<unsigned long long>(q.tasks / sample, 1);
        double const tasks =
            double((q.tasks + some.task_stride - 1) / some.task_stride);
        cudaStream_t stream = nullptr;
        cudaStream_t side = nullptr;
        check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
        check(cudaStreamCreateWithFlags(&side, cudaStreamNonBlocking));
        // The kernel's first launch loads it: launch it with no tasks first.
        UsageParams<BagKey> none = q;
        none.tasks = 0;
        check(cudaDeviceSynchronize());
        launch_usage<COUNT_TYPE>(none, stream);
        check(cudaStreamSynchronize(stream));
        check(cudaMemset(q.next_task, 0, sizeof(unsigned long long)));
        check(cudaDeviceSynchronize());
        auto const start = std::chrono::steady_clock::now();
        auto const elapsed = [&] {
            return std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start).count();
        };
        launch_usage<COUNT_TYPE>(some, stream);
        double seconds = 0;
        while (cudaStreamQuery(stream) == cudaErrorNotReady) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            // Stop at most of the sample: its last tasks are a few long
            // ones, a warp each.
            WalkProgress now;
            check(cudaMemcpyAsync(&now, p.progress, sizeof now,
                                  cudaMemcpyDeviceToHost, side));
            check(cudaStreamSynchronize(side));
            double const at = elapsed();
            if (at < most && double(now.tasks) < PILOT_SHARE * tasks) continue;
            seconds = at * tasks / double(std::max(now.tasks, 1ULL));
            int const one = 1;
            check(cudaMemcpyAsync(stop, &one, sizeof one,
                                  cudaMemcpyHostToDevice, side));
            check(cudaStreamSynchronize(side));
            break;
        }
        check(cudaStreamSynchronize(stream));
        if (seconds == 0) seconds = elapsed();
        check(cudaMemset(stop, 0, sizeof(int)));
        check(cudaMemset(p.progress, 0, sizeof(WalkProgress)));
        check(cudaMemset(q.next_task, 0, sizeof(unsigned long long)));
        check(cudaMemset(p.usage, 0,
                         size_t(bags) * sizeof(unsigned long long)));
        check(cudaStreamDestroy(side));
        check(cudaStreamDestroy(stream));
        return seconds;
    }

    void run(GpuWalkProgress const& progress) {
        cudaStream_t stream = nullptr;
        check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
        // The buffers' zeroing runs on the default stream, which the walk's
        // stream doesn't wait for.
        check(cudaDeviceSynchronize());
        launch_usage<COUNT_TYPE>(q, stream);
        wait_for_walk(stream, q.walk.progress, plan.totals, true,
                      plan.skipped, progress);
        check(cudaStreamDestroy(stream));
    }

    // Writes the walk's counts to `out`; with RESTS, the distinct rests,
    // returning how many found no slot.
    uint64_t finish(std::vector<uint64_t>* out) {
        static_assert(sizeof(unsigned long long) == sizeof(uint64_t));
        if constexpr (RESTS) {
            out->clear();
            rest_rows.clear();
            size_t const stride = size_t(q.set_stride);
            std::vector<unsigned long long> part(
                std::min(slots, size_t(1) << 23) * stride);
            for (size_t at = 0; at < slots * stride; at += part.size()) {
                d.download(q.set + at, &part);
                for (size_t k = 0; k < part.size(); k += stride) {
                    if (part[k] == 0) continue;
                    out->push_back(part[k] - 1);
                    if (stride == 2) rest_rows.push_back(part[k + 1]);
                }
            }
            std::vector<unsigned long long> dropped(1);
            d.download(q.set_dropped, &dropped);
            return dropped[0];
        }
        out->resize(size_t(bags));
        check(cudaMemcpy(out->data(), q.walk.usage,
                         out->size() * sizeof(uint64_t),
                         cudaMemcpyDeviceToHost));
        for (int i = 0; i < bags; ++i)
            (*out)[i] /= (i < pair_bags) == order.pairs_first
                ? order.first_divisor : order.other_divisor;
        return 0;
    }
};

template <WalkCountType COUNT_TYPE, typename BagKey>
uint64_t walk_usage(GpuLeftoverInputs const& inputs, Packing const& packing,
                    UsageOrder const& order, Memo const& memo, size_t slots,
                    size_t first_unit, size_t units,
                    GpuWalkProgress const& progress,
                    std::vector<uint64_t>* out) {
    UsageWalk<COUNT_TYPE, BagKey> walk;
    if (!walk.prepare(inputs, packing, order, memo, slots, first_unit, units))
        return UINT64_MAX;
    walk.run(progress);
    return walk.finish(out);
}

uint64_t choose2(int n) { return uint64_t(n) * uint64_t(n - 1) / 2; }

// An estimate of how many combinations of `order`'s steps fit the bag of
// `inputs`, from SET_DRAWS random draws a step.
double estimate_combinations(GpuLeftoverInputs const& inputs,
                             UsageOrder const& order) {
    int const bags = int(inputs.pairs.size());
    int const pair_bags = inputs.pair_bags;
    int const steps = order.first_steps + order.other_steps;
    int const begin[2] = {order.pairs_first ? 0 : pair_bags,
                          order.pairs_first ? pair_bags : 0};
    int const size[2] = {order.pairs_first ? pair_bags : bags - pair_bags,
                         order.pairs_first ? bags - pair_bags : pair_bags};
    double all = 1;
    for (int k = 0; k < steps; ++k) {
        int const list = k >= order.first_steps;
        int const nth = list ? k - order.first_steps : k;
        if (size[list] == 0) return 0;
        all *= double(size[list]) / double(nth + 1);
    }
    // Step by step: the share of draws that fit what a kept draw of the
    // steps before leaves.
    struct Partial {
        uint8_t rest[GPU_LETTERS_BYTES];
        int chosen[GPU_MAX_PAIR_STEPS];
    };
    std::mt19937_64 random(1);
    std::vector<Partial> kept(1);
    memcpy(kept[0].rest, inputs.bag, GPU_LETTERS_BYTES);
    double share = 1;
    for (int k = 0; k < steps && !kept.empty(); ++k) {
        int const list = k >= order.first_steps;
        std::vector<Partial> next;
        int fit = 0;
        for (int draw = 0; draw < SET_DRAWS; ++draw) {
            Partial partial = kept[random() % kept.size()];
            partial.chosen[k] =
                begin[list] + int(random() % uint64_t(size[list]));
            bool ok = true;
            for (int j = list ? order.first_steps : 0; j < k; ++j)
                ok = ok && partial.chosen[j] != partial.chosen[k];
            uint8_t const* const letters = &inputs.pair_letters[
                size_t(partial.chosen[k]) * GPU_LETTERS_BYTES];
            for (int letter = 0; letter < 26 && ok; ++letter) {
                ok = letters[letter] <= partial.rest[letter];
                partial.rest[letter] -= letters[letter];
            }
            if (!ok) continue;
            ++fit;
            if (next.size() < SET_KEPT) next.push_back(partial);
        }
        share *= double(fit) / SET_DRAWS;
        kept.swap(next);
    }
    return kept.empty() ? 0 : all * share;
}

// The sums walk's order for `inputs`: the list with 3 or more steps last,
// the PAIRS bags' if both have, less two of its steps.
UsageOrder sums_order(GpuLeftoverInputs const& inputs) {
    int const pairs = inputs.pair_steps;
    int const prospects = inputs.steps - inputs.pair_steps;
    if (pairs >= 3) {
        if (prospects == 0)
            return {true, pairs - 2, 0, choose2(pairs - 1), 1};
        return {false, prospects, pairs - 2, choose2(pairs),
                choose2(pairs - 1)};
    }
    if (pairs == 0)
        return {false, prospects - 2, 0, choose2(prospects - 1), 1};
    return {true, pairs, prospects - 2, choose2(prospects),
            choose2(prospects - 1)};
}

// Walks `inputs` in `order`, which leaves out two steps of its last list,
// counting each pair bag's usage into `out`: for each slab of rests, builds
// the table of the left-out list's pair sums that fit each rest, then walks
// the combinations that leave a rest in the slab and counts their last two
// picks from the table. False, with nothing walked, when the device has no
// room for a slab.
template <typename BagKey>
bool walk_usage_sums(GpuLeftoverInputs const& inputs, Packing const& packing,
                     UsageOrder const& order, double other_seconds,
                     GpuWalkProgress const& progress,
                     std::vector<uint64_t>* out) {
    using Entry = unsigned long long;
    int const bags = int(inputs.pairs.size());
    int const pair_bags = inputs.pair_bags;
    bool const two_lists = order.other_steps > 0;
    bool const pairs_left = order.pairs_first != two_lists;
    int const left_begin = pairs_left ? 0 : pair_bags;
    int const left_end = pairs_left ? pair_bags : bags;

    // The slab letters: the fewest slabs whose tables fit the device.
    std::vector<int> letters;
    long double total = 1;
    for (int letter = 0; letter < 26; ++letter) {
        if (inputs.bag[letter] == 0) continue;
        letters.push_back(letter);
        total *= inputs.bag[letter] + 1;
    }
    size_t free = 0;
    size_t device_total = 0;
    check(cudaMemGetInfo(&free, &device_total));
    size_t const room = free > SUMS_RESERVE ? free - SUMS_RESERVE : 0;
    long double const most = (long double)(room / sizeof(Entry));
    if (getenv("BSC_SLAB_ENTRIES")) const_cast<long double&>(most) = strtold(getenv("BSC_SLAB_ENTRIES"), nullptr);
    if (most < 1) return false;
    long double const need = total / most;
    std::vector<int> slab_letters;
    long double best = 0;
    int const count = int(letters.size());
    std::vector<int> chosen;
    std::function<void(int, long double)> search = [&](int from,
                                                       long double product) {
        if (product >= need) {
            if (best == 0 || product < best ||
                (product == best && chosen.size() < slab_letters.size())) {
                best = product;
                slab_letters = chosen;
            }
            return;
        }
        if (int(chosen.size()) == SLAB_LETTERS) return;
        for (int k = from; k < count; ++k) {
            chosen.push_back(letters[k]);
            search(k + 1, product * (inputs.bag[letters[k]] + 1));
            chosen.pop_back();
        }
    };
    search(0, 1);
    if (best == 0 || best > SLABS_MOST) return false;
    size_t const slabs = size_t(best);
    size_t const entries = size_t(total / best + 0.5L);

    // Every bag's number over the other letters, and the slab letters' bits.
    std::vector<uint64_t> strides(26, 0);
    std::vector<std::pair<int, uint64_t>> dims;
    uint64_t stride = 1;
    for (int const letter : letters) {
        if (std::find(slab_letters.begin(), slab_letters.end(), letter) !=
            slab_letters.end())
            continue;
        strides[letter] = stride;
        dims.push_back({inputs.bag[letter] + 1, stride});
        stride *= inputs.bag[letter] + 1;
    }
    auto const number = [&](uint8_t const* counts) {
        uint64_t sum = 0;
        for (int const letter : letters) sum += counts[letter] * strides[letter];
        return sum;
    };
    std::vector<uint64_t> numbers;
    numbers.resize(size_t(bags));
    for (int i = 0; i < bags; ++i)
        numbers[i] = number(&inputs.pair_letters[size_t(i) * GPU_LETTERS_BYTES]);

    DeviceBuffers d;
    UsageParams<BagKey> q{};
    LeftoverParams<BagKey>& p = q.walk;
    set_walk_inputs(inputs, packing, &d, &p);
    p.numbers = d.upload(numbers);
    p.bag_number = number(inputs.bag);
    Slabs<BagKey>& s = q.slabs;
    s.letters = int(slab_letters.size());
    uint32_t slab_stride = 1;
    for (int k = 0; k < s.letters; ++k) {
        int const letter = slab_letters[k];
        int bits = 0;
        while ((inputs.bag[letter] >> bits) != 0) ++bits;
        s.shifts[k] = packing.shifts[letter];
        s.masks[k] = uint8_t((1 << bits) - 1);
        s.strides[k] = slab_stride;
        slab_stride *= inputs.bag[letter] + 1;
        s.mask |= BagKey((1 << bits) - 1) << packing.shifts[letter];
        s.guards |= BagKey(1) << (packing.shifts[letter] + bits);
    }
    s.shortest = GPU_MAX_LETTERS;
    for (int i = left_begin; i < left_end; ++i)
        s.shortest = std::min(s.shortest, int(inputs.lengths[i]));
    s.counts = d.allocate<unsigned long long>(slabs);
    s.sums = d.allocate<Entry>(entries);

    // The walk of the combinations, and the walk of the left-out list's
    // 2-sets.
    int const* const pair_offsets = d.upload(inputs.offsets);
    int const* const prospect_offsets = inputs.prospect_offsets.empty()
        ? pair_offsets : d.upload(inputs.prospect_offsets);
    p.offsets[0] = order.pairs_first ? pair_offsets : prospect_offsets;
    p.offsets[1] = !two_lists ? p.offsets[0]
        : order.pairs_first ? prospect_offsets : pair_offsets;
    p.first_unit = order.pairs_first ? 0 : pair_bags;
    p.count = order.pairs_first ? pair_bags : bags - pair_bags;
    p.steps = order.first_steps + order.other_steps;
    p.pair_steps = order.first_steps;
    p.usage = d.allocate<unsigned long long>(size_t(bags));
    q.stop = d.allocate<int>(1);
    UsageParams<BagKey> sums = q;
    sums.walk.offsets[0] = pairs_left ? pair_offsets : prospect_offsets;
    sums.walk.offsets[1] = sums.walk.offsets[0];
    sums.walk.first_unit = left_begin;
    sums.walk.count = left_end - left_begin;
    sums.walk.steps = 2;
    sums.walk.pair_steps = 2;
    plan_tasks(&d, &q);
    plan_tasks(&d, &sums);

    cudaStream_t stream = nullptr;
    check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
    auto const run = [&](auto const& launch, unsigned long long* next_task) {
        check(cudaMemsetAsync(next_task, 0, sizeof(unsigned long long),
                              stream));
        launch();
        check(cudaStreamSynchronize(stream));
        check(cudaGetLastError());
    };
    run([&] { launch_usage<WalkCountType::SUM_SLABS>(q, stream); },
        q.next_task);
    std::vector<unsigned long long> slab_counts(slabs);
    d.download(s.counts, &slab_counts);
    unsigned long long combinations = 0;
    size_t used = 0;
    for (unsigned long long const n : slab_counts) {
        combinations += n;
        used += n != 0;
    }
    bool const debug = getenv("BSC_DEBUG") != nullptr;
    double const seconds = double(used) *
        (double(entries) * sizeof(Entry) * SUMS_SECONDS_PER_BYTE +
         SUMS_SECONDS_PER_SLAB);
    if (debug)
        fprintf(stderr, "DEBUG sums: %zu slabs of %zu entries (%.1f MB), %zu used, %llu combinations, letters %d, est %.1fs against %.1fs\n",
                slabs, entries, double(entries) * sizeof(Entry) / 1048576, used, combinations, s.letters, seconds, other_seconds);
    if (seconds > other_seconds) {
        check(cudaStreamDestroy(stream));
        return false;
    }
    unsigned long long done = 0;
    for (size_t slab = 0; slab < slabs; ++slab) {
        if (slab_counts[slab] == 0) continue;
        auto const t0 = std::chrono::steady_clock::now();
        BagKey key = 0;
        for (int k = 0; k < s.letters; ++k) {
            int const letter = slab_letters[k];
            key |= BagKey(slab / s.strides[k] % (inputs.bag[letter] + 1))
                << packing.shifts[letter];
        }
        q.slabs.slab = key;
        sums.slabs.slab = key;
        check(cudaMemsetAsync(s.sums, 0, entries * sizeof(Entry), stream));
        run([&] { launch_usage<WalkCountType::SUM_SCATTER>(sums, stream); },
            sums.next_task);
        auto const t1 = std::chrono::steady_clock::now();
        for (auto const& [size, at] : dims) {
            unsigned long long const lines = entries / size;
            sum_along<Entry><<<unsigned((lines + 255) / 256), 256, 0, stream>>>(
                s.sums, lines, at, size);
            check(cudaGetLastError());
        }
        check(cudaStreamSynchronize(stream));
        auto const t2 = std::chrono::steady_clock::now();
        run([&] { launch_usage<WalkCountType::PAIR_USAGE_SUMS>(q, stream); },
            q.next_task);
        auto const t3 = std::chrono::steady_clock::now();
        if (debug) {
            auto const sec = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
            fprintf(stderr, "DEBUG slab %zu: %llu combinations, scatter %.2fs, sums %.2fs, walk %.2fs\n",
                    slab, slab_counts[slab], sec(t0, t1), sec(t1, t2), sec(t2, t3));
        }
        done += slab_counts[slab];
        if (progress.due())
            progress.report(size_t(double(p.count) * double(done) /
                                   double(combinations)),
                            double(done) / double(combinations));
    }
    check(cudaStreamDestroy(stream));
    if (debug) {
        WalkProgress now;
        check(cudaMemcpy(&now, p.progress, sizeof now, cudaMemcpyDeviceToHost));
        fprintf(stderr, "DEBUG sums lookups %llu scans %llu scan_work %llu\n", now.lookups, now.misses, now.miss_work);
    }
    out->resize(size_t(bags));
    static_assert(sizeof(unsigned long long) == sizeof(uint64_t));
    check(cudaMemcpy(out->data(), p.usage, out->size() * sizeof(uint64_t),
                     cudaMemcpyDeviceToHost));
    for (int i = 0; i < bags; ++i)
        (*out)[i] /= (i < pair_bags) == order.pairs_first
            ? order.first_divisor : order.other_divisor;
    return true;
}

// Walks `inputs` counting COUNT_TYPE's counts into `out`: GpuLeftoverCounts
// for LEFTOVERS, one count per pair bag for PAIR_USAGE.
template <WalkCountType COUNT_TYPE, typename BagKey, typename Out>
void walk_leftovers(GpuLeftoverInputs const& inputs, Packing const& packing,
                    GpuWalkProgress const& progress, Out* out) {
    constexpr bool LEFTOVERS = COUNT_TYPE == WalkCountType::LEFTOVERS;
    int const bags = int(inputs.pairs.size());
    int const first_unit = inputs.pair_steps > 0 ? 0 : inputs.pair_bags;
    int const count = inputs.pair_steps > 0 ? inputs.pair_bags
                                            : bags - inputs.pair_bags;
    int const letters = rest_length(inputs.bag);
    size_t const lengths = size_t(letters) + 1;
    size_t const steps = size_t(inputs.steps);
    DeviceBuffers d;
    LeftoverParams<BagKey> p{};
    set_walk_inputs(inputs, packing, &d, &p);
    if constexpr (LEFTOVERS) p.numbers = d.upload(inputs.numbers);
    p.offsets[0] = d.upload(inputs.offsets);
    p.offsets[1] = inputs.prospect_offsets.empty()
        ? p.offsets[0] : d.upload(inputs.prospect_offsets);
    p.count = count;
    p.first_unit = first_unit;
    p.steps = inputs.steps;
    p.pair_steps = inputs.pair_steps;
    p.next_unit = d.allocate<unsigned long long>(1);
    if constexpr (LEFTOVERS) {
        p.by_picks = d.allocate<unsigned long long>(steps);
        p.combinations = d.allocate<unsigned long long>(lengths);
        p.rows = d.allocate<unsigned long long>(lengths);
        p.registers = d.allocate<uint32_t>(lengths << GPU_HLL_BITS);
    } else {
        p.usage = d.allocate<unsigned long long>(size_t(bags));
    }
    size_t const shared =
        LEFTOVERS ? walk_counts_bytes(letters, inputs.steps) : 0;
    int const grid =
        walk_grid(walk_leftover_picks<COUNT_TYPE, BagKey>, shared);
    p.progress = d.allocate<WalkProgress>(1);
    std::vector<unsigned long long> totals(WORK_GROUPS);
    if (inputs.steps > 1) {
        p.groups = d.allocate<uint8_t>(size_t(count));
        unsigned long long* const group_totals =
            d.allocate<unsigned long long>(WORK_GROUPS);
        count_first_candidates<BagKey>
            <<<walk_grid(count_first_candidates<BagKey>, 0), WALK_THREADS>>>(
                p, group_totals);
        check(cudaGetLastError());
        d.download(group_totals, &totals);
    }

    cudaStream_t stream = nullptr;
    check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
    walk_leftover_picks<COUNT_TYPE, BagKey>
        <<<grid, WALK_THREADS, shared, stream>>>(p);
    check(cudaGetLastError());
    wait_for_walk(stream, p.progress, totals, inputs.steps > 1, 0, progress);
    if constexpr (LEFTOVERS) {
        out->by_picks.assign(steps, 0);
        out->combinations.assign(lengths, 0);
        out->rows.assign(lengths, 0);
        out->registers.assign(lengths << GPU_HLL_BITS, 0);
        std::vector<unsigned long long> counts(steps);
        d.download(p.by_picks, &counts);
        std::copy(counts.begin(), counts.end(), out->by_picks.begin());
        counts.resize(lengths);
        d.download(p.combinations, &counts);
        std::copy(counts.begin(), counts.end(), out->combinations.begin());
        d.download(p.rows, &counts);
        std::copy(counts.begin(), counts.end(), out->rows.begin());
        d.download(p.registers, &out->registers);
    } else {
        static_assert(sizeof(unsigned long long) == sizeof(uint64_t));
        out->resize(size_t(bags));
        check(cudaMemcpy(out->data(), p.usage, out->size() * sizeof(uint64_t),
                         cudaMemcpyDeviceToHost));
    }
    check(cudaStreamDestroy(stream));
}

// Walks `inputs` counting each pair bag's usage into `out` by the fastest of
// the fit tables walk and the walk it replaces, the memo walk when `memo`,
// for PAIRS of at most `most`, else the scanning walk: the one `mode` names,
// else the one whose pilot gets through the larger share of its walk a
// second.
template <typename BagKey>
void choose_usage_walk(GpuLeftoverInputs const& inputs, Packing const& packing,
                       bool memo, uint64_t most, char const* mode,
                       GpuWalkProgress const& progress,
                       std::vector<uint64_t>* out) {
    int const pairs = inputs.pair_steps;
    int const prospects = inputs.steps - pairs;
    UsageOrder const all{pairs > 0, pairs > 0 ? pairs : prospects,
                         pairs > 0 ? prospects : 0};
    bool const can_leave =
        inputs.steps >= 3 && (pairs >= 2 || prospects >= 2);
    UsageOrder const less = can_leave ? memo_order(inputs) : all;
    char const forced = mode != nullptr ? mode[0] : 0;

    auto const t0 = std::chrono::steady_clock::now();
    auto const lap = [&](char const* what) {
        if (getenv("BSC_DEBUG"))
            fprintf(stderr, "DEBUG %.2fs %s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), what);
    };
    UsageWalk<WalkCountType::PAIR_USAGE, BagKey> scan;
    UsageWalk<WalkCountType::PAIR_USAGE_MEMO, BagKey> remembered;
    UsageWalk<WalkCountType::PAIR_USAGE_TABLES, BagKey> tables;
    scan.prepare(inputs, packing, all, {}, 0, 0, 0);
    lap("scan prepared");
    bool use_tables = can_leave && (forced == 0 || forced == 't') &&
        (forced == 't' || scan.first_candidates() >= PILOT_FROM) &&
        tables.prepare(inputs, packing, less, {}, 0, 0, 0);
    // The memo takes the room the tables leave.
    Memo layout{};
    bool const use_memo = memo && !(use_tables && forced == 't') &&
        memo_layout(inputs.number_count, most, &layout);
    lap("tables prepared");
    if (use_memo) remembered.prepare(inputs, packing, less, layout, 0, 0, 0);
    lap("memo prepared");
    if (use_tables && forced != 't') {
        double const table_seconds = tables.pilot(PILOT_TASKS, PILOT_SECONDS);
        lap("tables pilot");
        double const other_seconds = use_memo
            ? remembered.pilot(PILOT_TASKS, PILOT_SECONDS)
            : scan.pilot(PILOT_TASKS, PILOT_SECONDS);
        if (getenv("BSC_DEBUG"))
            fprintf(stderr, "DEBUG pilot: %llu of %llu tasks: tables %.3fs, %s %.3fs\n",
                    PILOT_TASKS, tables.q.tasks, table_seconds,
                    use_memo ? "memo" : "scan", other_seconds);
        lap("pilots");
        use_tables = table_seconds < other_seconds;
    }
    if (use_tables) {
        tables.run(progress);
        tables.finish(out);
    } else if (use_memo) {
        remembered.run(progress);
        remembered.finish(out);
    } else {
        scan.run(progress);
        scan.finish(out);
    }
}

}  // namespace

void run_leftovers_gpu(std::string_view program,
                       GpuLeftoverInputs const& inputs,
                       GpuWalkProgress const& progress,
                       GpuLeftoverCounts* out) {
    program_name = program;
    check(cudaSetDevice(0));
    Packing const packing(inputs.bag);
    if (packing.bits <= 64)
        walk_leftovers<WalkCountType::LEFTOVERS, Key64>(inputs, packing,
                                                        progress, out);
    else
        walk_leftovers<WalkCountType::LEFTOVERS, Key128>(inputs, packing,
                                                         progress, out);
}

void run_pair_usage_gpu(std::string_view program,
                        GpuLeftoverInputs const& inputs,
                        GpuWalkProgress const& progress,
                        std::vector<uint64_t>* out) {
    program_name = program;
    check(cudaSetDevice(0));
    Packing const packing(inputs.bag);
    bool const wide = packing.bits > 64;
    int const pairs = inputs.pair_steps;
    int const prospects = inputs.steps - pairs;
    uint64_t most = 0;
    for (uint64_t const count : inputs.pairs) most += count;
    char const* const mode = getenv("BSC_UC_MODE");
    bool const can_memo = !inputs.numbers.empty() && inputs.steps >= 3 &&
        (pairs >= 2 || prospects >= 2);
    bool const memo =
        can_memo && (mode != nullptr ? mode[0] == 'm' : inputs.steps >= 4);
    if (inputs.steps < 2 || (mode != nullptr && mode[0] == 'o')) {
        if (wide)
            walk_leftovers<WalkCountType::PAIR_USAGE, Key128>(
                inputs, packing, progress, out);
        else
            walk_leftovers<WalkCountType::PAIR_USAGE, Key64>(
                inputs, packing, progress, out);
        return;
    }
    bool const sums = (mode == nullptr || mode[0] == 's') &&
        inputs.steps >= 4 && (pairs >= 3 || prospects >= 3);
    if (sums) {
        UsageOrder const order = sums_order(inputs);
        double other = 1e300;
        if (mode == nullptr) {
            UsageOrder const walked = memo
                ? memo_order(inputs)
                : UsageOrder{pairs > 0, pairs > 0 ? pairs : prospects,
                             pairs > 0 ? prospects : 0};
            UsageOrder before = walked;
            if (!memo) --(before.other_steps > 0 ? before.other_steps
                                                 : before.first_steps);
            other = estimate_combinations(inputs, before) /
                (memo ? MEMO_RATE : SCAN_RATE);
        }
        if (wide ? walk_usage_sums<Key128>(inputs, packing, order, other,
                                           progress, out)
                 : walk_usage_sums<Key64>(inputs, packing, order, other,
                                          progress, out))
            return;
    }
    if (wide)
        choose_usage_walk<Key128>(inputs, packing, memo, most, mode, progress,
                                  out);
    else
        choose_usage_walk<Key64>(inputs, packing, memo, most, mode, progress,
                                 out);
}

namespace {

// Adds to `count` the slots of `set`, each `stride` words, that hold a
// rest.
__global__ void count_set_rests(unsigned long long const* set, size_t slots,
                                int stride, unsigned long long* count) {
    size_t const step = size_t(gridDim.x) * blockDim.x;
    unsigned long long found = 0;
    for (size_t i = size_t(blockIdx.x) * blockDim.x + threadIdx.x; i < slots;
         i += step)
        found += set[i * stride] != 0;
    found = warp_sum(found);
    if ((threadIdx.x & 31) == 0 && found != 0) atomicAdd(count, found);
}

// The RESTS walk of `inputs`, as run_distinct_rests_gpu() describes it.
template <typename BagKey>
uint64_t walk_distinct_rests(GpuLeftoverInputs const& inputs,
                             Packing const& packing, size_t slots,
                             size_t first_unit, size_t units,
                             std::vector<uint64_t>* out,
                             std::vector<uint64_t>* rows, size_t* found) {
    int const pairs = inputs.pair_steps;
    UsageOrder const order{pairs > 0, pairs > 0 ? pairs : inputs.steps,
                           pairs > 0 ? inputs.steps - pairs : 0};
    GpuWalkProgress const progress{[] { return false; },
                                   [](size_t, double) {}};
    UsageWalk<WalkCountType::RESTS, BagKey> walk;
    walk.count_rows = rows != nullptr;
    walk.prepare(inputs, packing, order, {}, slots, first_unit, units);
    walk.run(progress);
    if (out == nullptr) {
        unsigned long long* const count =
            walk.d.template allocate<unsigned long long>(1);
        count_set_rests<<<1024, 256>>>(walk.q.set, slots, walk.q.set_stride,
                                       count);
        check(cudaGetLastError());
        std::vector<unsigned long long> counted(1);
        walk.d.download(count, &counted);
        *found = size_t(counted[0]);
        std::vector<unsigned long long> dropped(1);
        walk.d.download(walk.q.set_dropped, &dropped);
        return dropped[0];
    }
    uint64_t const dropped = walk.finish(out);
    if (rows != nullptr) *rows = std::move(walk.rest_rows);
    if (found != nullptr) *found = out->size();
    return dropped;
}

// The TOP_ROWS walk of `inputs`, as run_top_rows_gpu() describes it.
template <typename BagKey>
bool walk_top_rows(GpuLeftoverInputs const& inputs, Packing const& packing,
                   size_t first_unit, size_t units,
                   std::vector<uint64_t> const& keys,
                   std::vector<uint32_t> const& values, uint32_t last_unit,
                   size_t capacity, std::vector<uint32_t>* rows,
                   std::vector<uint64_t>* unit_ties) {
    int const pairs = inputs.pair_steps;
    UsageOrder const order{pairs > 0, pairs > 0 ? pairs : inputs.steps,
                           pairs > 0 ? inputs.steps - pairs : 0};
    GpuWalkProgress const progress{[] { return false; },
                                   [](size_t, double) {}};
    UsageWalk<WalkCountType::TOP_ROWS, BagKey> walk;
    walk.prepare(inputs, packing, order, {}, 0, first_unit, units);
    UsageParams<BagKey>& q = walk.q;
    static_assert(sizeof(unsigned long long) == sizeof(uint64_t));
    if (!keys.empty()) {
        q.set = reinterpret_cast<unsigned long long*>(walk.d.upload(keys));
        q.set_mask = keys.size() - 1;
        q.top_values = walk.d.upload(values);
    }
    q.top_last_unit = last_unit;
    size_t const width = size_t(inputs.steps) + 1;
    size_t const all_units = inputs.units.size();
    if (rows == nullptr) {
        q.unit_ties =
            walk.d.template allocate<unsigned long long>(all_units);
    } else {
        q.top_rows = walk.d.template allocate<uint32_t>(capacity * width);
        q.top_capacity = capacity;
        q.top_count = walk.d.template allocate<unsigned long long>(1);
    }
    walk.run(progress);
    if (rows == nullptr) {
        std::vector<unsigned long long> ties(all_units);
        walk.d.download(q.unit_ties, &ties);
        unit_ties->assign(ties.begin(), ties.end());
        return true;
    }
    std::vector<unsigned long long> count(1);
    walk.d.download(q.top_count, &count);
    if (count[0] > capacity) return false;
    rows->resize(size_t(count[0]) * width);
    walk.d.download(q.top_rows, rows);
    return true;
}

}  // namespace

bool run_distinct_rests_gpu(std::string_view program,
                            GpuLeftoverInputs const& inputs,
                            size_t first_unit, size_t units, size_t rests,
                            std::vector<uint64_t>* out,
                            std::vector<uint64_t>* rows, size_t* found) {
    program_name = program;
    check(cudaSetDevice(0));
    size_t free = 0;
    size_t total = 0;
    check(cudaMemGetInfo(&free, &total));
    size_t const room = free > SET_RESERVE ? free - SET_RESERVE : 0;
    size_t const slot_bytes = sizeof(uint64_t) * (rows != nullptr ? 2 : 1);
    size_t slots = size_t(1) << 16;
    while (slots < 2 * rests && 2 * slots * slot_bytes <= room) slots *= 2;
    Packing const packing(inputs.bag);
    size_t count = 0;
    uint64_t const dropped = packing.bits > 64
        ? walk_distinct_rests<Key128>(inputs, packing, slots, first_unit,
                                      units, out, rows, &count)
        : walk_distinct_rests<Key64>(inputs, packing, slots, first_unit,
                                     units, out, rows, &count);
    if (found != nullptr) *found = count;
    return dropped == 0 && count <= rests;
}

size_t gpu_rest_set_capacity(bool rows) {
    check(cudaSetDevice(0));
    size_t free = 0;
    size_t total = 0;
    check(cudaMemGetInfo(&free, &total));
    size_t const room = free > SET_RESERVE ? free - SET_RESERVE : 0;
    size_t const slot_bytes = sizeof(uint64_t) * (rows ? 2 : 1);
    size_t slots = size_t(1) << 16;
    while (2 * slots * slot_bytes <= room) slots *= 2;
    return slots / 2;
}

bool run_top_rows_gpu(std::string_view program,
                      GpuLeftoverInputs const& inputs, size_t first_unit,
                      size_t units, std::vector<uint64_t> const& keys,
                      std::vector<uint32_t> const& values, uint32_t last_unit,
                      size_t capacity, std::vector<uint32_t>* rows,
                      std::vector<uint64_t>* unit_ties) {
    program_name = program;
    check(cudaSetDevice(0));
    Packing const packing(inputs.bag);
    return packing.bits > 64
        ? walk_top_rows<Key128>(inputs, packing, first_unit, units, keys,
                                values, last_unit, capacity, rows, unit_ties)
        : walk_top_rows<Key64>(inputs, packing, first_unit, units, keys,
                               values, last_unit, capacity, rows, unit_ties);
}
