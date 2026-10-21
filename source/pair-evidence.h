#ifndef NUTRIMATIC_PAIR_EVIDENCE_H
#define NUTRIMATIC_PAIR_EVIDENCE_H

#include "clue-index.h"
#include "p1-index.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

// What is known about one pair of words.
struct PairEvidence {
    // Whether the phase 1 index has the pair, and its verdicts.
    bool p1 = false;
    P1Yes yes;
    CluePairCounts clues;
};

// The phase 1 index and clue index a pair is looked up in, either optional.
class PairEvidenceSource {
  public:
    // Opens a make-p1-index file, `p1`, and the clue index beside
    // `cluedata`; either may be NULL. False, with `*error` set, when one
    // can't be opened.
    bool open(char const* p1, char const* cluedata, std::string* error);

    PairEvidence lookup(std::string_view a, std::string_view b) const;

  private:
    std::unique_ptr<P1Index> p1_;
    std::unique_ptr<ClueIndex> clues_;
};

// The default --prior-weights SPEC: the mean of the two word orders' YES,
// plus one for each kind of clue evidence.
inline constexpr char PRIOR_WEIGHTS_DEFAULT[] =
    "mean=1,exact=1,adjacent=1,adjacent-short=1,link=1,link-short=1,"
    "rare-answer=1";

// A pair's prior: the sum of each term's weight times its value.
struct PriorWeights {
    std::vector<double> weights;
};

// Parses SPEC, comma-separated NAME=WEIGHT, into `out`; terms SPEC doesn't
// name weigh 0. False, with `*error` set, on an unknown or repeated name or
// a weight that isn't a finite number.
bool parse_prior_weights(std::string_view spec, PriorWeights* out,
                         std::string* error);

double prior_score(PairEvidence const& evidence, PriorWeights const& weights);

// Parses `spec`, NULL for PRIOR_WEIGHTS_DEFAULT less the terms of an index
// not given, into `weights`, and opens in `source` the indexes among `p1`
// and `cluedata` that its terms read (both with `all`). False, with
// `*error` set, on a bad SPEC, a weighted term whose index isn't given, or
// an index that can't be opened.
bool open_prior(char const* p1, char const* cluedata, char const* spec,
                bool all, PriorWeights* weights, PairEvidenceSource* source,
                std::string* error);

// The pair's key, the same for either order: "a,b" with a <= b.
std::string pair_key(std::string_view a, std::string_view b);

// Prints a help line for each term, as dfs_help_option() does.
void help_prior_terms();

#endif
