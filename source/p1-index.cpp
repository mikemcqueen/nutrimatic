#include "p1-index.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr char MAGIC[8] = {'N', 'M', 'P', '1', 'I', 'D', 'X', '\n'};

struct Header {
    char magic[8];
    uint32_t version;
    uint32_t reserved;
    uint64_t source_text;
    uint64_t words;
    uint64_t word_text;
    uint64_t pairs;
    uint64_t padding[2];
};
static_assert(sizeof(Header) == 64);

size_t padded(size_t size) { return (size + 7) & ~size_t(7); }

long long mtime_ns(struct stat const& info) {
    return (long long)info.st_mtim.tv_sec * 1000000000LL +
        info.st_mtim.tv_nsec;
}

uint16_t quantize(double yes) {
    return uint16_t(std::lround(std::clamp(yes, 0.0, 1.0) * 65535));
}

// The YES probability in the `direction` list of a phase 1 line, 0 when it
// lists none.
double yes_after(std::string const& line, char const* direction) {
    size_t const start = line.find(direction);
    if (start == std::string::npos) return 0;
    size_t const end = line.find(']', start);
    size_t const yes = line.find("\"YES\":", start);
    if (yes == std::string::npos || yes > end) return 0;
    return strtod(line.c_str() + yes + 6, NULL);
}

struct Record {
    uint32_t a;
    uint32_t b;
    uint16_t ab;
    uint16_t ba;
};

struct Entry {
    uint64_t key;
    uint16_t low_high;
    uint16_t high_low;
};

struct WordHash {
    using is_transparent = void;
    size_t operator()(std::string_view word) const {
        return std::hash<std::string_view>()(word);
    }
};

// One reading thread's words and records, the records' ids local to it.
struct Part {
    std::unordered_map<std::string, uint32_t, WordHash, std::equal_to<>>
        words;
    std::vector<Record> records;

    uint32_t id(std::string_view word) {
        auto const found = words.find(word);
        if (found != words.end()) return found->second;
        uint32_t const id = uint32_t(words.size());
        words.emplace(std::string(word), id);
        return id;
    }
};

bool read_file(std::filesystem::path const& path, Part* part,
               std::string* error) {
    std::ifstream in(path);
    if (!in) {
        *error = path.string() + ": can't read";
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        size_t const start = line.find("\"pair\": \"");
        if (start == std::string::npos) continue;
        size_t const begin = start + 9;
        size_t const end = line.find('"', begin);
        if (end == std::string::npos) continue;
        std::string_view const pair(line.data() + begin, end - begin);
        size_t const comma = pair.find(',');
        if (comma == std::string_view::npos) continue;
        part->records.push_back({part->id(pair.substr(0, comma)),
                                 part->id(pair.substr(comma + 1)),
                                 quantize(yes_after(line, "\"fwd\"")),
                                 quantize(yes_after(line, "\"rvs\""))});
    }
    return true;
}

bool write_all(FILE* out, void const* data, size_t size) {
    static char const zeros[8] = {};
    return fwrite(data, 1, size, out) == size &&
        fwrite(zeros, 1, padded(size) - size, out) == padded(size) - size;
}

}  // namespace

bool build_p1_index(std::string const& dir, std::string const& out,
                    size_t threads, std::string* error) {
    std::error_code code;
    std::filesystem::path const root = std::filesystem::absolute(dir, code);
    std::vector<std::filesystem::path> files;
    for (auto const& entry : std::filesystem::directory_iterator(root, code))
        if (entry.path().extension() == ".jsonl") files.push_back(entry.path());
    if (code) {
        *error = dir + ": " + code.message();
        return false;
    }
    std::sort(files.begin(), files.end());

    std::string sources = root.string() + "\n";
    for (std::filesystem::path const& file : files) {
        struct stat info;
        if (stat(file.c_str(), &info) != 0) {
            *error = file.string() + ": can't stat";
            return false;
        }
        sources += file.filename().string() + "\t" +
            std::to_string((long long)info.st_size) + "\t" +
            std::to_string(mtime_ns(info)) + "\n";
    }

    threads = std::max<size_t>(1, std::min(threads, files.size()));
    std::vector<Part> parts(threads);
    std::atomic<size_t> next{0};
    std::mutex mutex;
    std::string failure;
    auto const work = [&](size_t t) {
        for (size_t f; (f = next.fetch_add(1)) < files.size();) {
            std::string message;
            if (read_file(files[f], &parts[t], &message)) continue;
            std::lock_guard<std::mutex> lock(mutex);
            failure = message;
            return;
        }
    };
    std::vector<std::thread> workers;
    for (size_t t = 1; t < threads; ++t) workers.emplace_back(work, t);
    work(0);
    for (std::thread& worker : workers) worker.join();
    if (!failure.empty()) {
        *error = failure;
        return false;
    }

    std::vector<std::string> words;
    for (Part const& part : parts)
        for (auto const& [word, id] : part.words) words.push_back(word);
    std::sort(words.begin(), words.end());
    words.erase(std::unique(words.begin(), words.end()), words.end());

    std::vector<Entry> entries;
    for (Part& part : parts) {
        std::vector<uint32_t> global(part.words.size());
        for (auto const& [word, id] : part.words)
            global[id] = uint32_t(
                std::lower_bound(words.begin(), words.end(), word) -
                words.begin());
        for (Record const& record : part.records) {
            uint64_t const a = global[record.a];
            uint64_t const b = global[record.b];
            if (a <= b) entries.push_back({a << 32 | b, record.ab, record.ba});
            else entries.push_back({b << 32 | a, record.ba, record.ab});
        }
        part = Part();
    }
    std::sort(entries.begin(), entries.end(),
              [](Entry const& x, Entry const& y) { return x.key < y.key; });
    size_t kept = 0;
    for (Entry const& entry : entries) {
        if (kept > 0 && entries[kept - 1].key == entry.key) {
            entries[kept - 1].low_high =
                std::max(entries[kept - 1].low_high, entry.low_high);
            entries[kept - 1].high_low =
                std::max(entries[kept - 1].high_low, entry.high_low);
        } else {
            entries[kept++] = entry;
        }
    }
    entries.resize(kept);

    std::vector<uint32_t> offsets{0};
    std::string text;
    for (std::string const& word : words) {
        text += word;
        offsets.push_back(uint32_t(text.size()));
    }
    std::vector<uint64_t> keys;
    std::vector<uint16_t> values;
    keys.reserve(entries.size());
    values.reserve(2 * entries.size());
    for (Entry const& entry : entries) {
        keys.push_back(entry.key);
        values.push_back(entry.low_high);
        values.push_back(entry.high_low);
    }

    Header header{};
    memcpy(header.magic, MAGIC, sizeof(MAGIC));
    header.version = P1_INDEX_VERSION;
    header.source_text = sources.size();
    header.words = words.size();
    header.word_text = text.size();
    header.pairs = keys.size();

    std::string const temporary = out + ".tmp";
    FILE* file = fopen(temporary.c_str(), "wb");
    if (file == NULL) {
        *error = temporary + ": can't write";
        return false;
    }
    bool const written = write_all(file, &header, sizeof(header)) &&
        write_all(file, sources.data(), sources.size()) &&
        write_all(file, offsets.data(), offsets.size() * 4) &&
        write_all(file, text.data(), text.size()) &&
        write_all(file, keys.data(), keys.size() * 8) &&
        write_all(file, values.data(), values.size() * 2);
    if (fclose(file) != 0 || !written ||
        rename(temporary.c_str(), out.c_str()) != 0) {
        remove(temporary.c_str());
        *error = out + ": can't write";
        return false;
    }
    return true;
}

bool P1Index::open(std::string const& path, std::string* error) {
    if (!file_.open(path, error)) return false;
    auto const bad = [&](std::string_view what) {
        *error = (path + ": ").append(what);
        return false;
    };
    Header header;
    if (file_.size() < sizeof(header)) return bad("not a p1 index");
    memcpy(&header, file_.data(), sizeof(header));
    if (memcmp(header.magic, MAGIC, sizeof(MAGIC)) != 0)
        return bad("not a p1 index");
    if (header.version != P1_INDEX_VERSION)
        return bad("not this version of the p1 index; rebuild it with "
                   "make-p1-index");

    size_t offset = sizeof(header);
    size_t const sizes[] = {header.source_text, (header.words + 1) * 4,
                            header.word_text, header.pairs * 8,
                            header.pairs * 4};
    size_t total = offset;
    for (size_t size : sizes) total += padded(size);
    if (total != file_.size()) return bad("truncated");

    std::string_view const sources(file_.data() + offset, header.source_text);
    offset += padded(sizes[0]);
    word_offsets_ = {reinterpret_cast<uint32_t const*>(file_.data() + offset),
                     header.words + 1};
    offset += padded(sizes[1]);
    word_text_ = file_.data() + offset;
    words_ = header.words;
    offset += padded(sizes[2]);
    keys_ = {reinterpret_cast<uint64_t const*>(file_.data() + offset),
             header.pairs};
    offset += padded(sizes[3]);
    values_ = reinterpret_cast<uint16_t const*>(file_.data() + offset);
    if (word_offsets_[0] != 0 || word_offsets_[words_] != header.word_text)
        return bad("word offsets don't agree; rebuild it");
    for (size_t i = 0; i < words_; ++i)
        if (word_offsets_[i] > word_offsets_[i + 1])
            return bad("word offsets don't agree; rebuild it");

    size_t const first = sources.find('\n');
    std::filesystem::path const root(std::string(sources.substr(0, first)));
    size_t listed = 0;
    for (size_t i = first + 1; i < sources.size(); ++listed) {
        size_t const end = sources.find('\n', i);
        std::string const line(sources.substr(i, end - i));
        size_t const tab = line.find('\t');
        size_t const tab2 = line.find('\t', tab + 1);
        std::filesystem::path const source = root / line.substr(0, tab);
        struct stat info;
        if (stat(source.c_str(), &info) != 0 ||
            std::to_string((long long)info.st_size) !=
                line.substr(tab + 1, tab2 - tab - 1) ||
            std::to_string(mtime_ns(info)) != line.substr(tab2 + 1)) {
            *error = path + ": " + source.string() +
                " is missing or has changed; rebuild it with make-p1-index";
            return false;
        }
        i = end + 1;
    }
    std::error_code code;
    size_t present = 0;
    for (auto const& entry : std::filesystem::directory_iterator(root, code))
        present += entry.path().extension() == ".jsonl";
    if (code || present != listed) {
        *error = path + ": " + root.string() +
            " has new .jsonl files; rebuild it with make-p1-index";
        return false;
    }
    return true;
}

int64_t P1Index::word(std::string_view text) const {
    size_t low = 0;
    size_t high = words_;
    while (low < high) {
        size_t const mid = (low + high) / 2;
        std::string_view const word(word_text_ + word_offsets_[mid],
                                    word_offsets_[mid + 1] -
                                        word_offsets_[mid]);
        if (word < text) low = mid + 1;
        else high = mid;
    }
    if (low == words_ ||
        std::string_view(word_text_ + word_offsets_[low],
                         word_offsets_[low + 1] - word_offsets_[low]) != text)
        return -1;
    return int64_t(low);
}

bool P1Index::yes(std::string_view a, std::string_view b, P1Yes* out) const {
    int64_t const id_a = word(a);
    int64_t const id_b = word(b);
    if (id_a < 0 || id_b < 0) return false;
    bool const swapped = id_b < id_a;
    uint64_t const key = swapped ? uint64_t(id_b) << 32 | uint64_t(id_a)
                                 : uint64_t(id_a) << 32 | uint64_t(id_b);
    uint64_t const* found = std::lower_bound(keys_.begin(), keys_.end(), key);
    if (found == keys_.end() || *found != key) return false;
    size_t const i = size_t(found - keys_.begin());
    double const low_high = values_[2 * i] / 65535.0;
    double const high_low = values_[2 * i + 1] / 65535.0;
    out->ab = swapped ? high_low : low_high;
    out->ba = swapped ? low_high : high_low;
    return true;
}
