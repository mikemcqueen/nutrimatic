#!/usr/bin/env bash
set -euo pipefail

dfs_anagrams=$1
make_index=$2
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
default_split_score=$(awk '$2 == "ab" && $3 == "dc" { print $1 }' \
  "$test_dir/all.stdout")
penalty_one_split_score=$(awk '$2 == "ab" && $3 == "dc" { print $1 }' \
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
