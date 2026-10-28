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

constexpr int THREADS = 1024;
constexpr int FILTER_BITS = 1 << 18;
constexpr int FILTER_WORDS = FILTER_BITS / 32;
constexpr int MAX_HALF = GPU_MAX_LETTERS / 2;
constexpr size_t SHARED_BYTES_MAX = 101376;
constexpr int STREAMS = 2;
constexpr int FILTER_BLOCK_WORDS = 8;
constexpr int FILTER_BITS_PER_PAIR = 12;
constexpr size_t FILTER_RESERVE = size_t(512) << 20;
constexpr int BUILD_THREADS = 256;

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
constexpr int LIST_CAPACITY = sizeof(Key) == 8 ? 4096 : 2048;

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

__device__ inline int bit_width(unsigned value) {
    return 32 - __clz(value);
}

// A group's slot in a block's hash: its rest-local key and index, and the
// stamp of the row that wrote it.
template <typename Key>
struct Slot {
    Key key;
    uint32_t index;
    uint32_t stamp;
};

// What one block keeps in shared memory for its row.
template <typename RestKey>
struct Shared {
    uint32_t filter[FILTER_WORDS];
    RestKey list_keys[LIST_CAPACITY<RestKey>];
    uint32_t list_indexes[LIST_CAPACITY<RestKey>];
    int pos[MAX_HALF + 2];
    int fill[MAX_HALF + 2];
    unsigned long long counts[6];
    RestKey rest;
    RestKey guards;
    uint8_t letters[GPU_LETTERS_BYTES];
    uint8_t present[26];
    uint8_t shifts[26];
    int present_count;
    int length;
    int row;
    int next_word;
    int words;
};

template <typename BagKey>
struct Params {
    BagKey const* group_keys;
    uint8_t const* group_letters;
    uint8_t const* group_lengths;
    uint32_t const* group_sizes;
    int const* lenstart;
    BagKey const* word_keys;
    uint8_t const* word_letters;
    uint8_t const* word_lengths;
    int word_count;
    BagKey bag_guards;
    uint8_t bag_shifts[26];
    int min_group_length;
    int words;
    unsigned long long min_pairs;
    uint8_t const* rests;
    int rows;
    uint32_t stamp_base;
    int* next;
    GpuCounts* out;
    Slot<BagKey>* hash;
    size_t hash_slots;
    void* list_keys;
    uint32_t* list_indexes;
    size_t list_capacity;
    void* word_list_keys;
    uint8_t* word_list_lengths;
    BagKey* word_list_bag_keys;
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

template <typename RestKey>
__device__ inline RestKey pack_rest(Shared<RestKey> const& s,
                                    uint8_t const* letters) {
    RestKey out = 0;
    for (int k = 0; k < s.present_count; ++k)
        out |= RestKey(letters[s.present[k]]) << s.shifts[k];
    return out;
}

template <typename RestKey>
__device__ inline bool filter_has(Shared<RestKey> const& s, uint64_t hash) {
    unsigned const b1 = unsigned(hash) & (FILTER_BITS - 1);
    unsigned const b2 = unsigned(hash >> 32) & (FILTER_BITS - 1);
    return (s.filter[b1 >> 5] >> (b1 & 31)) & (s.filter[b2 >> 5] >> (b2 & 31)) &
        1;
}

__device__ inline size_t home_slot(uint64_t hash, size_t slots) {
    return size_t(__umul64hi(hash, slots));
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
        for (int offset = 16; offset > 0; offset >>= 1)
            pairs += __shfl_xor_sync(~0u, pairs, offset);
        if (lane == 0 && pairs != 0) atomicAdd(p.pairs, pairs);
    }
}

template <typename BagKey, typename RestKey>
__device__ inline int find_group(Slot<BagKey> const* hash, size_t slots,
                                 uint32_t stamp, RestKey key, uint64_t mixed) {
    for (size_t i = home_slot(mixed, slots);; i = i + 1 == slots ? 0 : i + 1) {
        Slot<BagKey> const& slot = hash[i];
        if (slot.stamp != stamp) return -1;
        if (slot.key == BagKey(key)) return int(slot.index);
    }
}

template <typename BagKey, typename RestKey>
__device__ void count_leftover(Params<BagKey> const& p,
                               Shared<RestKey> const& s,
                               RestKey const* list_keys,
                               uint32_t const* list_indexes,
                               Slot<BagKey> const* hash, uint32_t stamp,
                               RestKey leftover, BagKey bag_leftover,
                               int length, int lane, GpuCounts* counts,
                               GpuFilterCounts* filtered) {
    ++counts->word_bags;
    if (length == 0) {
        ++counts->solo;
        return;
    }
    if (length < 2 * p.min_group_length) return;
    ++filtered->leftovers;
    uint64_t const sum_hash = mix_key(bag_leftover);
    bool bit = true;
    if (lane < FILTER_BLOCK_WORDS) {
        uint32_t const word =
            p.filter[home_slot(sum_hash, p.filter_blocks) * FILTER_BLOCK_WORDS +
                     lane];
        bit = (word & filter_bit(sum_hash, lane)) != 0;
    }
    if (__ballot_sync(~0u, bit) != ~0u) return;
    ++filtered->scanned;
    int const end = s.pos[length / 2 + 1];
    RestKey const pool = leftover | s.guards;
    unsigned long long pairs = 0;
    for (int k = lane; k < end; k += 32) {
        RestKey const a = list_keys[k];
        RestKey r;
        if (!fits(pool, a, s.guards, &r)) continue;
        uint32_t const i = list_indexes[k];
        if (r == a) {
            unsigned long long const size = p.group_sizes[i];
            pairs += size * (size - 1) / 2;
            continue;
        }
        uint64_t const mixed = mix_key(r);
        if (!filter_has(s, mixed)) continue;
        int const b = find_group(hash, p.hash_slots, stamp, r, mixed);
        if (b >= 0 && (2 * p.group_lengths[i] < length || uint32_t(b) > i))
            pairs += (unsigned long long)p.group_sizes[i] * p.group_sizes[b];
    }
    for (int offset = 16; offset > 0; offset >>= 1)
        pairs += __shfl_xor_sync(~0u, pairs, offset);
    if (pairs == 0) return;
    ++counts->with_pairs;
    counts->total_pairs += pairs;
    if (pairs > counts->max_pairs) counts->max_pairs = pairs;
    counts->with_min_pairs += pairs >= p.min_pairs;
}

template <typename BagKey, typename RestKey>
__global__ void __launch_bounds__(THREADS, 1)
count_rows(Params<BagKey> p) {
    static_assert(sizeof(Shared<RestKey>) <= SHARED_BYTES_MAX);
    extern __shared__ __align__(16) unsigned char shared_bytes[];
    Shared<RestKey>& s = *reinterpret_cast<Shared<RestKey>*>(shared_bytes);
    int const tid = threadIdx.x;
    int const lane = tid & 31;
    Slot<BagKey>* const hash = p.hash + blockIdx.x * p.hash_slots;
    RestKey* const big_keys = reinterpret_cast<RestKey*>(p.list_keys) +
        blockIdx.x * p.list_capacity;
    uint32_t* const big_indexes =
        p.list_indexes + blockIdx.x * p.list_capacity;
    RestKey* const word_keys = reinterpret_cast<RestKey*>(p.word_list_keys) +
        size_t(blockIdx.x) * p.word_count;
    uint8_t* const word_lengths =
        p.word_list_lengths + size_t(blockIdx.x) * p.word_count;
    BagKey* const word_bag_keys =
        p.word_list_bag_keys + size_t(blockIdx.x) * p.word_count;

    GpuFilterCounts filtered{};
    if (tid < 6) s.counts[tid] = 0;
    while (true) {
        if (tid == 0) {
            s.row = atomicAdd(p.next, 1);
            s.next_word = 0;
            s.words = 0;
        }
        if (tid < MAX_HALF + 2) s.fill[tid] = 0;
        __syncthreads();
        int const row = s.row;
        if (row >= p.rows) {
            if (lane == 0) {
                atomicAdd(&p.filter_counts[0],
                          (unsigned long long)filtered.leftovers);
                atomicAdd(&p.filter_counts[1],
                          (unsigned long long)filtered.scanned);
            }
            return;
        }
        if (tid < GPU_LETTERS_BYTES)
            s.letters[tid] = p.rests[size_t(row) * GPU_LETTERS_BYTES + tid];
        for (int w = tid; w < FILTER_WORDS; w += THREADS) s.filter[w] = 0;
        __syncthreads();
        if (tid == 0) {
            int bits = 0;
            int count = 0;
            int length = 0;
            RestKey guards = 0;
            for (int letter = 0; letter < 26; ++letter) {
                unsigned const value = s.letters[letter];
                if (value == 0) continue;
                int const width = bit_width(value) + 1;
                s.present[count] = uint8_t(letter);
                s.shifts[count] = uint8_t(bits);
                guards |= RestKey(1) << (bits + width - 1);
                bits += width;
                length += int(value);
                ++count;
            }
            s.present_count = count;
            s.length = length;
            s.guards = guards;
        }
        __syncthreads();
        if (tid == 0) s.rest = pack_rest(s, s.letters);
        int const length = s.length;
        uint32_t const stamp = p.stamp_base + uint32_t(row);
        BagKey y = 0;
        for (int letter = 0; letter < 26; ++letter)
            y |= BagKey(s.letters[letter]) << p.bag_shifts[letter];
        BagKey const y_pool = y | p.bag_guards;

        int const half = length / 2;
        bool const pairs_possible = length >= 2 * p.min_group_length;
        int const half_end = pairs_possible ? p.lenstart[half + 1] : 0;
        if (pairs_possible) {
            int const end = p.lenstart[length - p.min_group_length + 1];
            for (int i = tid; i < end; i += THREADS) {
                BagKey left;
                if (!fits(y_pool, p.group_keys[i], p.bag_guards, &left))
                    continue;
                RestKey const key =
                    pack_rest(s, p.group_letters + size_t(i) * GPU_LETTERS_BYTES);
                uint64_t const mixed = mix_key(key);
                unsigned const b1 = unsigned(mixed) & (FILTER_BITS - 1);
                unsigned const b2 = unsigned(mixed >> 32) & (FILTER_BITS - 1);
                atomicOr(&s.filter[b1 >> 5], 1u << (b1 & 31));
                atomicOr(&s.filter[b2 >> 5], 1u << (b2 & 31));
                for (size_t k = home_slot(mixed, p.hash_slots);;
                     k = k + 1 == p.hash_slots ? 0 : k + 1) {
                    uint32_t const seen =
                        *reinterpret_cast<uint32_t volatile*>(&hash[k].stamp);
                    if (seen == stamp ||
                        atomicCAS(&hash[k].stamp, seen, stamp) != seen)
                        continue;
                    hash[k].key = BagKey(key);
                    hash[k].index = uint32_t(i);
                    break;
                }
                if (i < half_end) atomicAdd(&s.fill[p.group_lengths[i]], 1);
            }
        }
        __syncthreads();
        if (tid == 0) {
            s.pos[0] = 0;
            for (int l = 0; l <= half; ++l) {
                s.pos[l + 1] = s.pos[l] + s.fill[l];
                s.fill[l] = s.pos[l];
            }
        }
        __syncthreads();
        bool const big = s.pos[half + 1] > LIST_CAPACITY<RestKey>;
        RestKey* const list_keys = big ? big_keys : s.list_keys;
        uint32_t* const list_indexes = big ? big_indexes : s.list_indexes;
        for (int i = tid; i < half_end; i += THREADS) {
            BagKey left;
            if (!fits(y_pool, p.group_keys[i], p.bag_guards, &left)) continue;
            int const k = atomicAdd(&s.fill[p.group_lengths[i]], 1);
            list_keys[k] =
                pack_rest(s, p.group_letters + size_t(i) * GPU_LETTERS_BYTES);
            list_indexes[k] = uint32_t(i);
        }
        for (int j = tid; j < p.word_count; j += THREADS) {
            BagKey left;
            if (!fits(y_pool, p.word_keys[j], p.bag_guards, &left)) continue;
            int const k = atomicAdd(&s.words, 1);
            word_keys[k] =
                pack_rest(s, p.word_letters + size_t(j) * GPU_LETTERS_BYTES);
            word_lengths[k] = p.word_lengths[j];
            word_bag_keys[k] = p.word_keys[j];
        }
        __syncthreads();

        GpuCounts counts{};
        int const words = s.words;
        RestKey const guards = s.guards;
        RestKey const rest = s.rest;
        while (true) {
            int first = 0;
            if (lane == 0) first = atomicAdd(&s.next_word, 1);
            first = __shfl_sync(~0u, first, 0);
            if (first >= words) break;
            RestKey rests[GPU_MAX_WORDS];
            BagKey bag_rests[GPU_MAX_WORDS];
            int lengths[GPU_MAX_WORDS];
            int picks[GPU_MAX_WORDS];
            fits(rest | guards, word_keys[first], guards, &rests[0]);
            bag_rests[0] = y - word_bag_keys[first];
            lengths[0] = length - word_lengths[first];
            if (p.words == 1) {
                count_leftover(p, s, list_keys, list_indexes, hash, stamp,
                               rests[0], bag_rests[0], lengths[0], lane,
                               &counts, &filtered);
                continue;
            }
            int depth = 1;
            picks[1] = first;
            while (true) {
                if (picks[depth] >= words) {
                    if (--depth == 0) break;
                    ++picks[depth];
                    continue;
                }
                int const j = picks[depth];
                RestKey left;
                if (!fits(rests[depth - 1] | guards, word_keys[j], guards,
                          &left)) {
                    ++picks[depth];
                    continue;
                }
                int const left_length = lengths[depth - 1] - word_lengths[j];
                BagKey const bag_left = bag_rests[depth - 1] - word_bag_keys[j];
                if (depth + 1 == p.words) {
                    count_leftover(p, s, list_keys, list_indexes, hash,
                                   stamp, left, bag_left, left_length, lane,
                                   &counts, &filtered);
                    ++picks[depth];
                    continue;
                }
                rests[depth] = left;
                bag_rests[depth] = bag_left;
                lengths[depth] = left_length;
                ++depth;
                picks[depth] = j;
            }
        }
        if (lane == 0) {
            atomicAdd(&s.counts[0], counts.word_bags);
            atomicAdd(&s.counts[1], counts.with_pairs);
            atomicAdd(&s.counts[2], counts.total_pairs);
            atomicMax(&s.counts[3], counts.max_pairs);
            atomicAdd(&s.counts[4], counts.solo);
            atomicAdd(&s.counts[5], counts.with_min_pairs);
        }
        __syncthreads();
        if (tid == 0) {
            p.out[row] = {s.counts[0], s.counts[1], s.counts[2], s.counts[3],
                          s.counts[4], s.counts[5]};
            for (int k = 0; k < 6; ++k) s.counts[k] = 0;
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

int rest_bits(uint8_t const* letters) {
    int bits = 0;
    for (int letter = 0; letter < 26; ++letter) {
        unsigned value = letters[letter];
        if (value == 0) continue;
        while (value != 0) {
            ++bits;
            value >>= 1;
        }
        ++bits;
    }
    return bits;
}

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
        void* hash = nullptr;
        void* list_keys = nullptr;
        uint32_t* list_indexes = nullptr;
        void* word_keys = nullptr;
        uint8_t* word_lengths = nullptr;
        void* word_bag_keys = nullptr;
        unsigned long long* filter_counts = nullptr;
        uint8_t* host_rests = nullptr;
        GpuCounts* host_out = nullptr;
        unsigned long long* host_filter_counts = nullptr;
        std::vector<uint32_t> order;
        uint32_t next_stamp = 1;
        bool busy = false;
    };

    std::string device;
    int key_bits = 0;
    size_t bytes = 0;
    int blocks = 0;
    int group_count = 0;
    int word_count = 0;
    int words = 0;
    int min_group_length = 0;
    unsigned long long min_pairs = 0;
    uint8_t bag_shifts[26] = {};
    Key128 bag_guards = 0;
    size_t hash_slots = 0;
    size_t list_capacity = 0;
    void* group_keys = nullptr;
    uint8_t* group_letters = nullptr;
    uint8_t* group_lengths = nullptr;
    uint32_t* group_sizes = nullptr;
    int* lenstart = nullptr;
    void* word_keys = nullptr;
    uint8_t* word_letters = nullptr;
    uint8_t* word_lengths = nullptr;
    uint32_t* filter = nullptr;
    size_t filter_blocks = 0;
    GpuFilterInfo filter_info{};
    GpuFilterCounts filter_counts{};
    Stream streams[STREAMS];
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
        stream->next = allocate<int>(2);
        stream->hash = allocate<Slot<Key>>(size_t(blocks) * hash_slots);
        check(cudaMemset(stream->hash, 0,
                         size_t(blocks) * hash_slots * sizeof(Slot<Key>)));
        stream->list_keys = allocate<Key>(size_t(blocks) * list_capacity);
        stream->list_indexes =
            allocate<uint32_t>(size_t(blocks) * list_capacity);
        stream->word_keys = allocate<Key>(size_t(blocks) * word_count);
        stream->word_lengths = allocate<uint8_t>(size_t(blocks) * word_count);
        stream->word_bag_keys = allocate<Key>(size_t(blocks) * word_count);
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

    template <typename BagKey, typename RestKey>
    void launch(Stream* stream, size_t first, size_t rows, uint32_t stamp,
                int* next) {
        Params<BagKey> p{};
        p.group_keys = static_cast<BagKey const*>(group_keys);
        p.group_letters = group_letters;
        p.group_lengths = group_lengths;
        p.group_sizes = group_sizes;
        p.lenstart = lenstart;
        p.word_keys = static_cast<BagKey const*>(word_keys);
        p.word_letters = word_letters;
        p.word_lengths = word_lengths;
        p.word_count = word_count;
        p.bag_guards = BagKey(bag_guards);
        memcpy(p.bag_shifts, bag_shifts, sizeof bag_shifts);
        p.min_group_length = min_group_length;
        p.words = words;
        p.min_pairs = min_pairs;
        p.rests = stream->rests + first * GPU_LETTERS_BYTES;
        p.rows = int(rows);
        p.stamp_base = stamp;
        p.next = next;
        p.out = stream->out + first;
        p.hash = static_cast<Slot<BagKey>*>(stream->hash);
        p.hash_slots = hash_slots;
        p.list_keys = stream->list_keys;
        p.list_indexes = stream->list_indexes;
        p.list_capacity = list_capacity;
        p.word_list_keys = stream->word_keys;
        p.word_list_lengths = stream->word_lengths;
        p.word_list_bag_keys = static_cast<BagKey*>(stream->word_bag_keys);
        p.filter = filter;
        p.filter_blocks = filter_blocks;
        p.filter_counts = stream->filter_counts;
        count_rows<BagKey, RestKey>
            <<<blocks, THREADS, sizeof(Shared<RestKey>), stream->stream>>>(p);
        check(cudaGetLastError());
    }

    template <typename BagKey, typename RestKey>
    static void allow_shared() {
        check(cudaFuncSetAttribute(count_rows<BagKey, RestKey>,
                                   cudaFuncAttributeMaxDynamicSharedMemorySize,
                                   int(sizeof(Shared<RestKey>))));
    }

    template <typename Key>
    size_t slot_bytes() const {
        return size_t(blocks) * hash_slots * sizeof(Slot<Key>);
    }
};

GpuRowCounter::GpuRowCounter(std::string_view program, GpuInputs const& inputs)
    : impl_(std::make_unique<Impl>()) {
    program_name = program;
    Impl& impl = *impl_;
    int device_count = 0;
    check(cudaGetDeviceCount(&device_count));
    check(cudaSetDevice(0));
    cudaDeviceProp properties;
    check(cudaGetDeviceProperties(&properties, 0));
    impl.device = properties.name;
    impl.blocks = properties.multiProcessorCount;

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
    impl.hash_slots = 2 * size_t(impl.group_count) + 1;
    impl.list_capacity = size_t(lenstart[letters / 2 + 1]);

    impl.group_letters = impl.upload(inputs.group_letters);
    impl.group_lengths = impl.upload(inputs.group_lengths);
    impl.group_sizes = impl.upload(inputs.group_sizes);
    impl.lenstart = impl.upload(lenstart);
    impl.word_letters = impl.upload(inputs.word_letters);
    impl.word_lengths = impl.upload(inputs.word_lengths);
    if (impl.key_bits == 64) {
        impl.upload_keys<Key64>(inputs, packing);
        for (Impl::Stream& stream : impl.streams)
            impl.allocate_stream<Key64>(&stream);
        impl.build_filter<Key64>(inputs.bag, packing, letters);
        Impl::allow_shared<Key64, Key64>();
    } else {
        impl.upload_keys<Key128>(inputs, packing);
        for (Impl::Stream& stream : impl.streams)
            impl.allocate_stream<Key128>(&stream);
        impl.build_filter<Key128>(inputs.bag, packing, letters);
        Impl::allow_shared<Key128, Key64>();
        Impl::allow_shared<Key128, Key128>();
    }
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
    Impl::Stream* stream;
    {
        std::unique_lock<std::mutex> lock(impl.mutex);
        impl.freed.wait(lock, [&] {
            return !impl.streams[0].busy || !impl.streams[1].busy;
        });
        stream = &impl.streams[impl.streams[0].busy ? 1 : 0];
        stream->busy = true;
    }

    std::vector<uint32_t>& order = stream->order;
    order.resize(rows);
    std::iota(order.begin(), order.end(), 0u);
    std::vector<uint64_t> sort_keys(rows);
    size_t wide = 0;
    for (size_t row = 0; row < rows; ++row) {
        uint8_t const* letters = rests + row * GPU_LETTERS_BYTES;
        bool const is_wide = rest_bits(letters) > 64;
        wide += is_wide;
        sort_keys[row] = uint64_t(is_wide) << 32 |
            uint64_t(GPU_MAX_LETTERS - rest_length(letters));
    }
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return sort_keys[a] < sort_keys[b];
    });
    for (size_t k = 0; k < rows; ++k)
        memcpy(stream->host_rests + k * GPU_LETTERS_BYTES,
               rests + size_t(order[k]) * GPU_LETTERS_BYTES,
               GPU_LETTERS_BYTES);

    cudaStream_t const s = stream->stream;
    if (uint64_t(stream->next_stamp) + rows >= UINT32_MAX) {
        check(cudaMemsetAsync(stream->hash, 0,
                              impl.key_bits == 64
                                  ? impl.slot_bytes<Key64>()
                                  : impl.slot_bytes<Key128>(),
                              s));
        stream->next_stamp = 1;
    }
    uint32_t const stamp = stream->next_stamp;
    stream->next_stamp += uint32_t(rows);
    check(cudaMemcpyAsync(stream->rests, stream->host_rests,
                          rows * GPU_LETTERS_BYTES, cudaMemcpyHostToDevice,
                          s));
    check(cudaMemsetAsync(stream->next, 0, 2 * sizeof(int), s));
    check(cudaMemsetAsync(stream->filter_counts, 0,
                          2 * sizeof(unsigned long long), s));
    size_t const narrow = rows - wide;
    if (impl.key_bits == 64) {
        impl.launch<Key64, Key64>(stream, 0, rows, stamp, stream->next);
    } else {
        if (narrow > 0)
            impl.launch<Key128, Key64>(stream, 0, narrow, stamp,
                                       stream->next);
        if (wide > 0)
            impl.launch<Key128, Key128>(stream, narrow, wide,
                                        stamp + uint32_t(narrow),
                                        stream->next + 1);
    }
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
