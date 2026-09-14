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

static DfsSoloMasks weighted_masks(
    uint16_t words, DfsPairBonusKind first,
    DfsPairBonusKind second = DFS_PAIR_BONUS_NONE) {
  DfsSoloMasks result = masks(words, words);
  result.pair_kinds[0] = first;
  result.pair_kinds[1] = second;
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

static void descending_best_test() {
  long double const base = logl(DFS_PAIR_BONUS_BASE);
  DfsScoreModel const fixed(1.0, 1, 0.0);
  check_close(
      fixed.exact_best_log_bonus(2),
      2.0L * DFS_BEST_PAIR_BONUS * base,
      "fixed BEST policy changed its cumulative score");

  DfsScoreModel const descending(
      1.0, 1, 0.0, 0.0, DfsBestBonusPolicy::descending(4));
  long double const expected[] = { 0.0L, 4.0L, 7.0L, 9.0L, 10.0L };
  for (size_t best = 0; best <= 4; ++best)
    check_close(
        descending.exact_best_log_bonus(best), expected[best] * base,
        "descending BEST cumulative exponent is wrong");

  DfsScoreModel const one(
      1.0, 1, 0.0, 0.0, DfsBestBonusPolicy::descending(1));
  DfsExactResultMatching const one_direct = dfs_exact_result_matching(
      std::vector<DfsSoloMasks>(), std::vector<bool>(), 1, one);
  check(one_direct.correction <= 0.0,
        "N=1 BEST correction improved the local upper score");
  check_close(
      one_direct.correction,
      (1.0L - DFS_YES_PAIR_BONUS) * base,
      "N=1 BEST correction did not remove the 1.05 upper bound");

  std::vector<DfsSoloMasks> one_best_edge = {
    weighted_masks(0x1, DFS_PAIR_BONUS_BEST),
  };
  DfsExactResultMatching const selected_best = dfs_exact_result_matching(
      one_best_edge, std::vector<bool>(1, false), 0, descending);
  check(selected_best.best_segment_count == 1 &&
            selected_best.solo_word_indexes[0] == 0,
        "selected solo BEST edge did not mark its segment");

  DfsExactResultMatching const overlapping_best =
      dfs_exact_result_matching(
          one_best_edge, std::vector<bool>(1, true), 1, descending);
  check(overlapping_best.best_segment_count == 1 &&
            overlapping_best.solo_word_indexes[0] == 0,
        "direct and solo BEST sources counted one segment twice");

  std::vector<DfsSoloMasks> best_or_yes = {
    weighted_masks(0x3, DFS_PAIR_BONUS_BEST, DFS_PAIR_BONUS_YES),
  };
  DfsScoreModel const two(
      1.0, 1, 0.0, 0.0, DfsBestBonusPolicy::descending(2));
  DfsExactResultMatching const late_choice = dfs_exact_result_matching(
      best_or_yes, std::vector<bool>(1, false), 1, two);
  check(late_choice.best_segment_count == 1 &&
            late_choice.solo_word_indexes[0] == 1,
        "fixed YES did not defeat the final marginal BEST edge");

  DfsExactResultMatching const fixed_choice = dfs_exact_result_matching(
      best_or_yes, std::vector<bool>(1, false), 0, fixed);
  check(fixed_choice.solo_word_indexes[0] == 0,
        "fixed-policy matching stopped preferring BEST");
}

int main() {
  profile_resolution_test();
  matching_test();
  descending_best_test();
  return 0;
}
