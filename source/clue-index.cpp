#include "clue-index.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sys/stat.h>

namespace {

bool read_text(std::filesystem::path const& path, std::string* out,
               std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *error = path.string() + ": can't read";
        return false;
    }
    out->assign(std::istreambuf_iterator<char>(in),
                std::istreambuf_iterator<char>());
    return true;
}

// The integer after `"key": ` in metadata.json's text, -1 when missing.
long long metadata_value(std::string const& text, char const* key) {
    size_t const at = text.find(std::string("\"") + key + "\":");
    if (at == std::string::npos) return -1;
    return strtoll(text.c_str() + at + strlen(key) + 3, NULL, 10);
}

bool open_array(std::filesystem::path const& dir, std::string_view name,
                MappedFile* file, auto* array, std::string* error) {
    std::filesystem::path const path = dir / name;
    if (!file->open(path, error)) return false;
    if (npy_array(*file, array, error)) return true;
    *error = path.string() + ": " + *error;
    return false;
}

std::string upper(std::string_view text) {
    std::string out(text);
    for (char& c : out)
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return out;
}

bool word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

// `clue`'s words as clean_words() splits them.
void clue_words(std::string_view clue, std::vector<std::string>* words) {
    words->clear();
    std::string text;
    for (char c : clue)
        if (c != '\'') text += char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    for (size_t i = 0; i < text.size();) {
        if (!word_char(text[i])) { ++i; continue; }
        size_t j = i;
        while (j < text.size() && word_char(text[j])) ++j;
        words->push_back(text.substr(i, j - i));
        i = j;
    }
}

// The ids in both sorted lists, the smaller galloping through the larger.
void intersect(NpyArray<uint32_t> small, NpyArray<uint32_t> big,
               std::vector<uint32_t>* out) {
    out->clear();
    if (small.size > big.size) std::swap(small, big);
    uint32_t const* from = big.begin();
    for (uint32_t id : small) {
        size_t step = 1;
        uint32_t const* high = from;
        while (high < big.end() && *high < id) {
            from = high;
            high = from + std::min<size_t>(step, size_t(big.end() - from));
            step *= 2;
        }
        from = std::lower_bound(from, high == big.end() ? high : high + 1, id);
        if (from == big.end()) return;
        if (*from == id) out->push_back(id);
    }
}

}  // namespace

bool ClueIndex::open(std::filesystem::path const& cluedata,
                     std::string* error) {
    std::filesystem::path const dir = cluedata.parent_path() / "index";
    std::string metadata;
    if (!read_text(dir / "metadata.json", &metadata, error)) return false;
    if (metadata_value(metadata, "version") != CLUE_INDEX_VERSION) {
        *error = dir.string() + " isn't a version " +
            std::to_string(CLUE_INDEX_VERSION) +
            " clue index; rebuild it with cluer/build_index.py";
        return false;
    }
    struct stat info;
    if (stat(cluedata.c_str(), &info) != 0) {
        *error = cluedata.string() + ": can't stat";
        return false;
    }
    long long const mtime_ns =
        (long long)info.st_mtim.tv_sec * 1000000000LL + info.st_mtim.tv_nsec;
    if (metadata_value(metadata, "source_size") != (long long)info.st_size ||
        metadata_value(metadata, "source_mtime_ns") != mtime_ns) {
        *error = dir.string() + " wasn't built from " + cluedata.string() +
            "; rebuild it with cluer/build_index.py";
        return false;
    }

    NpyArray<uint32_t> ref_start;
    NpyArray<uint32_t> refs;
    MappedFile ref_start_file;
    MappedFile refs_file;
    if (!data_.open(cluedata, error) ||
        !open_array(dir, "postings.npy", &postings_file_, &postings_, error) ||
        !open_array(dir, "postings_start.npy", &postings_start_file_,
                    &postings_start_, error) ||
        !open_array(dir, "clue_off.npy", &clue_off_file_, &clue_off_, error) ||
        !open_array(dir, "ref_start.npy", &ref_start_file, &ref_start, error) ||
        !open_array(dir, "refs.npy", &refs_file, &refs, error))
        return false;

    std::string tokens;
    std::string answers;
    if (!read_text(dir / "tokens.txt", &tokens, error) ||
        !read_text(dir / "answers.txt", &answers, error))
        return false;
    words_.clear();
    for (size_t i = 0, id = 0; i < tokens.size(); ++id) {
        size_t const end = std::min(tokens.find('\n', i), tokens.size());
        words_.emplace(tokens.substr(i, end - i), uint32_t(id));
        i = end + 1;
    }
    auto const disagree = [&] {
        *error = dir.string() + ": arrays don't agree; rebuild it";
        return false;
    };
    if (words_.size() + 1 != postings_start_.size ||
        ref_start.size != clue_off_.size + 1 ||
        postings_start_[0] != 0 || ref_start[0] != 0 ||
        postings_start_[words_.size()] != postings_.size ||
        ref_start[clue_off_.size] != refs.size)
        return disagree();
    for (size_t i = 0; i < words_.size(); ++i)
        if (postings_start_[i] > postings_start_[i + 1]) return disagree();
    for (size_t c = 0; c < clue_off_.size; ++c)
        if (ref_start[c] > ref_start[c + 1] ||
            clue_off_[c] >= data_.size() ||
            clue_off_[c] + 1 + uint8_t(data_.data()[clue_off_[c]]) >
                data_.size())
            return disagree();
    for (uint32_t clue : postings_)
        if (clue >= clue_off_.size) return disagree();

    std::vector<uint32_t> group;
    answers_.clear();
    for (size_t i = 0; i < answers.size();) {
        size_t const end = std::min(answers.find('\n', i), answers.size());
        auto const inserted = answers_.emplace(
            upper(std::string_view(answers).substr(i, end - i)),
            uint32_t(answers_.size()));
        group.push_back(inserted.first->second);
        i = end + 1;
    }
    answer_start_.assign(answers_.size() + 1, 0);
    for (uint32_t ref : refs) {
        if ((ref >> 1) >= group.size()) {
            *error = dir.string() + ": a reference past the answers";
            return false;
        }
        ++answer_start_[group[ref >> 1] + 1];
    }
    for (size_t i = 1; i < answer_start_.size(); ++i)
        answer_start_[i] += answer_start_[i - 1];
    std::vector<uint32_t> next(answer_start_.begin(), answer_start_.end() - 1);
    answer_clues_.assign(refs.size, 0);
    for (uint32_t c = 0; c < clue_off_.size; ++c)
        for (uint32_t r = ref_start[c]; r < ref_start[c + 1]; ++r)
            answer_clues_[next[group[refs[r] >> 1]]++] = c;
    std::vector<uint32_t> compact;
    std::vector<uint32_t> starts{0};
    for (size_t a = 0; a + 1 < answer_start_.size(); ++a) {
        for (uint32_t i = answer_start_[a]; i < answer_start_[a + 1]; ++i)
            if (compact.size() == starts.back() ||
                compact.back() != answer_clues_[i])
                compact.push_back(answer_clues_[i]);
        starts.push_back(uint32_t(compact.size()));
    }
    answer_clues_ = std::move(compact);
    answer_start_ = std::move(starts);
    return true;
}

NpyArray<uint32_t> ClueIndex::postings(std::string_view word) const {
    auto const found = words_.find(std::string(word));
    if (found == words_.end()) return {};
    uint32_t const start = postings_start_[found->second];
    return {postings_.data + start,
            postings_start_[found->second + 1] - start};
}

NpyArray<uint32_t> ClueIndex::answer_clues(std::string_view word) const {
    auto const found = answers_.find(upper(word));
    if (found == answers_.end()) return {};
    uint32_t const start = answer_start_[found->second];
    return {answer_clues_.data() + start,
            answer_start_[found->second + 1] - start};
}

std::string_view ClueIndex::clue(uint32_t id) const {
    uint32_t const offset = clue_off_[id];
    return std::string_view(data_.data() + offset + 1,
                            uint8_t(data_.data()[offset]));
}

CluePairCounts ClueIndex::pair(std::string_view a, std::string_view b) const {
    CluePairCounts out;
    NpyArray<uint32_t> const clues_a = postings(a);
    NpyArray<uint32_t> const clues_b = postings(b);
    out.clues_a = uint32_t(clues_a.size);
    out.clues_b = uint32_t(clues_b.size);

    std::vector<uint32_t> shared;
    std::vector<std::string> words;
    auto const answer_side = [&](std::string_view answer,
                                 NpyArray<uint32_t> other_clues,
                                 std::string_view other) {
        intersect(answer_clues(answer), other_clues, &shared);
        out.answer_links += uint32_t(shared.size());
        for (uint32_t id : shared) {
            clue_words(clue(id), &words);
            out.answer_exact += words.size() == 1 && words[0] == other;
            if (out.answer_min_words == 0 ||
                words.size() < out.answer_min_words)
                out.answer_min_words = uint32_t(words.size());
        }
    };
    answer_side(a, clues_b, b);
    answer_side(b, clues_a, a);

    std::string ab(a);
    ab += b;
    std::string ba(b);
    ba += a;
    out.phrase_answer = uint32_t(answer_clues(ab).size);
    if (ba != ab) out.phrase_answer += uint32_t(answer_clues(ba).size);

    if (clues_a.size == 0 || clues_b.size == 0) return out;
    intersect(clues_a, clues_b, &shared);
    out.together = uint32_t(shared.size());
    for (uint32_t id : shared) {
        clue_words(clue(id), &words);
        bool next_to = false;
        for (size_t i = 1; i < words.size(); ++i)
            next_to |= (words[i - 1] == a && words[i] == b) ||
                (words[i - 1] == b && words[i] == a);
        out.adjacent += next_to;
        out.adjacent_short += next_to && words.size() <= 3;
        out.exact += words.size() == 2 &&
            ((words[0] == a && words[1] == b) ||
             (words[0] == b && words[1] == a));
        if (out.min_words == 0 || words.size() < out.min_words)
            out.min_words = uint32_t(words.size());
    }
    return out;
}
