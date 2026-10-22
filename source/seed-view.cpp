#include "seed-view.h"

#include "mapped-file.h"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <thread>

namespace {

bool word_passes(std::string_view word, SeedFilter const& filter) {
    int const len = int(word.size());
    if (len < filter.min_len || len > filter.max_len) return false;
    return !filter.dictionary ||
        filter.dictionary->count(std::string(word)) != 0;
}

size_t count_lines(char const* begin, char const* end) {
    if (begin == end) return 0;
    return size_t(std::count(begin, end, '\n')) + (end[-1] != '\n');
}

// Reads the lines of [begin, end) into `out`, one per element. False, with
// `*bad` at the start of the first line that isn't two comma-separated
// words.
bool read_chunk(char const* begin, char const* end, P1Index const* p1,
                SeedPair* out, char const** bad) {
    for (char const* start = begin; start < end;) {
        char const* newline =
            static_cast<char const*>(memchr(start, '\n', size_t(end - start)));
        if (newline == nullptr) newline = end;
        std::string_view const line(start, size_t(newline - start));
        size_t const comma = line.find(',');
        if (comma == 0 || comma == std::string_view::npos ||
            comma + 1 == line.size() ||
            line.find(',', comma + 1) != std::string_view::npos) {
            *bad = start;
            return false;
        }
        std::string_view const a = line.substr(0, comma);
        std::string_view const b = line.substr(comma + 1);
        P1Yes yes;
        if (!p1 || !p1->yes(a, b, &yes)) {
            *out++ = {std::string(line), -1, uint16_t(comma)};
        } else if (yes.ba > yes.ab) {
            std::string swapped;
            swapped.reserve(line.size());
            swapped.append(b).append(1, ',').append(a);
            *out++ = {std::move(swapped), float(yes.ba), uint16_t(b.size())};
        } else {
            *out++ = {std::string(line), float(yes.ab), uint16_t(comma)};
        }
        start = newline + 1;
    }
    return true;
}

}  // namespace

bool read_seed_pairs(std::filesystem::path const& pairs,
                     P1Index const* p1, size_t threads,
                     std::vector<SeedPair>* out, std::string* error) {
    std::error_code code;
    if (!std::filesystem::is_regular_file(pairs, code)) {
        *error = "\"" + pairs.string() + "\" isn't a regular file";
        return false;
    }
    MappedFile file;
    if (!file.open(pairs, error)) return false;
    char const* const data = file.data();
    size_t const size = file.size();

    threads = std::max<size_t>(1, threads);
    std::vector<char const*> bounds{data};
    for (size_t t = 1; t < threads; ++t) {
        char const* bound = std::max(data + size * t / threads, bounds.back());
        if (bound > data && bound < data + size && bound[-1] != '\n') {
            char const* const newline = static_cast<char const*>(
                memchr(bound, '\n', size_t(data + size - bound)));
            bound = newline == nullptr ? data + size : newline + 1;
        }
        bounds.push_back(bound);
    }
    bounds.push_back(data + size);

    auto const run = [&](auto const& work) {
        std::vector<std::thread> workers;
        for (size_t t = 1; t < threads; ++t) workers.emplace_back(work, t);
        work(0);
        for (std::thread& worker : workers) worker.join();
    };
    std::vector<size_t> starts(threads + 1, 0);
    run([&](size_t t) {
        starts[t + 1] = count_lines(bounds[t], bounds[t + 1]);
    });
    for (size_t t = 0; t < threads; ++t) starts[t + 1] += starts[t];

    out->clear();
    out->resize(starts[threads]);
    std::vector<char const*> bad(threads, nullptr);
    run([&](size_t t) {
        read_chunk(bounds[t], bounds[t + 1], p1, out->data() + starts[t],
                   &bad[t]);
    });
    for (char const* const line : bad) {
        if (line == nullptr) continue;
        *error = "\"" + pairs.string() + "\" line " +
            std::to_string(std::count(data, line, '\n') + 1) +
            ": expected two comma-separated words";
        out->clear();
        return false;
    }
    return true;
}

bool seed_pair_passes(SeedPair const& pair, SeedFilter const& filter) {
    if (filter.min_yes && (pair.yes < 0 || pair.yes < *filter.min_yes))
        return false;
    std::string_view const line = pair.line;
    return word_passes(line.substr(0, pair.comma), filter) &&
        word_passes(line.substr(pair.comma + 1), filter);
}
