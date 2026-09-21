#!/usr/bin/env bash
set -euo pipefail

query_index=$1
make_index=$2

test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-query-index.XXXXXX")

cleanup() {
  rm -rf "$test_dir"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

synthetic_index="$test_dir/test.index"
"$make_index" "$synthetic_index"

set +e
"$query_index" "$synthetic_index" abcd \
  > "$test_dir/positional-index.stdout" \
  2> "$test_dir/positional-index.stderr"
positional_index_status=$?
"$query_index" abcd \
  > "$test_dir/missing-index.stdout" 2> "$test_dir/missing-index.stderr"
missing_index_status=$?
"$query_index" -i "$synthetic_index" \
  > "$test_dir/missing-letters.stdout" \
  2> "$test_dir/missing-letters.stderr"
missing_letters_status=$?
set -e
[[ $positional_index_status -eq 2 ]] ||
  fail "positional index should exit 2, got $positional_index_status"
grep -q '^usage: .* \[-i INDEX\] \[options\] letters$' \
  "$test_dir/positional-index.stdout" ||
  fail "positional index rejection did not show the new synopsis"
grep -q '^options:$' "$test_dir/positional-index.stdout" ||
  fail "query-index help did not show the options section"
grep -Eq '^  -i, --idx INDEX +read the completed Nutrimatic index' \
  "$test_dir/positional-index.stdout" ||
  fail "query-index help did not align option descriptions"
grep -Eq '^  --csv +omit the leading count or score from each result' \
  "$test_dir/positional-index.stdout" ||
  fail "query-index help did not describe --csv"
[[ $missing_index_status -eq 2 ]] ||
  fail "missing -i should exit 2, got $missing_index_status"
grep -q '^error: missing index; use -i INDEX or --wfroot DIR$' \
  "$test_dir/missing-index.stderr" ||
  fail "missing index error is unclear"
[[ $missing_letters_status -eq 2 ]] ||
  fail "missing letters should exit 2, got $missing_letters_status"
grep -q '^error: missing letters argument$' \
  "$test_dir/missing-letters.stderr" ||
  fail "missing letters error is unclear"

"$query_index" -i "$synthetic_index" abcd -u ab -m 2 -n 1 \
  --word-bonus 0 > /dev/null 2> "$test_dir/letter-bag.stderr"
grep -Eq '^\[[0-9][0-9]:[0-9][0-9]:[0-9][0-9]\] 2 letters "cd"$' \
  "$test_dir/letter-bag.stderr" ||
  fail "query-index did not report the final letter bag"

score_value() {
  "$query_index" -i "$synthetic_index" "$1" --score "${@:2}" \
      2> "$test_dir/score-value.stderr" |
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
  "$query_index" -i "$synthetic_index" "$sequence" --score "$@" \
    > "$test_dir/$name.stdout" 2> "$test_dir/$name.stderr"
  local status=$?
  set -e
  [[ $status -eq 2 ]] ||
    fail "$name should exit 2, got $status"
  [[ ! -s "$test_dir/$name.stdout" ]] ||
    grep -q "^usage: " "$test_dir/$name.stdout" ||
    fail "$name printed output before rejecting the sequence"
}

expect_near_failure() {
  local input=$1
  local target=$2
  local name=$3
  shift 3
  set +e
  "$query_index" -i "$synthetic_index" "$input" --near "$target" "$@" \
    > "$test_dir/$name.stdout" 2> "$test_dir/$name.stderr"
  local status=$?
  set -e
  [[ $status -eq 2 ]] ||
    fail "$name should exit 2, got $status"
  [[ ! -s "$test_dir/$name.stdout" ]] ||
    grep -q "^usage: " "$test_dir/$name.stdout" ||
    fail "$name printed output before rejecting the query"
}

[[ "$("$query_index" --idx "$synthetic_index" f --near ij)" == \
   "1 f gh ij" ]] ||
  fail "near query did not find the one-anchor intervening phrase"
[[ "$("$query_index" --idx "$synthetic_index" f --near ij --csv)" == \
   "f,gh,ij" ]] ||
  fail "near --csv did not print the phrase comma-separated without a count"
[[ "$("$query_index" -i "$synthetic_index" ij --near f)" == \
   "1 f gh ij" ]] ||
  fail "near query did not search from the second argument's anchor"
[[ -z $("$query_index" -i "$synthetic_index" gh --near ij) ]] ||
  fail "near query printed an adjacent-only phrase"
[[ -z $("$query_index" -i "$synthetic_index" ab --near cd) ]] ||
  fail "near query with two anchors printed an unexpected phrase"
[[ "$("$query_index" -i "$synthetic_index" f --near ij -n 1)" == \
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

# An entry counts what phase 1 counts, which is its whole trailing-space
# subtree: "ab" is 80, its own 10 plus the 70 of "ab cd".
for penalty in 1 100 1000000; do
  assert_close "$(score_value ab -P "$penalty")" 80 \
    "one exact entry should be invariant at P=$penalty"
  assert_close "$(score_value 'ab,cd' -P "$penalty" --word-bonus 0)" 70 \
    "a positional pair should remain one entry at P=$penalty"
  assert_close "$(score_value 'ab cd' -P "$penalty" --word-bonus 0)" 70 \
    "a multi-word entry should remain one segment at P=$penalty"
done

[[ "$(score_value 'ab,cd' -P 100)" == \
   "$(score_value 'cd,ab' -P 100)" ]] ||
  fail "a positional pair should use its indexed ordering"
[[ -z $(score_value 'missing,pair') ]] ||
  fail "a missing positional pair should not be printed"

[[ "$(score_value 'ab, cd')" == "$(score_value 'ab,cd')" ]] ||
  fail "spaces adjacent to commas should not affect scoring"
[[ "$("$query_index" -i "$synthetic_index" ab --score --csv)" == ab ]] ||
  fail "positional --score --csv did not omit the leading score"
expect_score_failure - sd-with-csv --sd --csv
grep -q -- '--sd cannot be used with --csv' \
  "$test_dir/sd-with-csv.stderr" ||
  fail "--sd should be rejected with --csv"

printf 'gh,ij\nij,gh\nab,cd\nf,gh,ij\nab\nqr,st\nmissing,pair\n' |
  "$query_index" -i "$synthetic_index" - --score --no-ptm \
    > "$test_dir/stdin-score.stdout" \
    2> "$test_dir/stdin-score.stderr"
[[ "$(awk '{ print $2 }' "$test_dir/stdin-score.stdout")" == \
   $'ab\nab,cd\ngh,ij\nij,gh\nqr,st\nf,gh,ij' ]] ||
  fail "stdin values were not sorted by descending score"
[[ "$(head -n 1 "$test_dir/stdin-score.stdout")" != Mean:* ]] ||
  fail "stdin scoring printed a score summary without --sd"
[[ $(awk '{ print NF }' "$test_dir/stdin-score.stdout" | sort -u) == 2 ]] ||
  fail "stdin scoring printed a deviation column without --sd"
assert_close "$(awk '$2 == "ab,cd" { print $1 }' \
    "$test_dir/stdin-score.stdout")" 70 \
  "stdin scoring did not query a comma-separated value as one phrase"
assert_close "$(awk '$2 == "ab" { print $1 }' \
    "$test_dir/stdin-score.stdout")" 80 \
  "stdin scoring did not accept a single-word value"
assert_close "$(awk '$2 == "f,gh,ij" { print $1 }' \
    "$test_dir/stdin-score.stdout")" 1 \
  "stdin scoring did not accept a three-word value"
assert_close "$(awk '$2 == "ij,gh" { print $1 }' \
    "$test_dir/stdin-score.stdout")" 5 \
  "stdin scoring did not fall back to the indexed pair orientation"
[[ -z $(awk '$2 == "missing,pair"' "$test_dir/stdin-score.stdout") ]] ||
  fail "stdin scoring printed a pair absent in both orientations"

printf 'gh,ij\nab\nmissing,pair\n' |
  "$query_index" -i "$synthetic_index" - --score --csv \
    > "$test_dir/stdin-csv.stdout" \
    2> "$test_dir/stdin-csv.stderr"
[[ "$(cat "$test_dir/stdin-csv.stdout")" == \
   $'ab\ngh,ij' ]] ||
  fail "stdin --score --csv printed a missing pair"

printf 'gh,ij\nij,gh\nab,cd\nf,gh,ij\nab\nqr,st\nmissing,pair\n' |
  "$query_index" -i "$synthetic_index" - --score --sd --no-ptm \
    > "$test_dir/stdin-score-sd.stdout" \
    2> "$test_dir/stdin-score-sd.stderr"
mean_line_pattern='^Mean: [0-9][0-9.e+-]*  1 sigma: x[0-9]+\.[0-9][0-9]$'
[[ "$(head -n 1 "$test_dir/stdin-score-sd.stdout")" =~ \
   $mean_line_pattern ]] ||
  fail "--sd did not print the mean score and deviation factor first"
mean_score=$(awk 'NR == 1 { print $2 }' "$test_dir/stdin-score-sd.stdout")
[[ "$(awk -v mean="$mean_score" \
     'NR > 1 && $2 != "-" { print ($1 > mean) == ($2 > 0) }' \
     "$test_dir/stdin-score-sd.stdout" | sort -u)" == 1 ]] ||
  fail "deviation signs disagree with each score's side of the mean"
[[ $(awk 'NR > 1 { print length($0) - length($3) }' \
     "$test_dir/stdin-score-sd.stdout" | sort -u | wc -l) -eq 1 ]] ||
  fail "stdin scores did not share one right-aligned column width"
[[ -z $(awk '$3 == "missing,pair"' \
     "$test_dir/stdin-score-sd.stdout") ]] ||
  fail "--sd printed a missing pair"
[[ "$(awk 'NR == 2 { print ($2 > 0) }' \
     "$test_dir/stdin-score-sd.stdout")" == 1 ]] ||
  fail "the highest-scoring value was not above the mean"

printf 'gh,ij\nij,gh\nab,cd\nf,gh,ij\nab\nqr,st\nmissing,pair\n' |
  "$query_index" -i "$synthetic_index" - --score --sd \
    > "$test_dir/stdin-score-ptm.stdout" \
    2> "$test_dir/stdin-score-ptm.stderr"
ptm_line_pattern='^Mean: [0-9][0-9.e+-]*  1 sigma: x[0-9]+\.[0-9][0-9]'
ptm_line_pattern+='  tail rate: [0-9]+\.[0-9][0-9][0-9]$'
[[ "$(head -n 1 "$test_dir/stdin-score-ptm.stdout")" =~ $ptm_line_pattern ]] ||
  fail "ptm did not add the fitted tail rate to the summary line"
[[ "$(awk 'NR > 1 { print $5 }' "$test_dir/stdin-score-ptm.stdout")" == \
   $'ab\nab,cd\ngh,ij\nij,gh\nqr,st\nf,gh,ij' ]] ||
  fail "ptm did not add two columns ahead of the value"
[[ "$(awk 'NR > 1 && $3 != "-" {
       if (seen && $3 > previous) unsorted = 1
       previous = $3; seen = 1
     } END { print unsorted + 0 }' \
     "$test_dir/stdin-score-ptm.stdout")" == 0 ]] ||
  fail "mapped deviations did not follow the score order"
[[ -z $(awk '$5 == "missing,pair"' \
     "$test_dir/stdin-score-ptm.stdout") ]] ||
  fail "ptm printed a missing pair"
[[ "$(awk 'NR > 1 && $4 != "-" {
       if (seen && $4 > previous) unsorted = 1
       previous = $4; seen = 1
     } END { print unsorted + 0 }' \
     "$test_dir/stdin-score-ptm.stdout")" == 0 ]] ||
  fail "mapped scores did not follow the score order"

printf 'gh,ij\nij,gh\nab,cd\nf,gh,ij\nab\nqr,st\nmissing,pair\n' |
  "$query_index" -i "$synthetic_index" - --score \
    > "$test_dir/stdin-score-ptm-no-sd.stdout" \
    2> "$test_dir/stdin-score-ptm-no-sd.stderr"
[[ "$(head -n 1 "$test_dir/stdin-score-ptm-no-sd.stdout")" != Mean:* ]] ||
  fail "ptm printed a score summary without --sd"
[[ $(awk '{ print NF }' "$test_dir/stdin-score-ptm-no-sd.stdout" |
     sort -u) == 4 ]] ||
  fail "ptm did not hide only the deviation column without --sd"

set +e
"$query_index" -i "$synthetic_index" abcd --sd \
  > "$test_dir/sd-listing.stdout" 2> "$test_dir/sd-listing.stderr"
sd_listing_status=$?
set -e
[[ $sd_listing_status -eq 2 ]] ||
  fail "--sd without --score should exit 2, got $sd_listing_status"
grep -q -- '^error: --sd cannot be used without --score$' \
  "$test_dir/sd-listing.stderr" ||
  fail "--sd without --score diagnostic is unclear"

printf 'cd,ab\n' > "$test_dir/stdin-score-pair.txt"
printf 'cd,ab\n' |
  "$query_index" -i "$synthetic_index" - --score \
    --pairs "$test_dir/stdin-score-pair.txt" --pair-bonus 0 \
    > "$test_dir/stdin-score-best-order.stdout" \
    2> "$test_dir/stdin-score-best-order.stderr"
assert_close "$(awk '{ print $1 }' \
    "$test_dir/stdin-score-best-order.stdout")" 70 \
  "stdin scoring did not choose the higher-scoring pair orientation"
[[ "$(awk '{ print $2 }' \
    "$test_dir/stdin-score-best-order.stdout")" == 'cd,ab' ]] ||
  fail "stdin scoring did not preserve the written pair order"

printf 'ab,cd\ngh,ij\n' |
  "$query_index" -i "$synthetic_index" - --score \
    --pairs "$test_dir/stdin-score-pair.txt" \
    > "$test_dir/stdin-score-bonus.stdout" \
    2> "$test_dir/stdin-score-bonus.stderr"
if grep -q '^Mean:' "$test_dir/stdin-score-bonus.stdout"; then
  fail "an applied bonus should withhold the score summary"
fi
[[ "$(awk '{ print $2 }' "$test_dir/stdin-score-bonus.stdout")" == \
   $'ab,cd\ngh,ij' ]] ||
  fail "a withheld summary should leave the value in the second column"

set +e
printf 'ab,,cd\n' |
  "$query_index" -i "$synthetic_index" - --score \
    > "$test_dir/stdin-score-malformed.stdout" \
    2> "$test_dir/stdin-score-malformed.stderr"
stdin_score_status=$?
set -e
[[ $stdin_score_status -eq 2 ]] ||
  fail "malformed stdin value should exit 2, got $stdin_score_status"
grep -q '^error: stdin line 1: expected comma-separated words$' \
  "$test_dir/stdin-score-malformed.stderr" ||
  fail "malformed stdin value diagnostic is unclear"

expect_score_failure missing missing-entry
grep -q 'index has no entry "missing"' "$test_dir/missing-entry.stderr" ||
  fail "missing-entry error does not name the failed item"
expect_score_failure 'ab,cd,ef' multiple-commas
grep -q '^error: positional --score query may contain at most one comma$' \
  "$test_dir/multiple-commas.stderr" ||
  fail "multiple positional commas diagnostic is unclear"
expect_score_failure 'ab,' trailing-empty-entry
grep -q '^error: positional --score pair must be two lowercase a-z0-9 words$' \
  "$test_dir/trailing-empty-entry.stderr" ||
  fail "an empty positional entry diagnostic is unclear"
expect_score_failure a prefix-only
expect_score_failure 'ab  cd' malformed-spacing

assert_close "$(score_value 'ab cd' --word-bonus 0 -P 1)" 70 \
  "a multi-word entry should score as its own count with an explicit zero bonus"

assert_close "$(score_value 'ab cd' -P 1)" 70 \
  "the default word bonus should be zero"
[[ "$(score_value 'ab cd' -P 1)" == \
   "$(score_value 'ab cd' --word-bonus 0 -P 1)" ]] ||
  fail "omitted --word-bonus did not match --word-bonus 0"
assert_close "$(score_value ab --word-bonus 1)" 80 \
  "--word-bonus should not apply to a single-word segment"

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
"$query_index" -i "$synthetic_index" wxyz -m 2 -n 10 \
  > "$test_dir/completable-off.stdout" 2> "$test_dir/completable-off.stderr"
[[ $(wc -l < "$test_dir/completable-off.stdout") -eq 4 ]] ||
  fail "expected all four wxyz-bag entries without --require-completable"
"$query_index" -i "$synthetic_index" wxyz -m 2 -n 10 -P 1 \
  > "$test_dir/penalty-one-ranking.stdout" \
  2> "$test_dir/penalty-one-ranking.stderr"
cmp "$test_dir/completable-off.stdout" \
    "$test_dir/penalty-one-ranking.stdout" ||
  fail "segment penalty changed ordinary one-entry ranking"

"$query_index" -i "$synthetic_index" wxyz -m 2 -n 10 \
  --require-completable -S 2 \
  > "$test_dir/completable-on.stdout" 2> "$test_dir/completable-on.stderr"
"$query_index" -i "$synthetic_index" wxyz -m 2 -n 10 \
  --require-completable \
  > "$test_dir/completable-default.stdout" \
  2> "$test_dir/completable-default.stderr"
"$query_index" -i "$synthetic_index" wxyz -m 2 -n 10 \
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
"$query_index" -i "$synthetic_index" "$wide_exact_bag" -m 2 -n 10 \
  --require-completable \
  > "$test_dir/exact-key-63-bit.stdout" \
  2> "$test_dir/exact-key-63-bit.stderr" ||
  fail "a 63-bit exact state count should fit the flat memo"
# One more radix-3 symbol makes 3 * 2^62 states: the class signature
# still fits in uint64_t, but packing its key with a verdict does not.
wide_exact_bag+="55"
set +e
"$query_index" -i "$synthetic_index" "$wide_exact_bag" -m 2 -n 10 \
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

"$query_index" -i "$synthetic_index" wxyz -m 2 -n 10 \
  --require-completable -S 2 -P 1 \
  > "$test_dir/completable-penalty-one.stdout" \
  2> "$test_dir/completable-penalty-one.stderr"
cmp "$test_dir/completable-on.stdout" \
    "$test_dir/completable-penalty-one.stdout" ||
  fail "segment penalty changed exact completability filtering"

# Filtering is score-independent: the phrase-only completion of "f" stays
# reachable regardless of how the surviving members are ranked for display.
"$query_index" -i "$synthetic_index" fghij -m 1 -n 10 \
  -w 1 --require-completable \
  > "$test_dir/phrase-completion.stdout" \
  2> "$test_dir/phrase-completion.stderr"
grep -q ' f$' "$test_dir/phrase-completion.stdout" ||
  fail "phrase-only completion of f was filtered out"

# Phrases remain available as completion classes under -w 1, but are
# filtered from the displayed members.
"$query_index" -i "$synthetic_index" qrstuv -m 2 -n 10 \
  -w 1 --require-completable \
  > "$test_dir/words-completed-by-phrase.stdout" \
  2> "$test_dir/words-completed-by-phrase.stderr"
[[ $(wc -l < "$test_dir/words-completed-by-phrase.stdout") -eq 1 ]] ||
  fail "-w 1 should print only the word completed by a phrase"
grep -q ' uv$' "$test_dir/words-completed-by-phrase.stdout" ||
  fail "a phrase was not retained as a completion path under -w 1"

# -x caps the words in one extracted entry, so it can only remove entries a
# capless run already found.
"$query_index" -i "$synthetic_index" abcdef -m 1 -n 0 \
  --word-bonus 0 \
  > "$test_dir/extract-uncapped.stdout" \
  2> "$test_dir/extract-uncapped.stderr"
"$query_index" -i "$synthetic_index" abcdef -m 1 -n 0 \
  --word-bonus 0 --csv \
  > "$test_dir/extract-csv.stdout" \
  2> "$test_dir/extract-csv.stderr"
awk '{ $1 = ""; sub(/^ /, ""); gsub(/ /, ","); print }' \
  "$test_dir/extract-uncapped.stdout" > "$test_dir/extract-text.stdout"
cmp "$test_dir/extract-text.stdout" "$test_dir/extract-csv.stdout" ||
  fail "--csv changed ordinary results beyond dropping the count and commas"
"$query_index" -i "$synthetic_index" abcdef -m 1 -n 0 -x 2 \
  --word-bonus 0 \
  > "$test_dir/extract-x2.stdout" 2> "$test_dir/extract-x2.stderr"
awk '{ print }' "$test_dir/extract-uncapped.stdout" |
  awk 'gsub(/ /, " ") <= 2' > "$test_dir/extract-filtered.stdout"
cmp "$test_dir/extract-filtered.stdout" "$test_dir/extract-x2.stdout" ||
  fail "-x 2 does not match the uncapped run filtered to two words"

[[ "$("$query_index" -i "$synthetic_index" klmn -m 1 -n 1 \
    --num-words 2)" == "5 kl mn" ]] ||
  fail "--num-words did not select the highest two-word entry before -n"
[[ "$("$query_index" -i "$synthetic_index" klmn -m 1 -n 1 \
    --max-words 2 --min-words 2)" == "5 kl mn" ]] ||
  fail "--min-words did not select a two-word entry with a matching cap"
[[ "$("$query_index" -i "$synthetic_index" klmn -m 1 -n 1 \
    -x 0 --min-words 2)" == "5 kl mn" ]] ||
  fail "--min-words rejected an unlimited -x"
set +e
"$query_index" -i "$synthetic_index" klmn -w 2 -x 1 \
  > "$test_dir/num-words-x.stdout" 2> "$test_dir/num-words-x.stderr"
num_words_x_status=$?
"$query_index" -i "$synthetic_index" klmn -w 2 -x 0 \
  > "$test_dir/num-words-x0.stdout" 2> "$test_dir/num-words-x0.stderr"
num_words_x0_status=$?
"$query_index" -i "$synthetic_index" klmn -w 2 --min-words 1 \
  > "$test_dir/num-words-min.stdout" 2> "$test_dir/num-words-min.stderr"
num_words_min_status=$?
"$query_index" -i "$synthetic_index" klmn --min-words 3 -x 2 \
  > "$test_dir/min-words-x.stdout" 2> "$test_dir/min-words-x.stderr"
min_words_x_status=$?
"$query_index" -i "$synthetic_index" klmn -w 0 \
  > "$test_dir/num-words-zero.stdout" 2> "$test_dir/num-words-zero.stderr"
num_words_zero_status=$?
set -e
[[ $num_words_x_status -eq 2 ]] || fail "-w 2 -x 1 should exit 2"
[[ $num_words_x0_status -eq 2 ]] || fail "-w 2 -x 0 should exit 2"
[[ $num_words_min_status -eq 2 ]] ||
  fail "-w 2 --min-words 1 should exit 2"
grep -q '^error: --num-words cannot be combined with --min-words or --max-words$' \
  "$test_dir/num-words-x.stderr" || fail "missing -w/-x diagnostic"
[[ $min_words_x_status -eq 2 ]] || fail "--min-words 3 -x 2 should exit 2"
grep -q '^error: --min-words 3 exceeds --max-words 2$' \
  "$test_dir/min-words-x.stderr" || fail "missing min/max diagnostic"
[[ $num_words_zero_status -eq 2 ]] || fail "-w 0 should exit 2"

# An explicit zero pair bonus makes loading a pair list leave output unchanged.
printf '1,2345\n' > "$test_dir/short-first.pairs"
printf '2345,1\n' > "$test_dir/short-last.pairs"
"$query_index" -i "$synthetic_index" 12345 -m 4 -n 10 \
  > "$test_dir/short-none.stdout" 2> "$test_dir/short-none.stderr"
! grep -q ' 1 2345$' "$test_dir/short-none.stdout" ||
  fail "a short pair was extracted without --pairs"
"$query_index" -i "$synthetic_index" 12345 -m 4 -n 10 \
  --pairs "$test_dir/short-first.pairs" --pair-bonus 0 \
  > "$test_dir/short-first.stdout" 2> "$test_dir/short-first.stderr"
grep -q ' 1 2345$' "$test_dir/short-first.stdout" ||
  fail "query-index did not extract a listed short-first pair"
! grep -q ' 2345 1$' "$test_dir/short-first.stdout" ||
  fail "query-index matched a short-first pair in reverse"
"$query_index" -i "$synthetic_index" 12345 -m 4 -n 10 \
  --pairs "$test_dir/short-last.pairs" --pair-bonus 0 \
  > "$test_dir/short-last.stdout" 2> "$test_dir/short-last.stderr"
grep -q ' 2345 1$' "$test_dir/short-last.stdout" ||
  fail "query-index did not extract a listed long-first short-last pair"
"$query_index" -i "$synthetic_index" 12345 -m 4 -n 10 -x 1 \
  --pairs "$test_dir/short-first.pairs" \
  > "$test_dir/short-extract-one.stdout" \
  2> "$test_dir/short-extract-one.stderr"
! grep -q ' 1 2345$' "$test_dir/short-extract-one.stdout" ||
  fail "query-index -x 1 kept a short-pair exception"
"$query_index" -i "$synthetic_index" 1234567 -m 4 -n 10 \
  --pairs "$test_dir/short-first.pairs" \
  > "$test_dir/short-longer.stdout" 2> "$test_dir/short-longer.stderr"
! grep -q ' 1 2345 67$' "$test_dir/short-longer.stdout" ||
  fail "a longer phrase containing a short pair was extracted"
"$query_index" -i "$synthetic_index" 12345 -m 4 -n 10 -w 1 \
  --pairs "$test_dir/short-first.pairs" \
  > "$test_dir/short-word-count.stdout" \
  2> "$test_dir/short-word-count.stderr"
! grep -q ' 1 2345$' "$test_dir/short-word-count.stdout" ||
  fail "-w 1 displayed a short-pair phrase"

# --score remains symmetric and does not apply the extraction minimum.
assert_close "$(score_value '2345 1' --word-bonus 0 \
    --pairs "$test_dir/short-first.pairs")" \
  1000000 "--score did not retain symmetric short-pair matching"

printf 'ab,cd\ncd,ab\n' > "$test_dir/pairs.txt"
"$query_index" -i "$synthetic_index" abcdef -m 1 -n 0 \
  --pairs "$test_dir/pairs.txt" --word-bonus 0 --pair-bonus 0 \
  > "$test_dir/pair-list.stdout" 2> "$test_dir/pair-list.stderr"
grep -q 'pair list: 2 pairs, 2 keys$' "$test_dir/pair-list.stderr" ||
  fail "the pair-list diagnostic did not report reversal and dedup"
grep -v ' cd ab$' "$test_dir/pair-list.stdout" \
  > "$test_dir/pair-list-indexed.stdout"
cmp "$test_dir/extract-uncapped.stdout" \
    "$test_dir/pair-list-indexed.stdout" ||
  fail "a loaded pair list changed an index-backed row at --pair-bonus 0"
grep -q '^1 cd ab$' "$test_dir/pair-list.stdout" ||
  fail "query-index did not synthesize a missing listed-pair orientation"
# Listing still synthesizes the missing orientation above; --score scores only
# what the index has.
expect_score_failure 'cd ab' unindexed-asserted \
  --pairs "$test_dir/pairs.txt" --pair-bonus 0 --word-bonus 0 -P 1
grep -q 'index has no entry "cd ab"' \
  "$test_dir/unindexed-asserted.stderr" ||
  fail "--score accepted an asserted entry absent from the index"

assert_close "$(score_value 'ab cd' --pairs "$test_dir/pairs.txt" \
    --word-bonus 0 -P 1)" 70000000 \
  "--score should apply the default pair bonus to a listed pair"
assert_close "$(score_value 'ab cd' --pairs "$test_dir/pairs.txt" \
    --word-bonus 1 --pair-bonus 1 -P 1)" 70000000000000 \
  "word and pair bonuses should be additive in log space"
assert_close "$(score_value 'gh ij' --pairs "$test_dir/pairs.txt" \
    --word-bonus 0 --pair-bonus 1 -P 1)" 5 \
  "--pair-bonus should not apply to an unlisted phrase"

# A pair list may also name one exact entry, which receives the pair bonus
# without requiring a comma-separated partner.
printf 'ab\n' > "$test_dir/single-word-pairs.txt"
assert_close "$(score_value ab --pairs "$test_dir/single-word-pairs.txt" -P 1)" \
  80000000 \
  "--score should apply the pair bonus to a listed standalone word"

# Every input loads once per invocation, not once per stdin line.
printf 'ab,cd\ncd,ab\nab,cd\n' |
  "$query_index" -i "$synthetic_index" - --score \
    --pairs "$test_dir/pairs.txt" \
    > "$test_dir/stdin-score-load-once.stdout" \
    2> "$test_dir/stdin-score-load-once.stderr"
pair_loads=$(grep -c 'pair list: 2 pairs' \
  "$test_dir/stdin-score-load-once.stderr" || true)
[[ "$pair_loads" == 1 ]] ||
  fail "stdin scoring read its pair file $pair_loads times, expected once"

# The fixed workflow tiers are repeatable, apply to either pair orientation,
# and use the maximum tier rather than stacking overlapping evidence.
printf 'ab,cd\n' > "$test_dir/seed-pairs.txt"
printf 'cd,ab\n' > "$test_dir/seed-pairs-reversed.txt"
printf 'cd,ab\n' > "$test_dir/yes-pairs.txt"
printf 'ab,cd\n' > "$test_dir/yes-pairs-reversed.txt"
printf 'ab,cd\n' > "$test_dir/best-pairs.txt"
printf 'cd,ab\n' > "$test_dir/best-pairs-reversed.txt"
assert_close "$(score_value 'ab cd' --word-bonus 0 -P 1 \
    --seed-pairs "$test_dir/seed-pairs.txt" \
    --seed-pairs "$test_dir/seed-pairs-reversed.txt")" \
  70000000 "repeated seed inputs did not retain the 1.00 tier"
assert_close "$(score_value 'ab cd' --word-bonus 0 -P 1 \
    --seed-pairs "$test_dir/seed-pairs.txt" \
    --yes-pairs "$test_dir/yes-pairs.txt" \
    --yes-pairs "$test_dir/yes-pairs-reversed.txt")" \
  "$(awk 'BEGIN { print 70 * exp(log(1000000) * 1.05) }')" \
  "YES pairs did not override the seed tier"
assert_close "$(score_value 'ab cd' --word-bonus 0 -P 1 \
    --seed-pairs "$test_dir/seed-pairs.txt" \
    --yes-pairs "$test_dir/yes-pairs.txt" \
    --yes-pairs "$test_dir/yes-pairs-reversed.txt" \
    --best-pairs "$test_dir/best-pairs.txt" \
    --more-best-pairs "$test_dir/best-pairs-reversed.txt")" \
  70000000 \
  "one-entry BEST score did not use the final exponent"

# Ordinary candidate listing has no exact result size, so it retains the
# fixed BEST exponent even though --score uses the final exponent.
"$query_index" -i "$synthetic_index" abcd -m 2 -n 1 -P 1 \
  --word-bonus 0 --best-pairs "$test_dir/best-pairs.txt" \
  > "$test_dir/fixed-best-listing.stdout" \
  2> "$test_dir/fixed-best-listing.stderr"
assert_close "$(awk 'NR == 1 { print $1 }' \
    "$test_dir/fixed-best-listing.stdout")" \
  "$(awk 'BEGIN { print 70 * exp(log(1000000) * 4) }')" \
  "ordinary listing did not retain the fixed BEST exponent"
expect_score_failure ab legacy-and-weighted --pairs "$test_dir/pairs.txt" \
  --seed-pairs "$test_dir/seed-pairs.txt"
grep -q '^error: --pairs cannot be combined with --seed-pairs, --yes-pairs, --best-pairs, or --more-best-pairs$' \
  "$test_dir/legacy-and-weighted.stderr" ||
  fail "legacy and fixed pair inputs were not rejected together"

# A small workflow root exercises the shared CLI resolver: default dictionary,
# classified YES, sentence seed discovery, complete-target BEST promotion, and
# the --wf environment alias.
workflow_root="$test_dir/workflow"
mkdir -p "$workflow_root/.wf/dict" \
  "$workflow_root/.wf/classified/yes" \
  "$workflow_root/.wf/best/idx" \
  "$workflow_root/.wf/best/s1/o-abcd/m2/g1"
cp "$synthetic_index" "$workflow_root/.wf/best/idx/wiki-merged.2.index"
printf 'ab\ncd\ngh\nij\n' > "$workflow_root/.wf/dict/words.filtered"
printf 'ab,cd\n' > "$workflow_root/.wf/classified/yes/yes.pairs"
printf 'gh,ij\n' > "$workflow_root/.wf/best/s1/seed.m2.pairs"
printf 'ab,cd\n' > "$workflow_root/.wf/best/s1/o-abcd/m2/g1/best.pairs"
assert_close "$(score_value 'gh ij' --word-bonus 0 -P 1 \
    --wfroot "$workflow_root" -t s1)" 5000000 \
  "sentence target did not auto-load its seed pairs"
assert_close "$(score_value 'ab cd' --word-bonus 0 -P 1 \
    --wfroot "$workflow_root" -t s1)" \
  "$(awk 'BEGIN { print 70 * exp(log(1000000) * 1.05) }')" \
  "sentence target did not load classified YES pairs"
full_target_score=$(score_value 'ab cd' --word-bonus 0 -P 1 \
  --wfroot "$workflow_root" -t 's1/o-abcd/m2/g1')
assert_close "$full_target_score" \
  70000000 \
  "complete target did not score its one BEST entry exactly"
printf 'gh,ij\n' > "$test_dir/replacement-best.pairs"
assert_close "$(score_value 'ab cd' --word-bonus 0 -P 1 \
    --wfroot "$workflow_root" -t 's1/o-abcd/m2/g1' \
    --best-pairs "$test_dir/replacement-best.pairs")" \
  "$(awk 'BEGIN { print 70 * exp(log(1000000) * 1.05) }')" \
  "--best-pairs did not replace the complete target's best.pairs"
assert_close "$(score_value 'ab cd' --word-bonus 0 -P 1 \
    --wfroot "$workflow_root" -t 's1/o-abcd/m2/g1' \
    --more-best-pairs "$test_dir/replacement-best.pairs")" \
  "$full_target_score" \
  "--more-best-pairs did not keep the complete target's best.pairs"
expect_score_failure ab repeated-best-pairs \
  --best-pairs "$test_dir/best-pairs.txt" \
  --best-pairs "$test_dir/best-pairs-reversed.txt"
grep -q '^error: --best-pairs may be specified only once$' \
  "$test_dir/repeated-best-pairs.stderr" ||
  fail "repeated --best-pairs was not rejected"
wf_alias_score=$(WFROOT="$workflow_root" score_value 'ab cd' \
  --word-bonus 0 -P 1 --wf -t 'S1/o-abcd/m2/g1')
assert_close "$wf_alias_score" "$full_target_score" \
  "--wf did not resolve WFROOT like --wfroot"
"$query_index" abcd -m 2 -n 10 --word-bonus 0 \
  --wfroot "$workflow_root" -t 's1/o-abcd/m2/g1' \
  > "$test_dir/workflow-default-dict.stdout" \
  2> "$test_dir/workflow-default-dict.stderr"
grep -q ' ab cd$' "$test_dir/workflow-default-dict.stdout" ||
  fail "workflow default dictionary dropped an allowed phrase"
! grep -q ' ab dc$' "$test_dir/workflow-default-dict.stdout" ||
  fail "workflow default dictionary failed to filter an unavailable word"
WFROOT="$workflow_root" "$query_index" abcd -m 2 -n 10 --word-bonus 0 \
  --wf -t 'S1/o-abcd/m2/g1' \
  > "$test_dir/workflow-index-alias.stdout" \
  2> "$test_dir/workflow-index-alias.stderr"
cmp "$test_dir/workflow-default-dict.stdout" \
    "$test_dir/workflow-index-alias.stdout" ||
  fail "--wf did not infer the workflow index like --wfroot"
rm "$workflow_root/.wf/best/idx/wiki-merged.2.index"
"$query_index" -i "$synthetic_index" abcd -m 2 -n 10 --word-bonus 0 \
  --wfroot "$workflow_root" -t 's1/o-abcd/m2/g1' \
  > "$test_dir/workflow-index-override.stdout" \
  2> "$test_dir/workflow-index-override.stderr"
cmp "$test_dir/workflow-default-dict.stdout" \
    "$test_dir/workflow-index-override.stdout" ||
  fail "explicit -i did not override a missing workflow index"
"$query_index" -i "$synthetic_index" 'ab cd' --score \
  --word-bonus 0 -P 1 --wfroot "$workflow_root" \
  > "$test_dir/workflow-no-target-score.stdout" \
  2> "$test_dir/workflow-no-target-score.stderr"
assert_close "$(awk '{ print $1 }' \
    "$test_dir/workflow-no-target-score.stdout")" \
  70 "workflow without a target loaded classified YES pairs"
grep -q 'WARNING: no target supplied; only dictionary and classified-no filtering are active$' \
  "$test_dir/workflow-no-target-score.stderr" ||
  fail "workflow score without a target did not warn"
assert_close "$(score_value 'gh ij' --word-bonus 0 -P 1 \
    --wfroot "$workflow_root" \
    --seed-pairs "$workflow_root/.wf/best/s1/seed.m2.pairs")" \
  5000000 "explicit --seed-pairs did not satisfy workflow mode"
assert_close "$(score_value 'ab cd' --word-bonus 0 -P 1 \
    --wfroot "$workflow_root" \
    --seed-pairs "$workflow_root/.wf/best/s1/seed.m2.pairs")" \
  70 "explicit seed without a target loaded classified YES pairs"

mkdir -p "$workflow_root/.wf/classified/no"
printf 'ab,cd\n' > "$workflow_root/.wf/classified/no/no.pairs"
assert_close "$(score_value 'ab cd' --word-bonus 0 -P 1 \
    --wfroot "$workflow_root" -t 's1/o-abcd/m2/g1')" \
  "$full_target_score" \
  "a workflow NO exclusion should not block --score"

# Score mode never inspects the NO path, so one it cannot even stat is fine;
# ordinary listing must still refuse it.
rm "$workflow_root/.wf/classified/no/no.pairs"
mkdir "$workflow_root/.wf/classified/no/no.pairs"
assert_close "$(score_value 'ab cd' --word-bonus 0 -P 1 \
    --wfroot "$workflow_root" -t 's1/o-abcd/m2/g1')" \
  "$full_target_score" \
  "--score inspected a workflow NO path it has no use for"
set +e
"$query_index" -i "$synthetic_index" abcd -m 2 -n 10 --word-bonus 0 \
  --wfroot "$workflow_root" -t 's1/o-abcd/m2/g1' \
  > "$test_dir/workflow-no-directory.stdout" \
  2> "$test_dir/workflow-no-directory.stderr"
workflow_no_directory_status=$?
set -e
[[ $workflow_no_directory_status -ne 0 ]] ||
  fail "ordinary listing accepted a workflow NO path that is a directory"
rmdir "$workflow_root/.wf/classified/no/no.pairs"
printf 'ab,cd\n' > "$workflow_root/.wf/classified/no/no.pairs"
"$query_index" -i "$synthetic_index" abcd -m 2 -n 10 --word-bonus 0 \
  --wfroot "$workflow_root" -t 's1/o-abcd/m2/g1' \
  > "$test_dir/workflow-no-listing.stdout" \
  2> "$test_dir/workflow-no-listing.stderr"
! grep -q ' ab cd$' "$test_dir/workflow-no-listing.stdout" ||
  fail "ordinary listing did not apply the workflow NO exclusion"
printf 'gh,ij\n' > "$workflow_root/.wf/classified/no/no.pairs"
WFROOT="$workflow_root" "$query_index" -i "$synthetic_index" abcdghij \
  -m 2 -n 10 --word-bonus 0 -P 1 --wf \
  > "$test_dir/workflow-no-target.stdout" \
  2> "$test_dir/workflow-no-target.stderr"
grep -q 'WARNING: no target supplied; only dictionary and classified-no filtering are active$' \
  "$test_dir/workflow-no-target.stderr" ||
  fail "workflow listing without a target did not warn"
! grep -q ' ab dc$' "$test_dir/workflow-no-target.stdout" ||
  fail "workflow without a target did not apply dictionary filtering"
! grep -q ' gh ij$' "$test_dir/workflow-no-target.stdout" ||
  fail "workflow without a target did not apply classified NO filtering"
assert_close "$(awk '$2 == "ab" && $3 == "cd" { print $1 }' \
    "$test_dir/workflow-no-target.stdout")" \
  70 "workflow listing without a target loaded classified YES pairs"

"$query_index" -i "$synthetic_index" abcdef -m 1 -n 1 \
  --pairs "$test_dir/pairs.txt" --word-bonus 0 \
  > "$test_dir/pair-bonus.stdout" 2> "$test_dir/pair-bonus.stderr"
[[ $(awk 'NR == 1 { print $2 " " $3 }' "$test_dir/pair-bonus.stdout") \
   == "ab cd" ]] ||
  fail "query-index did not promote a listed pair above both other groups"
assert_close "$(awk 'NR == 1 { print $1 }' "$test_dir/pair-bonus.stdout")" \
  70000000 "query-index printed the wrong listed-pair score"
[[ "$("$query_index" -i "$synthetic_index" abcdef -m 1 -n 1 \
    --pairs "$test_dir/pairs.txt" --word-bonus 0 --csv)" == \
   'ab,cd' ]] ||
  fail "--csv did not omit a computed listing score"

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

"$query_index" -i "$synthetic_index" wxyz -m 2 -n 0 \
  --solo-words ab,yz --word-bonus 1 \
  --pairs "$test_dir/solo-pairs.txt" --pair-bonus 1 \
  > "$test_dir/solo-unlimited.stdout" 2> "$test_dir/solo-unlimited.stderr"
"$query_index" -i "$synthetic_index" wxyz -m 2 -n 2 \
  --solo-words ab,yz --word-bonus 1 \
  --pairs "$test_dir/solo-pairs.txt" --pair-bonus 1 \
  > "$test_dir/solo-top2.stdout" 2> "$test_dir/solo-top2.stderr"
head -n 2 "$test_dir/solo-unlimited.stdout" \
  > "$test_dir/solo-expected-top2.stdout"
cmp "$test_dir/solo-expected-top2.stdout" "$test_dir/solo-top2.stdout" ||
  fail "bounded solo-word output differs from the unlimited prefix"
grep -q ' wx (ab)$' "$test_dir/solo-unlimited.stdout" ||
  fail "ordinary output did not show wx's selected solo partner"

"$query_index" -i "$synthetic_index" wxyz -m 2 -n 0 \
  --solo-words ab,yz --word-bonus 1 \
  --pairs "$test_dir/solo-pairs.txt" --pair-bonus 1 \
  --hide-solo-words \
  > "$test_dir/solo-hidden.stdout" 2> "$test_dir/solo-hidden.stderr"
grep -q ' wx$' "$test_dir/solo-hidden.stdout" ||
  fail "--hide-solo-words dropped or annotated wx"
grep -q '(' "$test_dir/solo-hidden.stdout" &&
  fail "--hide-solo-words left a partner annotation"

"$query_index" -i "$synthetic_index" abcdef -m 1 -n 0 \
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

# -w N caps the phase 1 walk at N words, like -x N, unless
# --require-completable needs the full-depth class list.
phase1() {
  grep -o 'phase 1 complete: .*' "$1"
}
for run in "x2:-x 2" "w2:-w 2" "uncapped:" "w2-complete:-w 2 --require-completable"; do
  "$query_index" -i "$synthetic_index" abcdefghijklmn -m 1 -n 0 --csv \
    ${run#*:} > "$test_dir/walk-${run%%:*}.stdout" \
    2> "$test_dir/walk-${run%%:*}.stderr"
done
[[ $(phase1 "$test_dir/walk-w2.stderr") == \
   $(phase1 "$test_dir/walk-x2.stderr") ]] ||
  fail "-w 2 did not cap the phase 1 walk like -x 2"
[[ $(phase1 "$test_dir/walk-uncapped.stderr") != \
   $(phase1 "$test_dir/walk-x2.stderr") ]] ||
  fail "the walk test letters do not reach a three-word entry"
[[ $(phase1 "$test_dir/walk-w2-complete.stderr") == \
   $(phase1 "$test_dir/walk-uncapped.stderr") ]] ||
  fail "-w 2 --require-completable capped the phase 1 walk"
grep , "$test_dir/walk-x2.stdout" > "$test_dir/walk-x2-pairs.stdout"
cmp "$test_dir/walk-x2-pairs.stdout" "$test_dir/walk-w2.stdout" ||
  fail "-w 2 does not match the two-word entries of -x 2"

expect_score_failure ab extract-with-score -x 2
grep -q -- '--max-words cannot be used with --score' \
  "$test_dir/extract-with-score.stderr" ||
  fail "-x should be rejected with --score"

printf 'qr\nst\nuv\n' > "$test_dir/dictionary-all"
"$query_index" -i "$synthetic_index" qrstuv -m 2 -n 10 \
  -w 1 --require-completable --dict "$test_dir/dictionary-all" \
  > "$test_dir/dictionary-all.stdout" \
  2> "$test_dir/dictionary-all.stderr"
grep -q ' uv$' "$test_dir/dictionary-all.stdout" ||
  fail "dictionary filtering dropped an allowed candidate or completion"

printf 'qr\nuv\n' > "$test_dir/dictionary-no-st"
"$query_index" -i "$synthetic_index" qrstuv -m 2 -n 10 \
  -w 1 --require-completable --dict "$test_dir/dictionary-no-st" \
  > "$test_dir/dictionary-no-st.stdout" \
  2> "$test_dir/dictionary-no-st.stderr"
[[ ! -s "$test_dir/dictionary-no-st.stdout" ]] ||
  fail "dictionary filtering did not remove a disallowed completion path"

if [[ -z ${IDX:-} ]]; then
  echo "SKIP: export IDX to run the rest of the query-index CLI test" >&2
  exit 77
fi

"$query_index" -i "$IDX" penbuilt -n 5 --word-bonus 0 \
  > "$test_dir/top5.stdout" 2> "$test_dir/top5.stderr"

[[ $(wc -l < "$test_dir/top5.stdout") -eq 5 ]] ||
  fail "expected 5 result lines"

if grep -Ev '^[0-9]+ [a-z]+( [a-z]+)*$' "$test_dir/top5.stdout"; then
  fail "unexpected output line format"
fi

awk '{ print $1 }' "$test_dir/top5.stdout" > "$test_dir/counts"
sort -rn -C "$test_dir/counts" ||
  fail "results are not sorted by descending count"

"$query_index" -i "$IDX" penbuilt -n 2 --word-bonus 0 \
  > "$test_dir/top2.stdout" 2> "$test_dir/top2.stderr"
head -n 2 "$test_dir/top5.stdout" > "$test_dir/expected-top2.stdout"
cmp "$test_dir/expected-top2.stdout" "$test_dir/top2.stdout" ||
  fail "--top did not retain the two highest-frequency entries"

"$query_index" -i "$IDX" penbuilt -n 5 -w 1 --word-bonus 0 \
  > "$test_dir/word-count-one.stdout" 2> "$test_dir/word-count-one.stderr"
if awk 'NF > 2 { exit 1 }' "$test_dir/word-count-one.stdout"; then :; else
  fail "-w 1 emitted a multi-word phrase"
fi

set +e
"$query_index" -i "$IDX" 'ab!' > /dev/null 2>&1
status=$?
set -e
[[ $status -eq 2 ]] || fail "bad letters should exit 2, got $status"

set +e
"$query_index" -i "$IDX" penbuilt -m nope > /dev/null 2>&1
status=$?
set -e
[[ $status -eq 2 ]] || fail "bad -m should exit 2, got $status"
