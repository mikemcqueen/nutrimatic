#include "dfs-solo-words.h"

#include "dfs-score.h"
#include "index.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <vector>

static void check(bool ok, char const* message) {
  if (!ok) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
  }
}

static void check_close(long double actual, long double expected,
                        char const* message) {
  if (fabsl(actual - expected) > 1e-15L) {
    fprintf(stderr, "FAIL: %s: expected %.20Lg, got %.20Lg\n",
            message, expected, actual);
    exit(1);
  }
}

static DfsSoloMasks masks(uint16_t words, uint16_t pairs = 0) {
  DfsSoloMasks result;
  result.word_mask = words;
  result.pair_mask = pairs;
  return result;
}

static void profile_resolution_test() {
  FILE* fp = tmpfile();
  check(fp != NULL, "could not create profile index");
  {
    IndexWriter writer(fp);
    writer.next("alpha ", 0, 11);
    writer.next("alpha right ", 0, 7);
    writer.next("beta ", 0, 4);
    writer.next("left ", 0, 5);
    writer.next("left beta ", 0, 3);
    writer.next("plain ", 0, 2);
    writer.next(NULL, 0, 0);
  }
  fflush(fp);
  rewind(fp);

  {
    IndexReader reader(fp);
    std::unordered_set<std::string> pairs;
    pairs.insert("plain asserted");
    std::vector<std::string> words = { "left", "right", "asserted" };
    DfsScoreModel const model(1.0, reader.count(), 1.0, 1.0);
    DfsSoloWords solo(&reader, words, &pairs, &model);

    IndexReader::EntryPosition alpha;
    IndexReader::EntryPosition beta;
    IndexReader::EntryPosition plain;
    check(reader.aggregate_entry_position("alpha", &alpha),
          "alpha position is missing");
    check(reader.aggregate_entry_position("beta", &beta),
          "aggregate prefix beta is missing");
    check(reader.aggregate_entry_position("plain", &plain),
          "plain position is missing");
    DfsSoloMasks const alpha_masks = solo.resolve(
        "alpha", alpha.continuation, alpha.aggregate_count);
    DfsSoloMasks const beta_masks = solo.resolve(
        "beta", beta.continuation, beta.aggregate_count);
    DfsSoloMasks const plain_masks = solo.resolve(
        "plain", plain.continuation, plain.aggregate_count);
    check(alpha_masks.word_mask == (uint16_t(1) << 1),
          "candidate-leading aggregate edge was not resolved");
    check(beta_masks.word_mask == (uint16_t(1) << 0),
          "solo-leading aggregate edge was not resolved");
    check(plain_masks.word_mask == (uint16_t(1) << 2) &&
              plain_masks.pair_mask == (uint16_t(1) << 2),
          "pairs-only edge was not promoted to a word edge");

    check(solo.register_profile(
              "alpha", alpha.continuation, alpha.aggregate_count) ==
              DFS_MEMBER_SOLO_WORD_EDGE,
          "ordinary edge registered the wrong flags");
    check(solo.register_profile(
              "plain", plain.continuation, plain.aggregate_count) ==
              (DFS_MEMBER_SOLO_WORD_EDGE | DFS_MEMBER_SOLO_PAIR_EDGE),
          "pair edge registered the wrong flags");
    solo.freeze();
    check(solo.lookup("alpha").word_mask == alpha_masks.word_mask,
          "frozen profile lookup lost alpha");
    check(solo.lookup("missing").word_mask == 0,
          "missing frozen profile acquired an edge");
  }
  fclose(fp);
}

static void matching_test() {
  long double const word = 2.0L;
  long double const pair = 5.0L;

  std::vector<DfsSoloMasks> scarce = {
    masks(0x1), masks(0x1),
  };
  check_close(dfs_solo_exact_bonus(scarce, word, pair), word,
              "one solo word was used twice");

  // Greedy segment 0 -> solo 0 blocks segment 1. The augmenting path must
  // reroute segment 0 -> solo 1 and retain both edges.
  std::vector<DfsSoloMasks> reroute = {
    masks(0x3), masks(0x1),
  };
  DfsSoloMatching const rerouted =
      dfs_solo_exact_matching(reroute, word, pair);
  check_close(rerouted.bonus, 2.0L * word,
              "augmenting path did not reroute an earlier match");
  check(rerouted.solo_word_indexes[0] == 1 &&
            rerouted.solo_word_indexes[1] == 0,
        "augmenting path assignment was not preserved");

  // The maximum-cardinality matching uses two ordinary edges (4), while the
  // one-edge matching can keep the high edge (7) and must win.
  std::vector<DfsSoloMasks> fewer_is_better = {
    masks(0x3, 0x1), masks(0x1),
  };
  DfsSoloMatching const fewer =
      dfs_solo_exact_matching(fewer_is_better, word, pair);
  check_close(fewer.bonus, word + pair,
              "matching cardinality displaced a higher-score frontier");
  check(fewer.solo_word_indexes[0] == 0 &&
            fewer.solo_word_indexes[1] == DFS_NO_SOLO_WORD,
        "selected assignment did not preserve the best frontier");

  std::vector<DfsSoloMasks> disjoint = {
    masks(0x1, 0x1), masks(0x2),
  };
  check(dfs_solo_score_correction(disjoint, double(word), double(pair)) == 0.0,
        "disjoint-mask fast path returned a correction");
  check(dfs_solo_score_correction(scarce, 0.0, 0.0) == 0.0,
        "zero bonuses returned a correction");

  double const correction =
      dfs_solo_score_correction(scarce, double(word), double(pair));
  check(correction <= 0.0 && correction == -double(word),
        "scarcity correction is not exact and non-positive");

  // This exact correction lies nearer the next double toward zero. The
  // conversion must nevertheless round down to preserve the pending bound.
  double const tiny = ldexp(3.0, -55);
  std::vector<DfsSoloMasks> ulp = {
    masks(0x1, 0x1), masks(0x1),
  };
  double const ulp_correction = dfs_solo_score_correction(ulp, 1.0, tiny);
  long double const exact = -1.0L + static_cast<long double>(tiny);
  check(static_cast<long double>(ulp_correction) <= exact,
        "correction conversion rounded above the exact value");
  check(ulp_correction == -1.0,
        "correction was not directed to the lower adjacent double");

  // Two overlapping profiles can still take two high edges. If W + P rounded
  // down in each pending term, the long-double difference is a tiny positive
  // artifact and must clamp to zero rather than improve the upper score.
  std::vector<DfsSoloMasks> rounded_upper = {
    masks(0x3, 0x3), masks(0x3, 0x3),
  };
  check(dfs_solo_score_correction(rounded_upper, 1.0, tiny) == 0.0,
        "positive pending-term rounding artifact was not clamped");
}

int main() {
  profile_resolution_test();
  matching_test();
  return 0;
}
