#!/usr/bin/env bash
set -euo pipefail

dfs_anagrams=$1
make_index=$2
query_index=$3
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-dfs-cli.XXXXXX")
index_file=$test_dir/test.index
diagnostic_prefix='^\[[0-9][0-9]:[0-9][0-9]:[0-9][0-9]\] '

cleanup() {
  rm -f "$test_dir"/*
  rmdir "$test_dir"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

expect_status() {
  expected=$1
  shift
  set +e
  "$@" > "$test_dir/status.stdout" 2> "$test_dir/status.stderr"
  actual=$?
  set -e
  [[ $actual -eq $expected ]] ||
    fail "expected exit $expected, got $actual from: $*"
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

"$make_index" "$index_file"

"$dfs_anagrams" "$index_file" abcd -m 2 -n 10 \
  > "$test_dir/all.stdout" 2> "$test_dir/all.stderr"
"$dfs_anagrams" "$index_file" abcd -m 2 -n 10 -P 1000000 \
  > "$test_dir/explicit-default.stdout" \
  2> "$test_dir/explicit-default.stderr"
"$dfs_anagrams" "$index_file" abcd -m 2 -n 10 \
  --cache-size 0 \
  --allow-cache-fallback \
  > "$test_dir/uncached.stdout" 2> "$test_dir/uncached.stderr"
"$dfs_anagrams" "$index_file" abcd -m 2 -n 10 \
  --preprocess-threads 1 \
  > "$test_dir/thread-one.stdout" 2> "$test_dir/thread-one.stderr"
"$dfs_anagrams" "$index_file" abcd -m 2 -n 10 \
  --preprocess-threads 4 \
  > "$test_dir/threaded.stdout" 2> "$test_dir/threaded.stderr"
"$dfs_anagrams" "$index_file" abcd -m 2 -n 10 \
  -d 0 \
  > "$test_dir/depth-zero.stdout" 2> "$test_dir/depth-zero.stderr"
"$dfs_anagrams" "$index_file" abcd -m 2 -n 10 \
  -S 1 \
  > "$test_dir/search-thread-one.stdout" \
  2> "$test_dir/search-thread-one.stderr"
"$dfs_anagrams" "$index_file" abcd -m 2 -n 10 \
  -p 2147483648 \
  > "$test_dir/wide-progress-factor.stdout" \
  2> "$test_dir/wide-progress-factor.stderr"
"$dfs_anagrams" "$index_file" abcd -m 2 -n 0 \
  > "$test_dir/unlimited.stdout" 2> "$test_dir/unlimited.stderr"
cmp "$test_dir/all.stdout" "$test_dir/explicit-default.stdout" ||
  fail "explicit default segment penalty changed stdout"
cmp "$test_dir/all.stdout" "$test_dir/uncached.stdout" ||
  fail "score cache changed stdout"
cmp "$test_dir/all.stdout" "$test_dir/thread-one.stdout" ||
  fail "--preprocess-threads 1 changed stdout"
cmp "$test_dir/all.stdout" "$test_dir/threaded.stdout" ||
  fail "threaded preprocessing changed stdout"
cmp "$test_dir/all.stdout" "$test_dir/depth-zero.stdout" ||
  fail "--projection-depth changed stdout"
cmp "$test_dir/all.stdout" "$test_dir/search-thread-one.stdout" ||
  fail "-S 1 changed stdout"
cmp "$test_dir/all.stdout" "$test_dir/wide-progress-factor.stdout" ||
  fail "64-bit --progress-factor changed stdout"
cmp "$test_dir/all.stdout" "$test_dir/unlimited.stdout" ||
  fail "-n 0 did not return all results"
grep -Eq \
  "${diagnostic_prefix}phase 2: using up to [2-4] threads to calculate projected score bounds bottom-up$" \
  "$test_dir/threaded.stderr" ||
  fail "threaded preprocessing diagnostic is missing"
[[ $(wc -l < "$test_dir/all.stdout") -eq 4 ]] ||
  fail "full synthetic search did not print four word sets"
[[ $(grep -c '^70.00 ab cd$' "$test_dir/all.stdout") -eq 1 ]] ||
  fail "the contiguous phrase did not win and deduplicate its split form"
if grep -Eq "$diagnostic_prefix" "$test_dir/all.stdout"; then
  fail "progress leaked onto stdout"
fi
grep -Eq "${diagnostic_prefix}"'4 letters "abcd", words of 2\+, at most 2 words$' \
  "$test_dir/all.stderr" ||
  fail "search header is missing from stderr"
grep -Eq "${diagnostic_prefix}depth -1 top 10 threads 1 search threads 1 cache 64 segment penalty 1000000$" \
  "$test_dir/all.stderr" ||
  fail "resolved argument diagnostic is missing from stderr"
grep -Eq "${diagnostic_prefix}phase 1 complete:" "$test_dir/all.stderr" ||
  fail "phase-1 statistics are missing from stderr"
grep -Eq "${diagnostic_prefix}phase 2 preflight: score-bound mode projected dense \\(4-byte values, capacity [0-9]+, complete effective coverage\\)$" \
  "$test_dir/all.stderr" ||
  fail "default projected cache mode diagnostic is missing from stderr"
grep -Eq "${diagnostic_prefix}phase 2 preflight: projected score table keeps 4 rarest letters exact, merges 0 wildcard letters;" \
  "$test_dir/all.stderr" ||
  fail "automatic projected depth diagnostic is missing from stderr"
grep -Eq "${diagnostic_prefix}phase 2 preflight: projected score table keeps 0 rarest letters exact, merges 4 wildcard letters;" \
  "$test_dir/depth-zero.stderr" ||
  fail "--projection-depth diagnostic is missing from stderr"
grep -Eq "${diagnostic_prefix}phase 2 preflight: score-bound mode off$" \
  "$test_dir/unlimited.stderr" ||
  fail "-n 0 unexpectedly enabled score-bound pruning"
grep -Eq "${diagnostic_prefix}phase 2: precomputed [0-9]+ bounded states in [0-9.]+s$" \
  "$test_dir/all.stderr" ||
  fail "phase-2 precompute timing is missing from stderr"
grep -Eq "${diagnostic_prefix}phase 2 complete:" "$test_dir/all.stderr" ||
  fail "phase-2 statistics are missing from stderr"
grep -Eq "${diagnostic_prefix}phase 2 timing: [0-9.]+s setup, [0-9.]+s search, [0-9]+ successful bound transitions, [0-9]+ nextafter calls$" \
  "$test_dir/all.stderr" ||
  fail "phase-2 timing or transition statistics are missing from stderr"
if grep -Eq "${diagnostic_prefix}phase 2 length certificate: .* prepare," \
    "$test_dir/all.stderr"; then
  fail "length-certificate preparation timing was printed"
fi
grep -Eq "${diagnostic_prefix}phase 2 length certificate: active, [0-9]+ table bytes$" \
  "$test_dir/all.stderr" ||
  fail "length-certificate summary is missing from stderr"
grep -Eq "${diagnostic_prefix}phase 2   [0-9]+ group tests, [0-9]+ rejected, [0-9]+ class scans kept, [0-9]+ skipped$" \
  "$test_dir/all.stderr" ||
  fail "length-certificate counters are missing from stderr"
grep -Eq "${diagnostic_prefix}phase 2 score cache: [0-9]+ bound entries, [0-9]+ bound bytes$" \
  "$test_dir/all.stderr" ||
  fail "phase-2 cache statistics are missing from stderr"
grep -Eq "${diagnostic_prefix}phase 2 complete: .* [0-9]+ retained$" \
  "$test_dir/all.stderr" ||
  fail "phase-2 completion statistics have the wrong format"

"$dfs_anagrams" "$index_file" abcd -m 2 -n 2 \
  > "$test_dir/top.stdout" 2> "$test_dir/top.stderr"
head -n 2 "$test_dir/all.stdout" > "$test_dir/expected-top.stdout"
cmp "$test_dir/expected-top.stdout" "$test_dir/top.stdout" ||
  fail "--top did not retain the two highest-scoring word sets"
[[ $(wc -l < "$test_dir/unlimited.stdout") -gt \
   $(wc -l < "$test_dir/top.stdout") ]] ||
  fail "-n 0 did not return more results than -n 2"

"$dfs_anagrams" "$index_file" abcd -m 2 -n 10 -P 1 \
  > "$test_dir/penalty-one.stdout" \
  2> "$test_dir/penalty-one.stderr"
"$dfs_anagrams" "$index_file" abcd -m 2 -n 10 \
  --segment-penalty 1 --cache-size 0 --allow-cache-fallback \
  > "$test_dir/penalty-one-uncached.stdout" \
  2> "$test_dir/penalty-one-uncached.stderr"
cmp "$test_dir/penalty-one.stdout" \
    "$test_dir/penalty-one-uncached.stdout" ||
  fail "cache mode changed stdout at a non-default segment penalty"
default_split_score=$(awk '$2 == "ab,dc" { print $1 }' \
  "$test_dir/all.stdout")
penalty_one_split_score=$(awk '$2 == "ab,dc" { print $1 }' \
  "$test_dir/penalty-one.stdout")
[[ -n $default_split_score && -n $penalty_one_split_score ]] ||
  fail "known two-segment spelling was not retained"
assert_close "$penalty_one_split_score" \
  "$(awk -v score="$default_split_score" \
      'BEGIN { print score * 1000000 }')" \
  "P=1 should remove one million-fold penalty from two segments"
assert_close "$(awk '$2 == "ab" && $3 == "cd" { print $1 }' \
    "$test_dir/penalty-one.stdout")" 70 \
  "one-segment phrase should be invariant"

"$dfs_anagrams" "$index_file" abcd -m 2 -n 10 --pairs \
  > "$test_dir/pairs.stdout" 2> "$test_dir/pairs.stderr"
"$dfs_anagrams" "$index_file" abcd -m 2 -n 10 --max-extract-words 2 \
  > "$test_dir/extract-two.stdout" 2> "$test_dir/extract-two.stderr"
"$dfs_anagrams" "$index_file" abcd -m 2 -n 10 -x 1 \
  > "$test_dir/extract-one.stdout" 2> "$test_dir/extract-one.stderr"
cmp "$test_dir/pairs.stdout" "$test_dir/extract-two.stdout" ||
  fail "--pairs differs from --max-extract-words 2"
cmp "$test_dir/all.stdout" "$test_dir/pairs.stdout" ||
  fail "--pairs changed stdout where no entry holds three words"
[[ $(grep -c '^70.00 ab cd$' "$test_dir/extract-one.stdout") -eq 0 ]] ||
  fail "-x 1 kept the two-word index entry"
grep -Eq "${diagnostic_prefix}at most 1 word per index entry$" \
  "$test_dir/extract-one.stderr" ||
  fail "--max-extract-words diagnostic is missing from stderr"

# Every result line's entry list must be pasteable into "query-index --score"
# and reproduce that line's own score.
while read -r result_score result_entries; do
  round_trip=$("$query_index" "$index_file" "$result_entries" --score |
    awk '{ print $1 }')
  assert_close "$round_trip" "$result_score" \
    "query-index --score disagrees on \"$result_entries\""
done < "$test_dir/all.stdout"

# -g N keeps only N-entry results. "ab cd" wins the unconstrained search as one
# contiguous phrase and absorbs its split form, so -g 2 is what exposes "ab,cd".
"$dfs_anagrams" "$index_file" abcd -m 2 -n 10 -g 1 \
  > "$test_dir/one-segment.stdout" 2> "$test_dir/one-segment.stderr"
"$dfs_anagrams" "$index_file" abcd -m 2 -n 10 -g 2 \
  > "$test_dir/two-segment.stdout" 2> "$test_dir/two-segment.stderr"
if grep -q ',' "$test_dir/one-segment.stdout"; then
  fail "-g 1 returned a multi-entry result"
fi
[[ $(grep -c '^[^ ]* [^,]*,[^,]*$' "$test_dir/two-segment.stdout") \
   -eq $(wc -l < "$test_dir/two-segment.stdout") ]] ||
  fail "-g 2 returned a result that does not use exactly two entries"
[[ $(grep -c '^70.00 ab cd$' "$test_dir/one-segment.stdout") -eq 1 ]] ||
  fail "-g 1 lost the contiguous phrase"
grep -Eq "${diagnostic_prefix}"'4 letters "abcd", words of 2\+, at most 2 words, exactly 1 segment$' \
  "$test_dir/one-segment.stderr" ||
  fail "the search header did not report the segment constraint"
grep -Eq "${diagnostic_prefix}"'.*, exactly 2 segments$' \
  "$test_dir/two-segment.stderr" ||
  fail "the search header did not pluralize the segment constraint"
grep -q ' ab,cd$' "$test_dir/two-segment.stdout" ||
  fail "-g 2 did not expose the split form of the contiguous phrase"
"$dfs_anagrams" "$index_file" abcd -m 2 -n 10 -g 9 \
  > "$test_dir/unreachable-segment.stdout" \
  2> "$test_dir/unreachable-segment.stderr"
[[ ! -s $test_dir/unreachable-segment.stdout ]] ||
  fail "-g beyond the letters' segment limit returned results"

"$dfs_anagrams" "$index_file" abcd -u ab -m 2 -n 10 \
  > "$test_dir/used.stdout" 2> "$test_dir/used.stderr"
"$dfs_anagrams" "$index_file" cd -m 2 -n 10 \
  > "$test_dir/left.stdout" 2> "$test_dir/left.stderr"
cmp "$test_dir/used.stdout" "$test_dir/left.stdout" ||
  fail "--used-letters differs from searching the remaining bag"

expect_status 2 "$dfs_anagrams" "$index_file" 'ab!'
expect_status 2 "$dfs_anagrams" "$index_file" abc -p 0
expect_status 2 "$dfs_anagrams" "$index_file" abc \
  --cache-size nope
expect_status 2 "$dfs_anagrams" "$index_file" abc \
  --preprocess-threads nope
expect_status 2 "$dfs_anagrams" "$index_file" abc \
  --search-threads 0
expect_status 2 "$dfs_anagrams" "$index_file" abc \
  --search-threads nope
expect_status 2 "$dfs_anagrams" "$index_file" abc \
  --projection-depth nope
expect_status 2 "$dfs_anagrams" "$index_file" abc \
  --max-extract-words nope
expect_status 2 "$dfs_anagrams" "$index_file" abc --pairs -x 3
grep -q '^error: --pairs is --max-extract-words 2, not 3$' \
  "$test_dir/status.stderr" ||
  fail "conflicting --pairs diagnostic is unclear"
expect_status 2 "$dfs_anagrams" "$index_file" abc -P 0
grep -q '^error: --segment-penalty must be at least 1$' \
  "$test_dir/status.stderr" ||
  fail "zero segment-penalty diagnostic is unclear"
expect_status 2 "$dfs_anagrams" "$index_file" abc \
  --segment-penalty 0.5
expect_status 2 "$dfs_anagrams" "$index_file" abc -P nope
expect_status 2 "$dfs_anagrams" "$index_file" abc -P inf
expect_status 2 "$dfs_anagrams" "$index_file" abcd -m 2 -n 10 \
  --cache-size 0
grep -q '^error: projected dense score table requires at least 1 MiB; supplied cache is 0 MiB$' \
  "$test_dir/status.stderr" ||
  fail "undersized projected-cache diagnostic is missing"
grep -q '^       use -C 1 or --allow-cache-fallback$' \
  "$test_dir/status.stderr" ||
  fail "projected-cache recovery diagnostic is missing"
expect_status 1 "$dfs_anagrams" "$test_dir/missing.index" abcd
