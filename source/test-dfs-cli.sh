#!/usr/bin/env bash
set -euo pipefail

dfs_anagrams=$1
make_index=$2
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-dfs-cli.XXXXXX")
index_file=$test_dir/test.index

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

"$make_index" "$index_file"

"$dfs_anagrams" "$index_file" abcd -m 2 -n 10 \
  > "$test_dir/all.stdout" 2> "$test_dir/all.stderr"
"$dfs_anagrams" "$index_file" abcd -m 2 -n 10 \
  --candidate-cache-mib 0 \
  > "$test_dir/uncached.stdout" 2> "$test_dir/uncached.stderr"
cmp "$test_dir/all.stdout" "$test_dir/uncached.stdout" ||
  fail "candidate cache changed stdout"
[[ $(wc -l < "$test_dir/all.stdout") -eq 4 ]] ||
  fail "full synthetic search did not print four word sets"
[[ $(grep -c '^70.00 ab cd$' "$test_dir/all.stdout") -eq 1 ]] ||
  fail "the contiguous phrase did not win and deduplicate its split form"
if grep -q '^# ' "$test_dir/all.stdout"; then
  fail "progress leaked onto stdout"
fi
grep -q '^# 4 letters "abcd", words of 2+, at most 2 words$' \
  "$test_dir/all.stderr" ||
  fail "search header is missing from stderr"
grep -q '^# phase 1 complete:' "$test_dir/all.stderr" ||
  fail "phase-1 statistics are missing from stderr"
grep -Eq '^# phase 2: precomputed [0-9]+ bounded states in [0-9.]+s$' \
  "$test_dir/all.stderr" ||
  fail "phase-2 precompute timing is missing from stderr"
grep -q '^# phase 2 complete:' "$test_dir/all.stderr" ||
  fail "phase-2 statistics are missing from stderr"
grep -Eq '^# phase 2 timing: [0-9.]+ s setup, [0-9.]+ s search, [0-9]+ successful bound transitions, [0-9]+ nextafter calls$' \
  "$test_dir/all.stderr" ||
  fail "phase-2 timing or transition statistics are missing from stderr"
grep -Eq '^[#] phase 2 complete: .* [0-9]+ retained$' \
  "$test_dir/all.stderr" ||
  fail "phase-2 completion statistics have the wrong format"

"$dfs_anagrams" "$index_file" abcd -m 2 -n 2 \
  > "$test_dir/top.stdout" 2> "$test_dir/top.stderr"
head -n 2 "$test_dir/all.stdout" > "$test_dir/expected-top.stdout"
cmp "$test_dir/expected-top.stdout" "$test_dir/top.stdout" ||
  fail "--top did not retain the two highest-scoring word sets"

"$dfs_anagrams" "$index_file" abcd -u ab -m 2 -n 10 \
  > "$test_dir/used.stdout" 2> "$test_dir/used.stderr"
"$dfs_anagrams" "$index_file" cd -m 2 -n 10 \
  > "$test_dir/left.stdout" 2> "$test_dir/left.stderr"
cmp "$test_dir/used.stdout" "$test_dir/left.stdout" ||
  fail "--used-letters differs from searching the remaining bag"

expect_status 2 "$dfs_anagrams" "$index_file" 'ab!'
expect_status 2 "$dfs_anagrams" "$index_file" abc -p 0
expect_status 2 "$dfs_anagrams" "$index_file" abc \
  --candidate-cache-mib nope
expect_status 1 "$dfs_anagrams" "$test_dir/missing.index" abcd
