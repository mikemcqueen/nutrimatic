#include "pair-evidence.h"

#include "dfs-cli-help.h"
#include "log.h"
#include "parse-number.h"
#include "workflow-paths.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <optional>
#include <system_error>

bool PairEvidenceSource::open(
        std::optional<std::filesystem::path> const& p1,
        std::optional<std::filesystem::path> const& cluedata,
        std::string* error, PriorIndex* failed) {
    PriorIndex unused;
    if (failed == nullptr) failed = &unused;
    *failed = PriorIndex::none;
    if (p1) {
        p1_ = std::make_unique<P1Index>();
        if (!p1_->open(*p1, error)) {
            *failed = PriorIndex::p1;
            return false;
        }
    }
    if (cluedata) {
        clues_ = std::make_unique<ClueIndex>();
        if (!clues_->open(*cluedata, error)) {
            *failed = PriorIndex::clues;
            return false;
        }
    }
    return true;
}

PairEvidence PairEvidenceSource::lookup(std::string_view a,
                                        std::string_view b) const {
    PairEvidence out;
    if (p1_) out.p1 = p1_->yes(a, b, &out.yes);
    if (clues_) out.clues = clues_->pair(a, b);
    return out;
}

namespace {

struct PriorTerm {
    char const* name;
    // Whether it reads the phase 1 index, else the clue index.
    bool p1;
    char const* help;
    double (*value)(PairEvidence const&);
};

PriorTerm const PRIOR_TERMS[] = {
    {"max", true,
     "the larger phase 1 YES of the two word orders, 0 without one",
     [](PairEvidence const& e) { return std::max(e.yes.ab, e.yes.ba); }},
    {"mean", true, "the mean of the two orders' phase 1 YES, 0 without one",
     [](PairEvidence const& e) { return (e.yes.ab + e.yes.ba) / 2; }},
    {"min", true, "the smaller of the two orders' phase 1 YES, 0 without one",
     [](PairEvidence const& e) { return std::min(e.yes.ab, e.yes.ba); }},
    {"together", false, "1 when a clue has both words",
     [](PairEvidence const& e) { return double(e.clues.together > 0); }},
    {"adjacent", false,
     "1 when a clue has them adjacent, consecutive words",
     [](PairEvidence const& e) { return double(e.clues.adjacent > 0); }},
    {"adjacent-short", false,
     "1 when a clue of at most three words has them adjacent",
     [](PairEvidence const& e) {
         return double(e.clues.adjacent_short > 0);
     }},
    {"exact", false, "1 when a clue is the two words and nothing else",
     [](PairEvidence const& e) { return double(e.clues.exact > 0); }},
    {"link", false, "1 when one word is an answer whose clue has the other",
     [](PairEvidence const& e) { return double(e.clues.answer_links > 0); }},
    {"link-short", false,
     "1 when one word is an answer whose clue has the other and at most one "
     "more word",
     [](PairEvidence const& e) {
         return double(e.clues.answer_min_words > 0 &&
                       e.clues.answer_min_words <= 2);
     }},
    {"answer", false, "1 when the two words run together are an answer",
     [](PairEvidence const& e) { return double(e.clues.phrase_answer > 0); }},
    {"not-answer", false, "1 when they aren't",
     [](PairEvidence const& e) {
         return double(e.clues.phrase_answer == 0);
     }},
    {"rare-answer", false,
     "1 when they are an answer in no more clues than have them adjacent",
     [](PairEvidence const& e) {
         return double(e.clues.phrase_answer <= e.clues.adjacent);
     }},
};

constexpr size_t PRIOR_TERM_COUNT =
    sizeof(PRIOR_TERMS) / sizeof(PRIOR_TERMS[0]);

}  // namespace

bool parse_prior_weights(std::string_view spec, PriorWeights* out,
                         std::string* error) {
    out->weights.assign(PRIOR_TERM_COUNT, 0);
    std::vector<char> named(PRIOR_TERM_COUNT, 0);
    std::string_view rest(spec);
    while (!rest.empty()) {
        size_t const comma = std::min(rest.find(','), rest.size());
        std::string const item(rest.substr(0, comma));
        rest.remove_prefix(std::min(comma + 1, rest.size()));
        size_t const equals = item.find('=');
        std::string const name = item.substr(0, equals);
        size_t term = 0;
        while (term < PRIOR_TERM_COUNT && name != PRIOR_TERMS[term].name)
            ++term;
        if (equals == std::string::npos || term == PRIOR_TERM_COUNT) {
            *error = "\"" + item + "\" isn't NAME=WEIGHT with a known NAME";
            return false;
        }
        if (named[term]) {
            *error = name + " is named twice";
            return false;
        }
        double value;
        if (!parse_number(std::string_view(item).substr(equals + 1), &value) ||
            !std::isfinite(value)) {
            *error = name + "'s weight isn't a finite number";
            return false;
        }
        named[term] = 1;
        out->weights[term] = value;
    }
    return true;
}

double prior_score(PairEvidence const& evidence, PriorWeights const& weights) {
    double out = 0;
    for (size_t i = 0; i < PRIOR_TERM_COUNT; ++i)
        if (weights.weights[i] != 0)
            out += weights.weights[i] * PRIOR_TERMS[i].value(evidence);
    return out;
}

bool open_prior(std::optional<std::filesystem::path> const& p1,
                std::optional<std::filesystem::path> const& cluedata,
                std::optional<std::string> const& spec, bool all,
                PriorWeights* weights, PairEvidenceSource* source,
                std::string* error, PriorIndex* failed) {
    if (failed != nullptr) *failed = PriorIndex::none;
    if (!parse_prior_weights(spec.value_or(PRIOR_WEIGHTS_DEFAULT), weights,
                             error)) {
        *error = "--prior-weights: " + *error;
        return false;
    }
    bool use_p1 = all && p1;
    bool use_clues = all && cluedata;
    for (size_t i = 0; i < PRIOR_TERM_COUNT; ++i) {
        if (weights->weights[i] == 0) continue;
        bool const given = PRIOR_TERMS[i].p1 ? p1.has_value()
                                             : cluedata.has_value();
        if (!given && !spec) {
            weights->weights[i] = 0;
        } else if (!given) {
            *error = std::string("--prior-weights ") + PRIOR_TERMS[i].name +
                " needs " + (PRIOR_TERMS[i].p1 ? "--p1" : "--clues");
            return false;
        } else {
            (PRIOR_TERMS[i].p1 ? use_p1 : use_clues) = true;
        }
    }
    return source->open(use_p1 ? p1 : std::nullopt,
                        use_clues ? cluedata : std::nullopt, error, failed);
}

void default_prior_paths(PriorPaths* paths) {
    std::error_code error;
    if (!paths->p1 && std::filesystem::exists(P1_INDEX_PATH, error)) {
        paths->p1 = P1_INDEX_PATH;
        paths->p1_default = true;
    }
    if (!paths->clues && std::filesystem::exists(CLUEDATA_PATH, error)) {
        paths->clues = CLUEDATA_PATH;
        paths->clues_default = true;
    }
}

bool open_default_prior(std::string_view program, PriorPaths* paths,
                        std::optional<std::string> const& spec, bool all,
                        PriorWeights* weights, PairEvidenceSource* source) {
    std::string error;
    std::string skipped;
    PriorIndex failed;
    while (!open_prior(paths->p1, paths->clues, spec, all, weights, source,
                       &error, &failed)) {
        std::optional<std::filesystem::path>* path;
        if (failed == PriorIndex::p1 && paths->p1_default) {
            path = &paths->p1;
            paths->p1_default = false;
        } else if (failed == PriorIndex::clues && paths->clues_default) {
            path = &paths->clues;
            paths->clues_default = false;
        } else if (skipped.empty()) {
            report(program, "%s", error.c_str());
            return false;
        } else {
            report(program, "%s, having skipped %s", error.c_str(),
                   skipped.c_str());
            return false;
        }
        warn(program, "skipping %s: %s", (*path)->c_str(), error.c_str());
        if (!skipped.empty()) skipped += " and ";
        skipped += (*path)->string();
        path->reset();
        *source = PairEvidenceSource();
    }
    return true;
}

std::string pair_key(std::string_view a, std::string_view b) {
    if (b < a) std::swap(a, b);
    std::string out(a);
    out += ',';
    out += b;
    return out;
}

void help_prior_terms() {
    for (PriorTerm const& term : PRIOR_TERMS)
        dfs_help_option(term.name, "%s", term.help);
}
