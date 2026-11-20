#include "pair-answers-impl.h"

#include "dfs-cli-args.h"
#include "log.h"
#include "run-threads.h"

#include <algorithm>

namespace {

bool splits(std::vector<std::string> const& sorted, DfsDictionary const& words,
            std::string_view part, bool backwards) {
    for (auto it = std::lower_bound(sorted.begin(), sorted.end(), part);
         it != sorted.end() && it->starts_with(part); ++it) {
        if (it->size() == part.size()) continue;
        std::string rest = it->substr(part.size());
        if (backwards) std::reverse(rest.begin(), rest.end());
        if (words.contains(rest)) return true;
    }
    return false;
}

}  // namespace

bool load_answer_dictionary(std::string_view program,
                            std::filesystem::path const& path,
                            AnswerDictionary* out) {
    if (!load_dictionary(path, &out->words)) return false;
    if (out->words.empty()) {
        report(program, "%s: no words", path.c_str());
        return false;
    }
    out->forward.assign(out->words.begin(), out->words.end());
    std::sort(out->forward.begin(), out->forward.end());
    out->reversed.clear();
    out->reversed.reserve(out->forward.size());
    for (std::string const& word : out->forward)
        out->reversed.emplace_back(word.rbegin(), word.rend());
    std::sort(out->reversed.begin(), out->reversed.end());
    return true;
}

std::string answer_flags(AnswerDictionary const& dict,
                         std::string_view answer) {
    std::string word(answer);
    for (char& c : word)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    std::string const backwards(word.rbegin(), word.rend());
    std::string out;
    if (dict.words.contains(word)) out += 'M';
    if (splits(dict.forward, dict.words, word, false)) out += 'P';
    if (splits(dict.reversed, dict.words, backwards, true)) out += 'S';
    if (out.empty()) out = "X";
    return out;
}

bool PairAnswers::open(std::string_view program,
                       std::filesystem::path const& cluedata) {
    std::string error;
    if (index_.open(cluedata, &error)) return true;
    report(program, "%s", error.c_str());
    return false;
}

void PairAnswers::answers(std::string_view left, std::string_view right,
                          ClueMatch match,
                          std::vector<std::string_view>* out) const {
    index_.answers(left, right, match, out);
    std::sort(out->begin(), out->end());
}

std::vector<char> PairAnswers::have_answers(
    std::span<std::pair<std::string_view, std::string_view> const> pairs,
    ClueMatch match, size_t threads) const {
    std::vector<char> out(pairs.size(), 0);
    threads = std::clamp<size_t>(threads, 1,
                                 std::max<size_t>(1, pairs.size()));
    run_threads(threads, [&](size_t t) {
        std::vector<std::string_view> found;
        size_t const end = pairs.size() * (t + 1) / threads;
        for (size_t i = pairs.size() * t / threads; i < end; ++i) {
            index_.answers(pairs[i].first, pairs[i].second, match, &found);
            out[i] = !found.empty();
        }
    });
    return out;
}
