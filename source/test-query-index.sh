#!/usr/bin/env bash
set -euo pipefail

query_index=$1
make_index=$2

test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-query-index.XXXXXX")

cleanup() {
  rm -f "$test_dir"/*
  rmdir "$test_dir"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

synthetic_index="$test_dir/test.index"
"$make_index" "$synthetic_index"

score_value() {
  "$query_index" "$synthetic_index" "$1" --score "${@:2}" |
    awk '{ print $1 }'
}

assert_close() {
  local actual=$1
  local expected=$2
  local description=$3
  awk -v actual="$actual" -v expected="$expected" '
    BEGIN {
      difference = actual - expected
      if (difference < 0) difference = -difference
      scale = expected < 0 ? -expected : expected
      if (scale == 0) scale = 1
      exit difference <= scale * 0.0006 ? 0 : 1
    }
  ' || fail "$description: expected $expected, got $actual"
}

expect_score_failure() {
  local sequence=$1
  local name=$2
  shift 2
  set +e
  "$query_index" "$synthetic_index" "$sequence" --score "$@" \
    > "$test_dir/$name.stdout" 2> "$test_dir/$name.stderr"
  local status=$?
  set -e
  [[ $status -eq 2 ]] ||
    fail "$name should exit 2, got $status"
  [[ ! -s "$test_dir/$name.stdout" ]] ||
    fail "$name printed output before rejecting the sequence"
}

# The synthetic corpus total is 136. Each comma after the first divides by
# corpus_total * P; spaces inside an exact entry do not add a segment.
default_two_entry_score=$(score_value 'ab,cd')
assert_close "$default_two_entry_score" \
  "$(awk 'BEGIN { print 10 * 7 / (136 * 1000000) }')" \
  "the default should preserve the production segment penalty"
assert_close "$(score_value 'ab,cd' -P 1000000)" \
  "$default_two_entry_score" \
  "explicit default segment penalty should match the omitted option"

for penalty in 1 100 1000000; do
  assert_close "$(score_value ab -P "$penalty")" 10 \
    "one exact entry should be invariant at P=$penalty"
done

assert_close "$(score_value 'ab,cd' -P 100)" \
  "$(awk 'BEGIN { print 10 * 7 / (136 * 100) }')" \
  "two entries should pay one segment penalty"
assert_close "$(score_value 'ab,cd,ab' --segment-penalty 100)" \
  "$(awk 'BEGIN { print 10 * 7 * 10 / (136 * 100)^2 }')" \
  "three entries should pay two segment penalties"
for penalty in 1 100 1000000; do
  assert_close "$(score_value 'ab cd' -P "$penalty")" 70 \
    "a multi-word entry should remain one segment at P=$penalty"
done
assert_close "$(score_value 'ab,ab')" \
  "$(awk 'BEGIN { print 10 * 10 / (136 * 1000000) }')" \
  "repeated entries should contribute repeatedly"

[[ "$(score_value 'ab, cd')" == "$(score_value 'ab,cd')" ]] ||
  fail "spaces adjacent to commas should not affect scoring"

expect_score_failure missing missing-entry
grep -q 'index has no entry "missing"' "$test_dir/missing-entry.stderr" ||
  fail "missing-entry error does not name the failed item"
expect_score_failure 'ab,,cd' empty-entry
expect_score_failure a prefix-only
expect_score_failure 'ab  cd' malformed-spacing

assert_close "$(score_value 'ab cd' --word-bonus 1 -P 1)" 70000000 \
  "--word-bonus should retain its million-fold boost at P=1"
assert_close "$(score_value ab --word-bonus 1)" 10 \
  "--word-bonus should not apply to a single-word segment"
assert_close "$(score_value 'ab cd,ab' --word-bonus 1 -P 1)" \
  "$(awk 'BEGIN { print 70 * 10 / 136 * 1e6 }')" \
  "a mixed sequence should bonus only its multi-word segment"

expect_score_failure ab penalty-zero -P 0
grep -q '^error: --segment-penalty must be at least 1$' \
  "$test_dir/penalty-zero.stderr" ||
  fail "zero segment-penalty diagnostic is unclear"
expect_score_failure ab penalty-fraction --segment-penalty 0.5
expect_score_failure ab penalty-malformed -P nope
expect_score_failure ab penalty-nonfinite -P inf

expect_score_failure ab incompatible-option -n 1
grep -q -- '--top cannot be used with --score' \
  "$test_dir/incompatible-option.stderr" ||
  fail "score-mode option error is unclear"

# "wx" and "yz" exactly tile each other's remainder of the "wxyz" bag; "xy"
# leaves "wz" behind, which nothing in the synthetic index can complete.
"$query_index" "$synthetic_index" wxyz -m 2 -n 10 \
  > "$test_dir/completable-off.stdout" 2> "$test_dir/completable-off.stderr"
[[ $(wc -l < "$test_dir/completable-off.stdout") -eq 4 ]] ||
  fail "expected all four wxyz-bag entries without --require-completable"
"$query_index" "$synthetic_index" wxyz -m 2 -n 10 -P 1 \
  > "$test_dir/penalty-one-ranking.stdout" \
  2> "$test_dir/penalty-one-ranking.stderr"
cmp "$test_dir/completable-off.stdout" \
    "$test_dir/penalty-one-ranking.stdout" ||
  fail "segment penalty changed ordinary one-entry ranking"

"$query_index" "$synthetic_index" wxyz -m 2 -n 10 \
  --require-completable -S 2 \
  > "$test_dir/completable-on.stdout" 2> "$test_dir/completable-on.stderr"
[[ $(wc -l < "$test_dir/completable-on.stdout") -eq 3 ]] ||
  fail "--require-completable should drop the one dead-end class"
grep -q ' xy$' "$test_dir/completable-on.stdout" &&
  fail "--require-completable kept the dead-end 'xy' class"
grep -q ' wx$' "$test_dir/completable-on.stdout" ||
  fail "--require-completable dropped the completable 'wx' class"
grep -q ' yz$' "$test_dir/completable-on.stdout" ||
  fail "--require-completable dropped the completable 'yz' class"
grep -Eq 'phase 2 completability: 4 classes checked, .* exact validations$' \
  "$test_dir/completable-on.stderr" ||
  fail "batch completability diagnostics are missing"
grep -Eq 'phase 2 exact memo: [0-9]+ states computed, [0-9]+ hits$' \
  "$test_dir/completable-on.stderr" ||
  fail "exact memo diagnostics are missing"
grep -q 'phase 2 exact validation parallelism: 2 requested, 2 used' \
  "$test_dir/completable-on.stderr" ||
  fail "-S did not enable parallel exact validation"
grep -Eq 'segment penalty 1000000$' \
  "$test_dir/completable-on.stderr" ||
  fail "phase-2 diagnostic omitted the segment penalty"
grep -q 'phase 2 preflight: score-bound mode off$' \
  "$test_dir/completable-on.stderr" ||
  fail "query-index completability unexpectedly created a score cache"

# A 2^62-state bag needs 63 bits and still fits the flat memo encoding.
wide_exact_bag=
for symbol in {a..z} {0..4}; do
  wide_exact_bag+="${symbol}${symbol}${symbol}"
done
"$query_index" "$synthetic_index" "$wide_exact_bag" -m 2 -n 10 \
  --require-completable \
  > "$test_dir/exact-key-63-bit.stdout" \
  2> "$test_dir/exact-key-63-bit.stderr" ||
  fail "a 63-bit exact state count should fit the flat memo"
# One more radix-3 symbol makes 3 * 2^62 states: the class signature
# still fits in uint64_t, but packing its key with a verdict does not.
wide_exact_bag+="55"
set +e
"$query_index" "$synthetic_index" "$wide_exact_bag" -m 2 -n 10 \
  --require-completable \
  > "$test_dir/exact-key-overflow.stdout" \
  2> "$test_dir/exact-key-overflow.stderr"
status=$?
set -e
[[ $status -eq 2 ]] ||
  fail "exact-key overflow should exit 2, got $status"
grep -q 'error: phase 2 exact memo key arithmetic overflowed 64 bits$' \
  "$test_dir/exact-key-overflow.stderr" ||
  fail "exact-key overflow diagnostic is missing"

"$query_index" "$synthetic_index" wxyz -m 2 -n 10 \
  --require-completable -S 2 -P 1 \
  > "$test_dir/completable-penalty-one.stdout" \
  2> "$test_dir/completable-penalty-one.stderr"
cmp "$test_dir/completable-on.stdout" \
    "$test_dir/completable-penalty-one.stdout" ||
  fail "segment penalty changed exact completability filtering"

# Filtering is score-independent. In particular, an extreme negative bonus
# must not make the phrase-only completion of "f" look unreachable.
"$query_index" "$synthetic_index" fghij -m 1 -n 10 \
  --words-only --require-completable --word-bonus -100 \
  > "$test_dir/underflow-bonus.stdout" \
  2> "$test_dir/underflow-bonus.stderr"
grep -q ' f$' "$test_dir/underflow-bonus.stdout" ||
  fail "--word-bonus underflow changed exact completability"

"$query_index" "$synthetic_index" wxyz -m 2 -n 1 \
  --require-completable --word-bonus 1 \
  > "$test_dir/bonus.stdout" 2> "$test_dir/bonus.stderr"
grep -q ' wx yz$' "$test_dir/bonus.stdout" ||
  fail "--word-bonus did not rank a low-frequency phrase above the -n floor"

# Phrases remain available as completion classes under --words-only, but are
# filtered from the displayed members.
"$query_index" "$synthetic_index" qrstuv -m 2 -n 10 \
  --words-only --require-completable \
  > "$test_dir/words-completed-by-phrase.stdout" \
  2> "$test_dir/words-completed-by-phrase.stderr"
[[ $(wc -l < "$test_dir/words-completed-by-phrase.stdout") -eq 1 ]] ||
  fail "--words-only should print only the word completed by a phrase"
grep -q ' uv$' "$test_dir/words-completed-by-phrase.stdout" ||
  fail "a phrase was not retained as a completion path under --words-only"

printf 'qr\nst\nuv\n' > "$test_dir/dictionary-all"
"$query_index" "$synthetic_index" qrstuv -m 2 -n 10 \
  --words-only --require-completable --dict "$test_dir/dictionary-all" \
  > "$test_dir/dictionary-all.stdout" \
  2> "$test_dir/dictionary-all.stderr"
grep -q ' uv$' "$test_dir/dictionary-all.stdout" ||
  fail "dictionary filtering dropped an allowed candidate or completion"

printf 'qr\nuv\n' > "$test_dir/dictionary-no-st"
"$query_index" "$synthetic_index" qrstuv -m 2 -n 10 \
  --words-only --require-completable --dict "$test_dir/dictionary-no-st" \
  > "$test_dir/dictionary-no-st.stdout" \
  2> "$test_dir/dictionary-no-st.stderr"
[[ ! -s "$test_dir/dictionary-no-st.stdout" ]] ||
  fail "dictionary filtering did not remove a disallowed completion path"

if [[ -z ${IDX:-} ]]; then
  echo "SKIP: export IDX to run the rest of the query-index CLI test" >&2
  exit 77
fi

"$query_index" "$IDX" penbuilt -n 5 \
  > "$test_dir/top5.stdout" 2> "$test_dir/top5.stderr"

[[ $(wc -l < "$test_dir/top5.stdout") -eq 5 ]] ||
  fail "expected 5 result lines"

if grep -Ev '^[0-9]+ [a-z]+( [a-z]+)*$' "$test_dir/top5.stdout"; then
  fail "unexpected output line format"
fi

awk '{ print $1 }' "$test_dir/top5.stdout" > "$test_dir/counts"
sort -rn -C "$test_dir/counts" ||
  fail "results are not sorted by descending count"

"$query_index" "$IDX" penbuilt -n 2 \
  > "$test_dir/top2.stdout" 2> "$test_dir/top2.stderr"
head -n 2 "$test_dir/top5.stdout" > "$test_dir/expected-top2.stdout"
cmp "$test_dir/expected-top2.stdout" "$test_dir/top2.stdout" ||
  fail "--top did not retain the two highest-frequency entries"

"$query_index" "$IDX" penbuilt -n 5 -w \
  > "$test_dir/words-only.stdout" 2> "$test_dir/words-only.stderr"
if awk 'NF > 2 { exit 1 }' "$test_dir/words-only.stdout"; then :; else
  fail "--words-only emitted a multi-word phrase"
fi

set +e
"$query_index" "$IDX" 'ab!' > /dev/null 2>&1
status=$?
set -e
[[ $status -eq 2 ]] || fail "bad letters should exit 2, got $status"

set +e
"$query_index" "$IDX" penbuilt -m nope > /dev/null 2>&1
status=$?
set -e
[[ $status -eq 2 ]] || fail "bad -m should exit 2, got $status"
