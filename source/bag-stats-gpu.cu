#include "bag-stats-gpu.h"
#include "log.h"

#include <cuda_runtime.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <numeric>
#include <string>
#include <string_view>

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
constexpr size_t FILTER_RESERVE = size_t(512) << 20;
constexpr int BUILD_THREADS = 256;

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
};

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

// One warp per group `a`: counts the groups `b` from `a` on whose sum with
// `a` fits the bag, or sets that sum's bits in the filter.
template <typename BagKey, bool INSERT>
__global__ void pair_sums(PairSumParams<BagKey> p) {
    int const a = int((size_t(blockIdx.x) * blockDim.x + threadIdx.x) >> 5);
    int const lane = threadIdx.x & 31;
    if (a >= p.group_count) return;
    BagKey const key = p.group_keys[a];
    BagKey left;
    if (!fits(p.bag | p.bag_guards, key, p.bag_guards, &left)) return;
    BagKey const pool = left | p.bag_guards;
    int const end = p.lenstart[p.bag_length - p.group_lengths[a] + 1];
    bool const twice = p.group_sizes[a] >= 2;
    unsigned long long pairs = 0;
    for (int b = a + lane; b < end; b += 32) {
        if (b == a && !twice) continue;
        BagKey rest;
        if (!fits(pool, p.group_keys[b], p.bag_guards, &rest)) continue;
        if constexpr (INSERT)
            filter_insert(p.filter, p.filter_blocks,
                          mix_key(key + p.group_keys[b]));
        else
            ++pairs;
    }
    if constexpr (!INSERT) {
        pairs = warp_sum(pairs);
        if (lane == 0 && pairs != 0) atomicAdd(p.pairs, pairs);
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
            if (length < 2 * p.min_group_length) continue;
            ++leftovers;
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
        __syncthreads();
        if (tid == 0) {
            p.out[row] = {s.word_bags, 0, 0, 0, s.solo, 0};
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
                    passes(p, key)) {
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
        size_t const word_entries =
            size_t(leftover_blocks + pair_blocks) * word_count;
        stream->word_keys = allocate<Key>(word_entries);
        stream->word_lengths = allocate<uint8_t>(word_entries);
        stream->list_keys = allocate<Key>(size_t(pair_blocks) * list_capacity);
        stream->list_indexes =
            allocate<uint32_t>(size_t(pair_blocks) * list_capacity);
        stream->filter_counts = allocate<unsigned long long>(2);
        check(cudaMallocHost(&stream->host_rests,
                             GPU_BATCH_ROWS * GPU_LETTERS_BYTES));
        check(cudaMallocHost(&stream->host_out,
                             GPU_BATCH_ROWS * sizeof(GpuCounts)));
        check(cudaMallocHost(&stream->host_filter_counts,
                             2 * sizeof(unsigned long long)));
    }

    template <typename Key>
    void build_filter(uint8_t const* bag, Packing const& packing,
                      int letters) {
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
        int const grid = (group_count + BUILD_THREADS / 32 - 1) /
            (BUILD_THREADS / 32);
        if (grid > 0) pair_sums<Key, false><<<grid, BUILD_THREADS>>>(p);
        check(cudaGetLastError());
        unsigned long long pairs = 0;
        check(cudaMemcpy(&pairs, p.pairs, sizeof pairs,
                         cudaMemcpyDeviceToHost));

        size_t free = 0;
        size_t total = 0;
        check(cudaMemGetInfo(&free, &total));
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
        if (grid > 0) pair_sums<Key, true><<<grid, BUILD_THREADS>>>(p);
        check(cudaGetLastError());
        check(cudaDeviceSynchronize());
        filter_info = {
            pairs, filter_bytes,
            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          start)
                .count()};
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
        count_leftovers<Key><<<leftover_blocks, LEFTOVER_THREADS,
                               sizeof(LeftoverShared<Key>), stream->stream>>>(
            p);
        check(cudaGetLastError());
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
        for (Stream& stream : streams) allocate_stream<Key>(&stream);
        build_filter<Key>(inputs.bag, packing, letters);
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
