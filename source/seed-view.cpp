#include "seed-view.h"

#include "log.h"
#include "mapped-file.h"

#include <algorithm>
#include <bitset>
#include <cstring>
#include <map>
#include <set>
#include <string_view>
#include <thread>
#include <utility>

namespace {

bool word_passes(std::string_view word, SeedFilter const& filter) {
    int const len = int(word.size());
    if (len < filter.min_len || len > filter.max_len) return false;
    return !filter.dictionary ||
        filter.dictionary->count(std::string(word)) != 0;
}

using WordPair = std::pair<std::string_view, std::string_view>;

// The words of `pair`, the smaller first.
WordPair sorted_words(SeedPair const& pair) {
    std::string_view const line = pair.line;
    std::string_view const a = line.substr(0, pair.comma);
    std::string_view const b = line.substr(pair.comma + 1);
    return a < b ? WordPair{a, b} : WordPair{b, a};
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

bool add_forced_pairs(std::filesystem::path const& forced,
                      P1Index const* p1, std::vector<SeedPair>* pairs,
                      std::string* error) {
    std::error_code code;
    if (!std::filesystem::exists(forced, code)) return true;
    std::vector<SeedPair> listed;
    if (!read_seed_pairs(forced, p1, 1, &listed, error)) return false;
    std::map<WordPair, bool> found;
    std::vector<std::bitset<256>> starts;
    auto const start =
        [&](std::string_view word) -> std::bitset<256>::reference {
        if (starts.size() <= word.size()) starts.resize(word.size() + 1);
        return starts[word.size()][uint8_t(word[0])];
    };
    for (SeedPair const& pair : listed) {
        WordPair const key = sorted_words(pair);
        found.emplace(key, false);
        start(key.first) = true;
        start(key.second) = true;
    }
    for (SeedPair& pair : *pairs) {
        if (pair.comma >= starts.size() ||
            !starts[pair.comma][uint8_t(pair.line[0])])
            continue;
        auto const it = found.find(sorted_words(pair));
        if (it == found.end()) continue;
        pair.forced = true;
        it->second = true;
    }
    pairs->reserve(pairs->size() + size_t(std::ranges::count(
        found, false, &std::pair<WordPair const, bool>::second)));
    for (SeedPair& pair : listed) {
        bool& seen = found.find(sorted_words(pair))->second;
        if (seen) continue;
        seen = true;
        pair.forced = true;
        pairs->push_back(pair);
    }
    return true;
}

bool valid_seed_sentence(std::string_view sentence) {
    return sentence.size() == 2 && sentence[0] == 's' &&
        sentence[1] >= '1' && sentence[1] <= '9';
}

std::filesystem::path forced_pairs_path(std::filesystem::path const& directory,
                                        std::string_view sentence) {
    if (!valid_seed_sentence(sentence)) return {};
    return directory / (std::string(sentence) + ".pairs");
}

void warn_forced_words_missing(std::string_view program,
                               std::vector<SeedPair> const& pairs,
                               std::filesystem::path const& forced,
                               DfsDictionary const& dictionary,
                               std::filesystem::path const& dictionary_path) {
    std::set<std::string> missing;
    for (SeedPair const& pair : pairs) {
        if (!pair.forced) continue;
        std::string_view const line = pair.line;
        for (std::string_view const word :
                 {line.substr(0, pair.comma), line.substr(pair.comma + 1)})
            if (!dictionary.contains(std::string(word)))
                missing.emplace(word);
    }
    for (std::string const& word : missing)
        alert(program, "forced pair word \"%s\" of %s isn't in %s",
              word.c_str(), forced.c_str(), dictionary_path.c_str());
}

bool seed_pair_passes(SeedPair const& pair, SeedFilter const& filter) {
    if (filter.min_yes && !pair.forced &&
        (pair.yes < 0 || pair.yes < *filter.min_yes))
        return false;
    std::string_view const line = pair.line;
    return word_passes(line.substr(0, pair.comma), filter) &&
        word_passes(line.substr(pair.comma + 1), filter);
}
