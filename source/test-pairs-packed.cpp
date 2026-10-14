#include "pairs-impl.h"

#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <vector>

namespace {

char const* const WORDS[] = {
    "stop", "pots", "tops", "spot", "post", "opts", "late", "tale", "teal",
    "petal", "plate", "leap", "pale", "peal", "plea", "step", "pest", "sept",
    "lots", "slot", "apes", "spat", "past", "taps", "pats", "pastel",
    "staple", "plates", "toast", "stoat", "potato", "pot", "a",
};

void sub_bags(PairsCounts const& bag, int letter, PairsCounts* sub,
              std::vector<PairsCounts>* out) {
  if (letter == 26) {
    out->push_back(*sub);
    return;
  }
  for (int n = 0; n <= bag.values[letter]; ++n) {
    sub->values[letter] = uint8_t(n);
    sub_bags(bag, letter + 1, sub, out);
  }
  sub->values[letter] = 0;
}

template <typename Key>
int check(PairsExactCounter const& exact,
          std::vector<PairsCounts> const& subs) {
  PackedPairsCounter<Key> const packed(exact);
  PackedRemainderGroups<Key> all_hashed;
  all_hashed.build(packed.groups_by_length());
  typename PackedPairsCounter<Key>::Scratch scratch;
  PackedGroups<Key> fitting;
  PackedRemainderGroups<Key> hashed;
  int failures = 0;
  for (PairsCounts const& sub : subs) {
    uint64_t const want = exact.count(sub);
    packed.filter(packed.groups_by_length(), packed.pack(sub), &fitting);
    hashed.build(fitting);
    uint64_t const narrow = packed.count(sub, fitting, hashed, &scratch);
    uint64_t const wide =
        packed.count(sub, packed.groups_by_length(), all_hashed, &scratch);
    if (narrow != want || wide != want) {
      fprintf(stderr, "FAIL: %zu-bit count %llu/%llu, want %llu\n",
              8 * sizeof(Key), (unsigned long long) narrow,
              (unsigned long long) wide, (unsigned long long) want);
      ++failures;
    }
  }
  return failures;
}

}  // namespace

int main() {
  PairsCounts const bag = pairs_letter_counts("stopspotpotatoplatestaple");
  PairsExactCounter exact(bag, PAIRS_DEFAULT_MIN_WORD_LENGTH);
  for (char const* word : WORDS) exact.add(word);
  exact.finish();

  std::vector<PairsCounts> subs;
  PairsCounts sub{};
  sub_bags(bag, 0, &sub, &subs);
  uint64_t total = 0;
  for (PairsCounts const& s : subs) total += exact.count(s);
  if (total == 0) {
    fprintf(stderr, "FAIL: no pairs to compare\n");
    return 1;
  }

  int const failures = check<uint64_t>(exact, subs) +
                       check<PairsKey128>(exact, subs);
  if (failures != 0) return 1;
  printf("ok: %zu sub-bags, %llu pairs\n", subs.size(),
         (unsigned long long) total);
  return 0;
}
