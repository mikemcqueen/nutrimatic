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

expect_near_failure() {
  local input=$1
  local target=$2
  local name=$3
  shift 3
  set +e
  "$query_index" "$synthetic_index" "$input" --near "$target" "$@" \
    > "$test_dir/$name.stdout" 2> "$test_dir/$name.stderr"
  local status=$?
  set -e
  [[ $status -eq 2 ]] ||
    fail "$name should exit 2, got $status"
  [[ ! -s "$test_dir/$name.stdout" ]] ||
    fail "$name printed output before rejecting the query"
}

[[ "$("$query_index" "$synthetic_index" f --near ij)" == \
   "1 f gh ij" ]] ||
  fail "near query did not find the one-anchor intervening phrase"
[[ "$("$query_index" "$synthetic_index" ij --near f)" == \
   "1 f gh ij" ]] ||
  fail "near query did not search from the second argument's anchor"
[[ -z $("$query_index" "$synthetic_index" gh --near ij) ]] ||
  fail "near query printed an adjacent-only phrase"
[[ -z $("$query_index" "$synthetic_index" ab --near cd) ]] ||
  fail "near query with two anchors printed an unexpected phrase"
[[ "$("$query_index" "$synthetic_index" f --near ij -n 1)" == \
   "1 f gh ij" ]] ||
  fail "--top was not accepted in near mode"

expect_near_failure nope missing near-missing-anchors
grep -q 'index has neither near anchor "nope" nor "missing"' \
  "$test_dir/near-missing-anchors.stderr" ||
  fail "missing-anchor error does not name both arguments"
expect_near_failure ab cd near-dfs-option -m 1
grep -q -- '--min-word-length cannot be used with --near' \
  "$test_dir/near-dfs-option.stderr" ||
  fail "near-mode DFS option error is unclear"
expect_near_failure ab cd near-score --score
grep -q -- '--score cannot be used with --near' \
  "$test_dir/near-score.stderr" ||
  fail "--score should be rejected with --near"
expect_near_failure 'f  gh' ij near-malformed-spacing
expect_near_failure f iJ near-malformed-character

# The synthetic corpus total is 1148. Each comma after the first divides by
# corpus_total * P; spaces inside an exact entry do not add a segment. An
# entry counts what phase 1 counts, which is its whole trailing-space subtree:
# "ab" is 80, its own 10 plus the 70 of "ab cd".
default_two_entry_score=$(score_value 'ab,cd')
assert_close "$default_two_entry_score" \
  "$(awk 'BEGIN { print 80 * 7 / (1148 * 1000000) }')" \
  "the default should preserve the production segment penalty"
assert_close "$(score_value 'ab,cd' -P 1000000)" \
  "$default_two_entry_score" \
  "explicit default segment penalty should match the omitted option"

for penalty in 1 100 1000000; do
  assert_close "$(score_value ab -P "$penalty")" 80 \
    "one exact entry should be invariant at P=$penalty"
done

assert_close "$(score_value 'ab,cd' -P 100)" \
  "$(awk 'BEGIN { print 80 * 7 / (1148 * 100) }')" \
  "two entries should pay one segment penalty"
assert_close "$(score_value 'ab,cd,ab' --segment-penalty 100)" \
  "$(awk 'BEGIN { print 80 * 7 * 80 / (1148 * 100)^2 }')" \
  "three entries should pay two segment penalties"
for penalty in 1 100 1000000; do
  assert_close "$(score_value 'ab cd' -P "$penalty")" 70 \
    "a multi-word entry should remain one segment at P=$penalty"
done
assert_close "$(score_value 'ab,ab')" \
  "$(awk 'BEGIN { print 80 * 80 / (1148 * 1000000) }')" \
  "repeated entries should contribute repeatedly"

[[ "$(score_value 'ab, cd')" == "$(score_value 'ab,cd')" ]] ||
  fail "spaces adjacent to commas should not affect scoring"

expect_score_failure missing missing-entry
grep -q 'index has no entry "missing"' "$test_dir/missing-entry.stderr" ||
  fail "missing-entry error does not name the failed item"
expect_score_failure 'ab,,cd' empty-entry
expect_score_failure a prefix-only
expect_score_failure 'ab  cd' malformed-spacing

assert_close "$(score_value 'ab cd' -P 1)" 70 \
  "a multi-word entry should score as its own count without a bonus"
assert_close "$(score_value 'ab cd,ab' -P 1)" \
  "$(awk 'BEGIN { print 70 * 80 / 1148 }')" \
  "word count should not affect any segment's score without a bonus"

assert_close "$(score_value 'ab cd' --word-bonus 1 -P 1)" 70000000 \
  "--word-bonus should retain its million-fold boost at P=1"
assert_close "$(score_value ab --word-bonus 1)" 80 \
  "--word-bonus should not apply to a single-word segment"
assert_close "$(score_value 'ab cd,ab' --word-bonus 1 -P 1)" \
  "$(awk 'BEGIN { print 70 * 80 / 1148 * 1e6 }')" \
  "a mixed sequence should bonus only its multi-word segment"

expect_score_failure ab penalty-zero -P 0
grep -q '^error: --segment-penalty must be at least 1$' \
  "$test_dir/penalty-zero.stderr" ||
  fail "zero segment-penalty diagnostic is unclear"
expect_score_failure ab penalty-fraction --segment-penalty 0.5
expect_score_failure ab penalty-malformed -P nope
expect_score_failure ab penalty-nonfinite -P inf
expect_score_failure ab pair-bonus-malformed --pair-bonus nope

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
"$query_index" "$synthetic_index" wxyz -m 2 -n 10 \
  --require-completable \
  > "$test_dir/completable-default.stdout" \
  2> "$test_dir/completable-default.stderr"
"$query_index" "$synthetic_index" wxyz -m 2 -n 10 \
  --require-completable -S 0 \
  > "$test_dir/completable-auto.stdout" \
  2> "$test_dir/completable-auto.stderr"
[[ $(wc -l < "$test_dir/completable-on.stdout") -eq 3 ]] ||
  fail "--require-completable should drop the one dead-end class"
cmp "$test_dir/completable-default.stdout" "$test_dir/completable-auto.stdout" ||
  fail "-S 0 changed completable output"
grep -q 'search threads 1 cache 0 segment penalty 1000000' \
  "$test_dir/completable-default.stderr" ||
  fail "query-index default search threads changed"
grep -Eq 'search threads [1-9][0-9]* cache 0 segment penalty 1000000' \
  "$test_dir/completable-auto.stderr" ||
  fail "query-index -S 0 did not resolve to a positive thread count"
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

# Filtering is score-independent: the phrase-only completion of "f" stays
# reachable regardless of how the surviving members are ranked for display.
"$query_index" "$synthetic_index" fghij -m 1 -n 10 \
  --words-only --require-completable \
  > "$test_dir/phrase-completion.stdout" \
  2> "$test_dir/phrase-completion.stderr"
grep -q ' f$' "$test_dir/phrase-completion.stdout" ||
  fail "phrase-only completion of f was filtered out"

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

# -x caps the words in one extracted entry, so it can only remove entries a
# capless run already found.
"$query_index" "$synthetic_index" abcdef -m 1 -n 0 \
  > "$test_dir/extract-uncapped.stdout" \
  2> "$test_dir/extract-uncapped.stderr"
"$query_index" "$synthetic_index" abcdef -m 1 -n 0 -x 2 \
  > "$test_dir/extract-x2.stdout" 2> "$test_dir/extract-x2.stderr"
awk '{ print }' "$test_dir/extract-uncapped.stdout" |
  awk 'gsub(/ /, " ") <= 2' > "$test_dir/extract-filtered.stdout"
cmp "$test_dir/extract-filtered.stdout" "$test_dir/extract-x2.stdout" ||
  fail "-x 2 does not match the uncapped run filtered to two words"

# An explicit zero pair bonus makes loading a pair list leave output unchanged.
printf '1,2345\n' > "$test_dir/short-first.pairs"
printf '2345,1\n' > "$test_dir/short-last.pairs"
"$query_index" "$synthetic_index" 12345 -m 4 -n 10 \
  > "$test_dir/short-none.stdout" 2> "$test_dir/short-none.stderr"
! grep -q ' 1 2345$' "$test_dir/short-none.stdout" ||
  fail "a short pair was extracted without --pairs"
"$query_index" "$synthetic_index" 12345 -m 4 -n 10 \
  --pairs "$test_dir/short-first.pairs" --pair-bonus 0 \
  > "$test_dir/short-first.stdout" 2> "$test_dir/short-first.stderr"
grep -q ' 1 2345$' "$test_dir/short-first.stdout" ||
  fail "query-index did not extract a listed short-first pair"
! grep -q ' 2345 1$' "$test_dir/short-first.stdout" ||
  fail "query-index matched a short-first pair in reverse"
"$query_index" "$synthetic_index" 12345 -m 4 -n 10 \
  --pairs "$test_dir/short-last.pairs" --pair-bonus 0 \
  > "$test_dir/short-last.stdout" 2> "$test_dir/short-last.stderr"
grep -q ' 2345 1$' "$test_dir/short-last.stdout" ||
  fail "query-index did not extract a listed long-first short-last pair"
"$query_index" "$synthetic_index" 12345 -m 4 -n 10 -x 1 \
  --pairs "$test_dir/short-first.pairs" \
  > "$test_dir/short-extract-one.stdout" \
  2> "$test_dir/short-extract-one.stderr"
! grep -q ' 1 2345$' "$test_dir/short-extract-one.stdout" ||
  fail "query-index -x 1 kept a short-pair exception"
"$query_index" "$synthetic_index" 1234567 -m 4 -n 10 \
  --pairs "$test_dir/short-first.pairs" \
  > "$test_dir/short-longer.stdout" 2> "$test_dir/short-longer.stderr"
! grep -q ' 1 2345 67$' "$test_dir/short-longer.stdout" ||
  fail "a longer phrase containing a short pair was extracted"
"$query_index" "$synthetic_index" 12345 -m 4 -n 10 --words-only \
  --pairs "$test_dir/short-first.pairs" \
  > "$test_dir/short-words-only.stdout" \
  2> "$test_dir/short-words-only.stderr"
! grep -q ' 1 2345$' "$test_dir/short-words-only.stdout" ||
  fail "--words-only displayed a short-pair phrase"

# --score remains symmetric and does not apply the extraction minimum.
assert_close "$(score_value '2345 1' --pairs "$test_dir/short-first.pairs")" \
  1000000 "--score did not retain symmetric short-pair matching"

printf 'ab,cd\ncd,ab\n' > "$test_dir/pairs.txt"
"$query_index" "$synthetic_index" abcdef -m 1 -n 0 \
  --pairs "$test_dir/pairs.txt" --pair-bonus 0 \
  > "$test_dir/pair-list.stdout" 2> "$test_dir/pair-list.stderr"
! grep -q 'pair list:' "$test_dir/pair-list.stderr" ||
  fail "pair-list loading unexpectedly wrote to stderr"
cmp "$test_dir/extract-uncapped.stdout" "$test_dir/pair-list.stdout" ||
  fail "a loaded pair list changed stdout at --pair-bonus 0"

assert_close "$(score_value 'ab cd' --pairs "$test_dir/pairs.txt" \
    -P 1)" 70000000 \
  "--score should apply the default pair bonus to a listed pair"
assert_close "$(score_value 'ab cd' --pairs "$test_dir/pairs.txt" \
    --word-bonus 1 --pair-bonus 1 -P 1)" 70000000000000 \
  "word and pair bonuses should be additive in log space"
assert_close "$(score_value 'gh ij' --pairs "$test_dir/pairs.txt" \
    --pair-bonus 1 -P 1)" 5 \
  "--pair-bonus should not apply to an unlisted phrase"

# A pair list may also name one exact entry, which receives the pair bonus
# without requiring a comma-separated partner.
printf 'ab\n' > "$test_dir/single-word-pairs.txt"
assert_close "$(score_value ab --pairs "$test_dir/single-word-pairs.txt" -P 1)" \
  80000000 \
  "--score should apply the pair bonus to a listed standalone word"

"$query_index" "$synthetic_index" abcdef -m 1 -n 1 \
  --pairs "$test_dir/pairs.txt" \
  > "$test_dir/pair-bonus.stdout" 2> "$test_dir/pair-bonus.stderr"
[[ $(awk 'NR == 1 { print $2 " " $3 }' "$test_dir/pair-bonus.stdout") \
   == "ab cd" ]] ||
  fail "query-index did not promote a listed pair above both other groups"
assert_close "$(awk 'NR == 1 { print $1 }' "$test_dir/pair-bonus.stdout")" \
  70000000 "query-index printed the wrong listed-pair score"

# Solo words are external one-use partners. Index phrases work in either
# order, asserted pairs work without index support, and an aggregate-only
# phrase prefix has the same presence semantics as phase 1.
assert_close "$(score_value ab --solo-words cd --word-bonus 1)" 80000000 \
  "candidate-leading solo edge did not earn the word bonus"
assert_close "$(score_value cd --solo-words ab --word-bonus 1)" 7000000 \
  "solo-leading edge did not earn the word bonus"
assert_close "$(score_value f --solo-words gh --word-bonus 1)" 11000000 \
  "aggregate-only phrase prefix did not create a solo edge"

printf 'ba,dc\nwx,ab\nxy,ab\n' > "$test_dir/solo-pairs.txt"
assert_close "$(score_value ba --solo-words dc --word-bonus 1 \
    --pairs "$test_dir/solo-pairs.txt" --pair-bonus 1)" 5000000000000 \
  "pairs-only solo edge did not earn both bonuses"

# Both entries can reach cd, but the external word has capacity one.
assert_close "$(score_value 'ab,ba' -P 1 --solo-words cd --word-bonus 1 \
    --pairs "$test_dir/solo-pairs.txt" --pair-bonus 0)" \
  "$(awk 'BEGIN { print 80 * 5 / 1148 * 1e6 }')" \
  "one solo word was spent twice in --score"

# wx prefers the high edge to ab but can reroute to yz; xy has only the high
# edge to ab. The optimum therefore needs the augmenting-path reroute and earns
# three log-space bonus units.
reroute_score=$(score_value 'wx,xy' -P 1 --solo-words ab,yz \
  --word-bonus 1 --pairs "$test_dir/solo-pairs.txt" --pair-bonus 1)
assert_close "$reroute_score" \
  "$(awk 'BEGIN { print 7 * 4 / 1148 * 1e18 }')" \
  "solo assignment did not reroute to its maximum-score matching"

"$query_index" "$synthetic_index" wxyz -m 2 -n 0 \
  --solo-words ab,yz --word-bonus 1 \
  --pairs "$test_dir/solo-pairs.txt" --pair-bonus 1 \
  > "$test_dir/solo-unlimited.stdout" 2> "$test_dir/solo-unlimited.stderr"
"$query_index" "$synthetic_index" wxyz -m 2 -n 2 \
  --solo-words ab,yz --word-bonus 1 \
  --pairs "$test_dir/solo-pairs.txt" --pair-bonus 1 \
  > "$test_dir/solo-top2.stdout" 2> "$test_dir/solo-top2.stderr"
head -n 2 "$test_dir/solo-unlimited.stdout" \
  > "$test_dir/solo-expected-top2.stdout"
cmp "$test_dir/solo-expected-top2.stdout" "$test_dir/solo-top2.stdout" ||
  fail "bounded solo-word output differs from the unlimited prefix"
grep -q ' wx (ab)$' "$test_dir/solo-unlimited.stdout" ||
  fail "ordinary output did not show wx's selected solo partner"

"$query_index" "$synthetic_index" wxyz -m 2 -n 0 \
  --solo-words ab,yz --word-bonus 1 \
  --pairs "$test_dir/solo-pairs.txt" --pair-bonus 1 \
  --hide-solo-words \
  > "$test_dir/solo-hidden.stdout" 2> "$test_dir/solo-hidden.stderr"
grep -q ' wx$' "$test_dir/solo-hidden.stdout" ||
  fail "--hide-solo-words dropped or annotated wx"
grep -q '(' "$test_dir/solo-hidden.stdout" &&
  fail "--hide-solo-words left a partner annotation"

"$query_index" "$synthetic_index" abcdef -m 1 -n 0 \
  --solo-words ab --word-bonus 0 --pair-bonus 0 \
  > "$test_dir/solo-inert.stdout" 2> "$test_dir/solo-inert.stderr"
cmp "$test_dir/extract-uncapped.stdout" "$test_dir/solo-inert.stdout" ||
  fail "score-inert --solo-words changed ordinary output"

expect_score_failure ab solo-empty --solo-words ab,
expect_score_failure ab solo-malformed --solo-words Ab
expect_score_failure ab solo-duplicate --solo-words ab --solo-words ab
expect_score_failure ab solo-missing-argument --solo-words
seventeen=a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q
expect_score_failure ab solo-too-many --solo-words "$seventeen"
expect_score_failure ab solo-negative-word \
  --solo-words cd --word-bonus -1
expect_score_failure ab solo-negative-pair \
  --solo-words cd --pair-bonus -1

# --csv keeps exactly the multi-word entries of an ordinary run, printed as
# their words with the count column dropped.
"$query_index" "$synthetic_index" abcdef -m 1 -n 0 --csv \
  > "$test_dir/csv.stdout" 2> "$test_dir/csv.stderr"
[[ -s "$test_dir/csv.stdout" ]] || fail "--csv printed nothing"
grep -Ev '^[a-z0-9]+(,[a-z0-9]+)+$' "$test_dir/csv.stdout" &&
  fail "--csv emitted a single-word entry or a non-CSV line"
awk 'NF > 2 { $1 = ""; sub(/^ /, ""); gsub(/ /, ","); print }' \
  "$test_dir/extract-uncapped.stdout" > "$test_dir/csv-expected.stdout"
cmp "$test_dir/csv-expected.stdout" "$test_dir/csv.stdout" ||
  fail "--csv does not match the ordinary run's multi-word entries"

set +e
"$query_index" "$synthetic_index" abcdef --csv -w \
  > "$test_dir/csv-words-only.stdout" 2> "$test_dir/csv-words-only.stderr"
status=$?
set -e
[[ $status -eq 2 ]] || fail "--csv -w should exit 2, got $status"
expect_score_failure ab csv-with-score --csv
grep -q -- '--csv cannot be used with --score' \
  "$test_dir/csv-with-score.stderr" ||
  fail "--csv should be rejected with --score"

expect_score_failure ab extract-with-score -x 2
grep -q -- '--max-extract-words cannot be used with --score' \
  "$test_dir/extract-with-score.stderr" ||
  fail "-x should be rejected with --score"

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
