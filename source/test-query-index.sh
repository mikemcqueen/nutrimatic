#!/usr/bin/env bash
set -euo pipefail

query_index=$1

if [[ -z ${IDX:-} ]]; then
  echo "SKIP: export IDX to run the query-index CLI test" >&2
  exit 77
fi

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
